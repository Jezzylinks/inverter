/******************************************************************************
 * @file battery_storage.c
 * @brief Battery persistent storage using ESP-IDF NVS
 ******************************************************************************/

#include "battery_storage.h"

#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

#define BATTERY_STORAGE_NAMESPACE "battery"
#define BATTERY_STORAGE_KEY "state"

/******************************************************************************
 * Save Battery Data
 ******************************************************************************/

bool battery_storage_save(
    const battery_storage_data_t *data)
{
    if (data == NULL)
    {
        return false;
    }

    nvs_handle_t handle;

    esp_err_t err = nvs_open(
        BATTERY_STORAGE_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        return false;
    }

    err = nvs_set_blob(
        handle,
        BATTERY_STORAGE_KEY,
        data,
        sizeof(battery_storage_data_t));

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return false;
    }

    err = nvs_commit(handle);

    nvs_close(handle);

    return (err == ESP_OK);
}

/******************************************************************************
 * Load Battery Data
 ******************************************************************************/

bool battery_storage_load(
    battery_storage_data_t *data)
{
    if (data == NULL)
    {
        return false;
    }

    memset(data, 0, sizeof(*data));

    nvs_handle_t handle;

    esp_err_t err = nvs_open(
        BATTERY_STORAGE_NAMESPACE,
        NVS_READONLY,
        &handle);

    if (err != ESP_OK)
    {
        return false;
    }

    size_t required_size =
        sizeof(battery_storage_data_t);

    err = nvs_get_blob(
        handle,
        BATTERY_STORAGE_KEY,
        data,
        &required_size);

    nvs_close(handle);

    if (err != ESP_OK)
    {
        return false;
    }

    /* Validate version */
    if (data->version != BATTERY_STORAGE_VERSION)
    {
        memset(data, 0, sizeof(*data));
        return false;
    }

    return true;
}

/******************************************************************************
 * Erase Battery Data
 ******************************************************************************/

bool battery_storage_erase(void)
{
    nvs_handle_t handle;

    esp_err_t err = nvs_open(
        BATTERY_STORAGE_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        return false;
    }

    err = nvs_erase_key(
        handle,
        BATTERY_STORAGE_KEY);

    if (err != ESP_OK)
    {
        nvs_close(handle);
        return false;
    }

    err = nvs_commit(handle);

    nvs_close(handle);

    return (err == ESP_OK);
}