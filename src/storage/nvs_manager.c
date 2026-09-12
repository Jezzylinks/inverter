#include "storage/nvs_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "NVS_MANAGER";
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;
static storage_nvs_state_t s_state = STORAGE_NVS_STATE_UNINITIALIZED;
static esp_err_t s_last_error = ESP_OK;
static unsigned s_recovery_count;
static uint32_t s_schema_version;

#define STORAGE_SCHEMA_NAMESPACE "inv_sys_v2"
#define STORAGE_SCHEMA_KEY "schema_ver"
#define STORAGE_SCHEMA_CURRENT 1U

static esp_err_t initialize_flash_partition(void)
{
    return nvs_flash_init();
}

static esp_err_t lock_storage(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void remember_error(esp_err_t err)
{
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        s_last_error = err;
    }
}

static esp_err_t initialize_schema_locked(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_SCHEMA_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    uint32_t version = 0U;
    err = nvs_get_u32(handle, STORAGE_SCHEMA_KEY, &version);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        version = STORAGE_SCHEMA_CURRENT;
        err = nvs_set_u32(handle, STORAGE_SCHEMA_KEY, version);
        if (err == ESP_OK) err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        s_schema_version = version;
        if (version != STORAGE_SCHEMA_CURRENT) {
            ESP_LOGW(TAG, "Stored schema=%lu differs from current=%u; preserving data for migration",
                     (unsigned long)version, STORAGE_SCHEMA_CURRENT);
        }
    }
    return err;
}

esp_err_t storage_nvs_init(void)
{
    if (s_state == STORAGE_NVS_STATE_READY || s_state == STORAGE_NVS_STATE_RECOVERED) {
        return ESP_OK;
    }
    esp_err_t err = lock_storage();
    if (err != ESP_OK) {
        s_state = STORAGE_NVS_STATE_FAILED;
        s_last_error = err;
        return err;
    }
    if (s_state == STORAGE_NVS_STATE_READY || s_state == STORAGE_NVS_STATE_RECOVERED) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    err = initialize_flash_partition();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS partition recovery needed: %s", esp_err_to_name(err));
        s_state = STORAGE_NVS_STATE_FAILED;
        s_last_error = err;
    } else if (err == ESP_OK) {
        s_state = STORAGE_NVS_STATE_READY;
    }

    if (err == ESP_OK) {
        err = initialize_schema_locked();
    }
    if (err != ESP_OK) {
        s_state = STORAGE_NVS_STATE_FAILED;
        s_last_error = err;
        ESP_LOGE(TAG, "NVS initialization failed: %s (0x%x)", esp_err_to_name(err), err);
    } else {
        s_last_error = ESP_OK;
        ESP_LOGI(TAG, "NVS ready (state=%s)",
                 s_state == STORAGE_NVS_STATE_RECOVERED ? "recovered" : "ready");
    }
    xSemaphoreGive(s_mutex);
    return err;
}

storage_nvs_state_t storage_nvs_state(void) { return s_state; }
bool storage_nvs_is_ready(void)
{
    return s_state == STORAGE_NVS_STATE_READY || s_state == STORAGE_NVS_STATE_RECOVERED;
}
esp_err_t storage_nvs_last_error(void) { return s_last_error; }
unsigned storage_nvs_recovery_count(void) { return s_recovery_count; }
uint32_t storage_nvs_schema_version(void) { return s_schema_version; }

esp_err_t storage_nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                           nvs_handle_t *out_handle)
{
    if (!namespace_name || !out_handle) return ESP_ERR_INVALID_ARG;
    esp_err_t err = storage_nvs_init();
    if (err != ESP_OK) return err;
    err = lock_storage();
    if (err != ESP_OK) return err;
    err = nvs_open(namespace_name, mode, out_handle);
    if (err != ESP_OK) {
        remember_error(err);
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "NVS OPEN FAILED namespace=%s error=%s", namespace_name, esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_nvs_close(nvs_handle_t handle)
{
    nvs_close(handle);
    if (s_mutex) xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t storage_nvs_commit_close(nvs_handle_t handle)
{
    esp_err_t err = nvs_commit(handle);
    if (err != ESP_OK) {
        remember_error(err);
        ESP_LOGE(TAG, "NVS COMMIT FAILED error=%s", esp_err_to_name(err));
    }
    nvs_close(handle);
    if (s_mutex) xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t storage_nvs_get_stats(nvs_stats_t *out_stats)
{
    if (!out_stats) return ESP_ERR_INVALID_ARG;
    esp_err_t err = storage_nvs_init();
    if (err != ESP_OK) return err;
    err = lock_storage();
    if (err == ESP_OK) {
        err = nvs_get_stats(NULL, out_stats);
        if (err != ESP_OK) remember_error(err);
        xSemaphoreGive(s_mutex);
    }
    return err;
}

esp_err_t storage_nvs_erase_namespace(const char *namespace_name)
{
    if (!namespace_name) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = storage_nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = storage_nvs_commit_close(handle);
    else storage_nvs_close(handle);
    return err;
}

esp_err_t storage_nvs_factory_reset(void)
{
    esp_err_t err = storage_nvs_init();
    if (err != ESP_OK) return err;
    err = lock_storage();
    if (err != ESP_OK) return err;
    ESP_LOGW(TAG, "Explicit factory reset: erasing the complete default NVS partition");
    nvs_flash_deinit();
    err = nvs_flash_erase();
    if (err == ESP_OK) err = initialize_flash_partition();
    if (err == ESP_OK) s_state = STORAGE_NVS_STATE_READY;
    else { s_state = STORAGE_NVS_STATE_FAILED; s_last_error = err; }
    xSemaphoreGive(s_mutex);
    return err;
}
