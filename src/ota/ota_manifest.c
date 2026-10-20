#include "ota/ota_manifest.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "server/ntp/ntp_client.h"
#include "utility/quiet_hours.h"

static const char *TAG = "OTA_TLS_DIAG";

/* 2024-01-01 UTC: anything earlier means the RTC was never set by SNTP. */
#define OTA_MIN_VALID_EPOCH 1704067200
#define OTA_TIME_SYNC_WAIT_MS 5000U
#define OTA_TIME_SYNC_POLL_MS 500U
/* A TLS handshake needs roughly 40 KB of heap and, within it, one block big
 * enough for the 16 KB record buffer when dynamic buffers are off. */
#define OTA_TLS_MIN_LARGEST_BLOCK 20000U
/* Room for the redirect URL copies plus a new TLS session. */
#define OTA_REDIRECT_MIN_LARGEST_BLOCK 6000U
/* Total free heap needed to finish a handshake whose certificate chain is
 * verified against the bundle. Measured on the device: a handshake started
 * with ~37 KB free succeeded, one started with ~25.7 KB failed mid-verify with
 * "alloc failed" -> ESP_ERR_HTTP_CONNECT. Override with a build flag to tune. */
#ifndef OTA_TLS_MIN_FREE_HEAP
#define OTA_TLS_MIN_FREE_HEAP 34000U
#endif
/* Give freed memory (task teardown, delayed frees) a moment to come back
 * before giving up. */
#define OTA_TLS_HEAP_WAIT_MS 4000U
#define OTA_TLS_HEAP_POLL_MS 250U

/*
 * Runs once, only when the clock never got set. Separates the usual causes:
 * DNS cannot resolve the NTP server (lookup FAILED), versus DNS fine but no
 * reply (UDP port 123 blocked by the network, sync status still RESET).
 */
static void ota_log_ntp_diagnostics(void)
{
    const char *const hosts[] = {NTP_DEFAULT_SERVER, NTP_FALLBACK_SERVER};
    for (size_t i = 0U; i < sizeof(hosts) / sizeof(hosts[0]); ++i)
    {
        struct addrinfo hints = {0};
        struct addrinfo *result = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        const int rc = getaddrinfo(hosts[i], "123", &hints, &result);
        ESP_LOGW(TAG, "NTP diag: DNS lookup of %s %s (rc=%d)", hosts[i],
                 (rc == 0 && result != NULL) ? "ok" : "FAILED", rc);
        if (result != NULL)
        {
            freeaddrinfo(result);
        }
    }
    ESP_LOGW(TAG, "NTP diag: sntp_enabled=%d sync_status=%d (0=reset 1=completed 2=in-progress)",
             (int)esp_sntp_enabled(), (int)sntp_get_sync_status());
}

/*
 * Certificate validity is checked against the system clock, so a handshake
 * made before SNTP has set it fails as "certificate not yet valid" and
 * surfaces as ESP_ERR_HTTP_CONNECT. Give SNTP a bounded chance to finish.
 * Never blocks forever: on timeout the attempt proceeds so the real TLS error
 * is still logged.
 */
static void ota_wait_for_valid_time(void)
{
    /* The certificate bundle validated GitHub's chain on a device whose clock
     * was never set, so a missing clock is not fatal. Wait at most once per
     * boot so every check/install does not add another delay. */
    static bool s_wait_already_timed_out;
    if (time(NULL) >= OTA_MIN_VALID_EPOCH || s_wait_already_timed_out)
    {
        return;
    }
    ESP_LOGW(TAG, "System clock not set; waiting up to %u ms for SNTP before TLS",
             (unsigned)OTA_TIME_SYNC_WAIT_MS);
    quiet_hours_request_sync();
    for (uint32_t waited = 0U; waited < OTA_TIME_SYNC_WAIT_MS;
         waited += OTA_TIME_SYNC_POLL_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(OTA_TIME_SYNC_POLL_MS));
        if (time(NULL) >= OTA_MIN_VALID_EPOCH)
        {
            ESP_LOGI(TAG, "SNTP time available after %u ms", (unsigned)waited);
            return;
        }
    }
    s_wait_already_timed_out = true;
    ESP_LOGE(TAG, "SNTP did not set the clock; TLS certificate dates cannot be verified");
    ota_log_ntp_diagnostics();
}

static bool ota_tls_heap_ok(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_8BIT) >= OTA_TLS_MIN_FREE_HEAP &&
           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >=
               OTA_TLS_MIN_LARGEST_BLOCK;
}

/*
 * Refuse to start a handshake that cannot finish. Running out of memory inside
 * mbedTLS surfaces as a certificate error and ESP_ERR_HTTP_CONNECT, which sends
 * the debugging in the wrong direction; ESP_ERR_NO_MEM says what happened.
 */
