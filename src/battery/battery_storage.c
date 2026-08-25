#include "battery/battery_storage.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define BATTERY_STORAGE_NAMESPACE "battery"
#define BATTERY_STORAGE_KEY "state"
#define TAG "BATTERY_STORAGE"

static bool battery_storage_data_valid(const battery_storage_data_t *data)
{
    if (!data || data->version != BATTERY_STORAGE_VERSION) {
        return false;
    }
    if (!isfinite(data->soc) || !isfinite(data->soh) ||
        data->soc < 0.0f || data->soc > 100.0f ||
        data->soh < 0.0f || data->soh > 100.0f ||
        !isfinite(data->measured_capacity_ah) ||
        !isfinite(data->rated_capacity_ah) ||
        data->measured_capacity_ah < 0.0f || data->rated_capacity_ah <= 0.0f ||
        data->measured_capacity_ah > 1000000.0f || data->rated_capacity_ah > 1000000.0f ||
        data->chemistry > 31U) {
        return false;
    }
    return true;
}

bool battery_storage_save(const battery_storage_data_t *data)
{
    if (!battery_storage_data_valid(data)) {
        ESP_LOGW(TAG, "Refusing to persist invalid battery state");
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(BATTERY_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_set_blob(handle, BATTERY_STORAGE_KEY, data, sizeof(*data));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool battery_storage_load(battery_storage_data_t *data)
{
    if (!data) {
        return false;
    }
    memset(data, 0, sizeof(*data));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(BATTERY_STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }
    size_t required_size = sizeof(*data);
    err = nvs_get_blob(handle, BATTERY_STORAGE_KEY, data, &required_size);
    nvs_close(handle);

    if (err != ESP_OK || required_size != sizeof(*data) || !battery_storage_data_valid(data)) {
        memset(data, 0, sizeof(*data));
        return false;
    }
    return true;
}

bool battery_storage_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BATTERY_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_erase_key(handle, BATTERY_STORAGE_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}
