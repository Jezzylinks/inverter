#ifndef CLOUD_REPORTING_H
#define CLOUD_REPORTING_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "system_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOUD_REPORTING_ENDPOINT_MAX 160
#define CLOUD_REPORTING_HARDWARE_ID_MAX 96
#define CLOUD_REPORTING_ENROLLMENT_CODE_MAX 96
#define CLOUD_REPORTING_DEVICE_TOKEN_MAX 96

typedef struct {
    bool enabled;
    char endpoint[CLOUD_REPORTING_ENDPOINT_MAX];
    char hardware_id[CLOUD_REPORTING_HARDWARE_ID_MAX];
    char enrollment_code[CLOUD_REPORTING_ENROLLMENT_CODE_MAX];
    char device_token[CLOUD_REPORTING_DEVICE_TOKEN_MAX];
    uint32_t period_sec;
} cloud_reporting_config_t;

typedef struct {
    bool enabled;
    bool configured;
    bool enrolled;
    bool publish_in_progress;
    uint32_t last_success_ms;
    char last_error[48];
} cloud_reporting_status_t;

esp_err_t cloud_reporting_init(void);
esp_err_t cloud_reporting_get_config(cloud_reporting_config_t *out);
esp_err_t cloud_reporting_set_config(const cloud_reporting_config_t *config);
esp_err_t cloud_reporting_get_status(cloud_reporting_status_t *out);

/* Schedules a bounded HTTPS transaction; it never blocks the ADC loop. */
void cloud_reporting_publish(const system_state_t *state, float solar_power_kw,
                             float load_power_kw, int rssi);

#ifdef __cplusplus
}
#endif

#endif