static esp_err_t ota_wait_for_tls_heap(unsigned hop)
{
    uint32_t waited_ms = 0U;
    while (!ota_tls_heap_ok() && waited_ms < OTA_TLS_HEAP_WAIT_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(OTA_TLS_HEAP_POLL_MS));
        waited_ms += OTA_TLS_HEAP_POLL_MS;
    }
    if (!ota_tls_heap_ok())
    {
        ESP_LOGE(TAG,
                 "Not enough heap for a TLS handshake at hop=%u: free=%u (need %u) "
                 "largest_block=%u (need %u)",
                 hop,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)OTA_TLS_MIN_FREE_HEAP,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)OTA_TLS_MIN_LARGEST_BLOCK);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void ota_log_heap_before_tls(void)
{
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Heap before TLS: free=%u largest_block=%u",
             (unsigned)free_heap, (unsigned)largest);
    if (largest < OTA_TLS_MIN_LARGEST_BLOCK)
    {
        ESP_LOGW(TAG, "Largest free block is below %u bytes; TLS allocation may fail",
                 (unsigned)OTA_TLS_MIN_LARGEST_BLOCK);
    }
}

/*
 * Log only the scheme and host: redirected GitHub asset URLs can contain
 * time-limited signed query parameters that must not be printed to UART.
 */
static void ota_log_current_endpoint(esp_http_client_handle_t client,
                                     unsigned hop)
{
    /* Signed redirect URLs are long; keep the buffer off the task stack. */
    enum { OTA_LOG_URL_CAPACITY = 2048 };
    char *url = calloc(1U, OTA_LOG_URL_CAPACITY);
    char host[256] = "unknown";
    char utc[32] = "unavailable";

    if (url != NULL &&
        esp_http_client_get_url(client, url, OTA_LOG_URL_CAPACITY) == ESP_OK)
    {
        const char *scheme_end = strstr(url, "://");
        const char *authority = scheme_end ? scheme_end + 3 : url;
        size_t host_len = strcspn(authority, "/?#:");
        if (host_len > 0U && host_len < sizeof(host))
        {
            memcpy(host, authority, host_len);
            host[host_len] = '\0';
        }
    }
    free(url);

    const time_t now = time(NULL);
    if (now >= OTA_MIN_VALID_EPOCH) /* reject obvious unset RTC time */
    {
        struct tm utc_tm;
        if (gmtime_r(&now, &utc_tm) != NULL)
        {
            (void)strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
        }
    }

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    #if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL
    ESP_LOGI(TAG, "Trust config: certificate bundle enabled, default FULL");
    #elif defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN
    ESP_LOGI(TAG, "Trust config: certificate bundle enabled, default CMN");
    #else
    ESP_LOGI(TAG, "Trust config: certificate bundle enabled, default bundle not identified");
    #endif
#else
    ESP_LOGE(TAG, "Trust config: certificate bundle is NOT enabled in this build");
#endif

    ESP_LOGI(TAG, "HTTPS attempt hop=%u host=%s UTC=%s (epoch=%lld)",
             hop, host, utc, (long long)now);
}

static bool parse_release_version(const char *text, uint32_t components[4],
                                  size_t *component_count)
{
    if (!text || !components || !component_count || text[0] == '\0')
    {
        return false;
    }
    const char *cursor = text;
    if (*cursor == 'v' || *cursor == 'V')
    {
        ++cursor;
    }
    size_t count = 0U;
    while (*cursor != '\0' && count < 4U)
    {
        if (!isdigit((unsigned char)*cursor))
        {
            return false;
        }
        uint32_t value = 0U;
        do
        {
            const uint32_t digit = (uint32_t)(*cursor - '0');
            if (value > (UINT32_MAX - digit) / 10U)
            {
                return false;
            }
            value = value * 10U + digit;
            ++cursor;
        } while (isdigit((unsigned char)*cursor));
        components[count++] = value;
        if (*cursor == '\0')
        {
            break;
        }
        if (*cursor != '.')
        {
            return false;
        }
        ++cursor;
        if (*cursor == '\0')
        {
            return false;
        }
    }
    if (*cursor != '\0' || (count != 3U && count != 4U))
    {
        return false;
    }
    *component_count = count;
    return true;
}

bool ota_manifest_is_https_url(const char *url)
{
    if (url == NULL || strncasecmp(url, "https://", 8U) != 0)
    {
        return false;
    }

    const size_t length = strlen(url);
    if (length <= 8U || length >= OTA_MAX_URL_LENGTH)
    {
        return false;
    }

    /* Require a non-empty authority/host. Reject whitespace, backslashes,
     * and user-info so malformed persisted URLs fail before HTTP setup. */
    const char *authority = url + 8U;
    const char *authority_end = strpbrk(authority, "/?#");
    const size_t authority_length = authority_end != NULL
                                        ? (size_t)(authority_end - authority)
                                        : strlen(authority);
    if (authority_length == 0U)
    {
        return false;
    }
    for (size_t i = 0U; i < authority_length; ++i)
    {
        const unsigned char ch = (unsigned char)authority[i];
        if (isspace(ch) || ch == '\\' || ch == '@')
        {
            return false;
        }
    }
    return true;
}

