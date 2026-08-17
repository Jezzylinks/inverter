#include "ota_manifest.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

static bool parse_release_version(const char *text, uint32_t components[4],
                                  size_t *component_count)
{
    if (!text || !components || !component_count || text[0] == '\0') {
        return false;
    }
    const char *cursor = text;
    if (*cursor == 'v' || *cursor == 'V') {
        ++cursor;
    }
    size_t count = 0U;
    while (*cursor != '\0' && count < 4U) {
        if (!isdigit((unsigned char)*cursor)) {
            return false;
        }
        uint32_t value = 0U;
        do {
            const uint32_t digit = (uint32_t)(*cursor - '0');
            if (value > (UINT32_MAX - digit) / 10U) {
                return false;
            }
            value = value * 10U + digit;
            ++cursor;
        } while (isdigit((unsigned char)*cursor));
        components[count++] = value;
        if (*cursor == '\0') {
            break;
        }
        if (*cursor != '.') {
            return false;
        }
        ++cursor;
        if (*cursor == '\0') {
            return false;
        }
    }
    if (*cursor != '\0' || (count != 3U && count != 4U)) {
        return false;
    }
    *component_count = count;
    return true;
}

bool ota_manifest_is_https_url(const char *url)
{
    return url && strncasecmp(url, "https://", 8) == 0 &&
           strlen(url) < OTA_MAX_URL_LENGTH;
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
        !parse_release_version(installed, installed_parts, &installed_count)) {
        return 0;
    }

    const size_t count = candidate_count > installed_count
                             ? candidate_count : installed_count;
    for (size_t index = 0U; index < count; ++index) {
        const uint32_t candidate_value = index < candidate_count
                                             ? candidate_parts[index] : 0U;
        const uint32_t installed_value = index < installed_count
                                             ? installed_parts[index] : 0U;
        if (candidate_value != installed_value) {
            return candidate_value > installed_value ? 1 : -1;
        }
    }
    return 0;
}

char *ota_manifest_trim(char *text)
{
    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (!text || !*text || !value) {
        return false;
    }
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *ota_manifest_trim(end) != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool is_sha256(const char *text)
{
    if (!text || !*text) {
        return true;
    }
    if (strlen(text) != 64U) {
        return false;
    }
    for (size_t i = 0; i < 64U; ++i) {
        if (!isxdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

esp_err_t ota_manifest_parse_csv(const char *csv, size_t csv_len,
                                 ota_manifest_entry_t *entry)
{
    if (!csv || csv_len == 0U || csv_len > OTA_MAX_MANIFEST_BYTES || !entry) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(entry, 0, sizeof(*entry));

    char *copy = calloc(1, csv_len + 1U);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, csv, csv_len);
    copy[csv_len] = '\0';

    char *save_line = NULL;
    char *line = strtok_r(copy, "\n", &save_line);
    while (line) {
        char *current = ota_manifest_trim(line);
        if (*current == '\0' || *current == '#') {
            line = strtok_r(NULL, "\n", &save_line);
            continue;
        }

        char *fields[4] = {0};
        char *save_field = NULL;
        size_t field_count = 0U;
        char *field = strtok_r(current, ",", &save_field);
        while (field && field_count < 4U) {
            fields[field_count++] = ota_manifest_trim(field);
            field = strtok_r(NULL, ",", &save_field);
        }
        if (field != NULL || field_count < 2U) {
            free(copy);
            return ESP_ERR_INVALID_SIZE;
        }
        if (strcasecmp(fields[0], "version") == 0) {
            line = strtok_r(NULL, "\n", &save_line);
            continue;
        }
        if (field_count < 4U ||
            strlen(fields[0]) >= OTA_MAX_VERSION_LENGTH ||
            strlen(fields[1]) >= OTA_MAX_URL_LENGTH ||
            !ota_manifest_is_https_url(fields[1]) ||
            !ota_manifest_version_is_valid(fields[0]) ||
            !is_sha256(fields[2]) || fields[2][0] == '\0') {
            free(copy);
            return ESP_ERR_INVALID_ARG;
        }

        strncpy(entry->version, fields[0], sizeof(entry->version) - 1U);
        strncpy(entry->url, fields[1], sizeof(entry->url) - 1U);
        if (field_count >= 3U && fields[2][0] != '\0') {
            strncpy(entry->sha256, fields[2], sizeof(entry->sha256) - 1U);
        }
        if (fields[3][0] == '\0' ||
            !parse_u32(fields[3], &entry->image_size) ||
            entry->image_size == 0U) {
            free(copy);
            return ESP_ERR_INVALID_ARG;
        }
        free(copy);
        return ESP_OK;
    }

    free(copy);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ota_manifest_fetch_text(const char *url, char *buffer, size_t capacity,
                                  size_t *out_len, ota_manifest_cancel_cb_t cancel_cb,
                                  void *context)
{
    if (!ota_manifest_is_https_url(url) || !buffer || capacity < 2U || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        const int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length > (int64_t)(capacity - 1U)) {
            err = ESP_ERR_INVALID_SIZE;
        } else if (esp_http_client_get_status_code(client) != 200) {
            err = ESP_FAIL;
        } else {
            bool reached_eof = false;
            while (*out_len < capacity - 1U) {
                if (cancel_cb && cancel_cb(context)) {
                    err = ESP_ERR_INVALID_STATE;
                    break;
                }
                const int read_len = esp_http_client_read(client, buffer + *out_len,
                                                          capacity - 1U - *out_len);
                if (read_len < 0) {
                    err = ESP_FAIL;
                    break;
                }
                if (read_len == 0) {
                    reached_eof = true;
                    break;
                }
                *out_len += (size_t)read_len;
            }
            if (err == ESP_OK && !reached_eof && *out_len >= capacity - 1U) {
                err = ESP_ERR_INVALID_SIZE;
            }
            buffer[*out_len] = '\0';
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t ota_manifest_fetch_and_parse(const char *csv_url,
                                       ota_manifest_entry_t *entry,
                                       ota_manifest_cancel_cb_t cancel_cb,
                                       void *context)
{
    if (!entry) {
        return ESP_ERR_INVALID_ARG;
    }
    char manifest[OTA_MAX_MANIFEST_BYTES + 1U] = {0};
    size_t manifest_len = 0U;
    esp_err_t err = ota_manifest_fetch_text(csv_url, manifest, sizeof(manifest),
                                            &manifest_len, cancel_cb, context);
    if (err != ESP_OK) {
        return err;
    }
    if (cancel_cb && cancel_cb(context)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ota_manifest_parse_csv(manifest, manifest_len, entry);
}