bool ota_manifest_version_is_valid(const char *text)
{
    uint32_t components[4] = {0};
    size_t count = 0U;
    return parse_release_version(text, components, &count);
}

int ota_manifest_compare_versions(const char *candidate, const char *installed)
{
    uint32_t candidate_parts[4] = {0};
    uint32_t installed_parts[4] = {0};
    size_t candidate_count = 0U;
    size_t installed_count = 0U;
    if (!parse_release_version(candidate, candidate_parts, &candidate_count) ||
        !parse_release_version(installed, installed_parts, &installed_count))
    {
        return 0;
    }

    const size_t count = candidate_count > installed_count
                             ? candidate_count
                             : installed_count;
    for (size_t index = 0U; index < count; ++index)
    {
        const uint32_t candidate_value = index < candidate_count
                                             ? candidate_parts[index]
                                             : 0U;
        const uint32_t installed_value = index < installed_count
                                             ? installed_parts[index]
                                             : 0U;
        if (candidate_value != installed_value)
        {
            return candidate_value > installed_value ? 1 : -1;
        }
    }
    return 0;
}

char *ota_manifest_trim(char *text)
{
    while (*text && isspace((unsigned char)*text))
    {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
    {
        --end;
    }
    *end = '\0';
    return text;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (!text || !*text || !value)
    {
        return false;
    }
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *ota_manifest_trim(end) != '\0' || parsed > UINT32_MAX)
    {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool is_sha256(const char *text)
{
    if (!text || !*text)
    {
        return true;
    }
    if (strlen(text) != 64U)
    {
        return false;
    }
    for (size_t i = 0; i < 64U; ++i)
    {
        if (!isxdigit((unsigned char)text[i]))
        {
            return false;
        }
    }
    return true;
}

esp_err_t ota_manifest_parse_csv(const char *csv, size_t csv_len,
                                 ota_manifest_entry_t *entry)
{
    if (!csv || csv_len == 0U || csv_len > OTA_MAX_MANIFEST_BYTES || !entry)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(entry, 0, sizeof(*entry));

    char *copy = calloc(1, csv_len + 1U);
    if (!copy)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, csv, csv_len);
    copy[csv_len] = '\0';

    char *save_line = NULL;
    char *line = strtok_r(copy, "\n", &save_line);
    while (line)
    {
        char *current = ota_manifest_trim(line);
        if (*current == '\0' || *current == '#')
        {
            line = strtok_r(NULL, "\n", &save_line);
            continue;
        }

        char *fields[4] = {0};
        char *save_field = NULL;
        size_t field_count = 0U;
        char *field = strtok_r(current, ",", &save_field);
        while (field && field_count < 4U)
        {
            fields[field_count++] = ota_manifest_trim(field);
            field = strtok_r(NULL, ",", &save_field);
        }
        if (field != NULL || field_count < 2U)
        {
            free(copy);
            return ESP_ERR_INVALID_SIZE;
        }
        if (strcasecmp(fields[0], "version") == 0)
        {
            line = strtok_r(NULL, "\n", &save_line);
            continue;
        }
        if (field_count < 4U ||
            strlen(fields[0]) >= OTA_MAX_VERSION_LENGTH ||
            strlen(fields[1]) >= OTA_MAX_URL_LENGTH ||
            !ota_manifest_is_https_url(fields[1]) ||
            !ota_manifest_version_is_valid(fields[0]) ||
            !is_sha256(fields[2]) || fields[2][0] == '\0')
        {
            free(copy);
            return ESP_ERR_INVALID_ARG;
        }

        strncpy(entry->version, fields[0], sizeof(entry->version) - 1U);
        strncpy(entry->url, fields[1], sizeof(entry->url) - 1U);
        if (field_count >= 3U && fields[2][0] != '\0')
        {
            strncpy(entry->sha256, fields[2], sizeof(entry->sha256) - 1U);
        }
        if (fields[3][0] == '\0' ||
            !parse_u32(fields[3], &entry->image_size) ||
            entry->image_size == 0U)
        {
            free(copy);
            return ESP_ERR_INVALID_ARG;
        }
        free(copy);
        return ESP_OK;
    }

    free(copy);
    return ESP_ERR_NOT_FOUND;
}

static bool is_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

esp_err_t ota_http_open_following_redirects(esp_http_client_handle_t client,
                                            int64_t *content_length,
                                            int *status_code)
{
    if (client == NULL || content_length == NULL || status_code == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ota_wait_for_valid_time();
    for (unsigned hop = 0U; hop <= OTA_HTTP_MAX_REDIRECTS; ++hop)
    {
        ota_log_current_endpoint(client, hop);
        ota_log_heap_before_tls();
        esp_err_t err = ota_wait_for_tls_heap(hop);
        if (err != ESP_OK)
        {
            return err;
        }
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "HTTPS connect/TLS handshake failed at hop=%u: %s (0x%x)",
                     hop, esp_err_to_name(err), (unsigned)err);
            ota_log_heap_before_tls();
            ESP_LOGE(TAG, "TLS verification remains enabled; inspect preceding esp-tls/mbedTLS logs for certificate failure details");
            return err;
        }
        const int64_t length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (is_redirect_status(status))
        {
            /* esp_http_client_set_redirection() copies the (long, signed)
             * Location URL and asserts on allocation failure instead of
             * returning an error, which reboots the device. Refuse cleanly
             * when memory is too tight to survive that copy. */
            ota_log_heap_before_tls();
            if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) <
                OTA_REDIRECT_MIN_LARGEST_BLOCK)
            {
                ESP_LOGE(TAG, "Not enough free heap to follow redirect at hop=%u", hop);
                esp_http_client_close(client);
                return ESP_ERR_NO_MEM;
            }
            err = esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Redirect processing failed at hop=%u status=%d: %s",
                         hop, status, esp_err_to_name(err));
                return err;
            }
            /* The next iteration logs the new host without signed query data. */
            ESP_LOGI(TAG, "HTTPS redirect hop=%u status=%d; following securely",
                     hop, status);
            /* Never follow a redirect that downgrades to plain HTTP. */
            if (esp_http_client_get_transport_type(client) != HTTP_TRANSPORT_OVER_SSL)
            {
                ESP_LOGE(TAG, "Rejected redirect because it downgraded from HTTPS");
                return ESP_ERR_INVALID_RESPONSE;
            }
            continue;
        }
        if (status == 0 && length < 0)
        {
            esp_http_client_close(client);
            return ESP_FAIL;
        }
        *content_length = length;
        *status_code = status;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE; /* too many redirects */
}

esp_err_t ota_manifest_fetch_text(const char *url, char *buffer, size_t capacity,
                                  size_t *out_len, ota_manifest_cancel_cb_t cancel_cb,
                                  void *context)
{
    if (!ota_manifest_is_https_url(url) || !buffer || capacity < 2U || !out_len)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_HTTP_BUFFER_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        return ESP_ERR_NO_MEM;
    }

    int64_t content_length = 0;
    int status_code = 0;
    esp_err_t err = ota_http_open_following_redirects(client, &content_length,
                                                      &status_code);
    if (err == ESP_OK)
    {
        if (content_length > (int64_t)(capacity - 1U))
        {
            err = ESP_ERR_INVALID_SIZE;
        }
        else if (status_code != 200)
        {
            err = ESP_FAIL;
        }
        else
        {
            bool reached_eof = false;
            while (*out_len < capacity - 1U)
            {
                if (cancel_cb && cancel_cb(context))
                {
                    err = ESP_ERR_INVALID_STATE;
                    break;
                }
                const int read_len = esp_http_client_read(client, buffer + *out_len,
                                                          capacity - 1U - *out_len);
                if (read_len < 0)
                {
                    err = ESP_FAIL;
                    break;
                }
                if (read_len == 0)
                {
                    reached_eof = true;
                    break;
                }
                *out_len += (size_t)read_len;
            }
            if (err == ESP_OK && !reached_eof && *out_len >= capacity - 1U)
            {
                err = ESP_ERR_INVALID_SIZE;
            }
            buffer[*out_len] = '\0';
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t ota_manifest_fetch_and_parse(
    const char *csv_url,
    ota_manifest_entry_t *entry,
    ota_manifest_cancel_cb_t cancel_cb,
    void *context)
{
    if (entry == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char *manifest = calloc(1U, OTA_MAX_MANIFEST_BYTES + 1U);
    if (manifest == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    size_t manifest_len = 0U;
    esp_err_t err = ota_manifest_fetch_text(
        csv_url,
        manifest,
        OTA_MAX_MANIFEST_BYTES + 1U,
        &manifest_len,
        cancel_cb,
        context);

    if (err == ESP_OK)
    {
        if (cancel_cb != NULL && cancel_cb(context))
        {
            err = ESP_ERR_INVALID_STATE;
        }
        else
        {
            err = ota_manifest_parse_csv(
                manifest,
                manifest_len,
                entry);
        }
    }

    free(manifest);
    return err;
}