#ifndef INVERTER_ERROR_CODES_H
#define INVERTER_ERROR_CODES_H

#include <stdint.h>
#include "security/protection.h"
#include "system/inverter_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical user-facing runtime protection codes.
 * Startup safety codes remain in inverter_start_error_code_t. */
typedef enum {
    INVERTER_RUNTIME_ERROR_AC_VOLTAGE   = 0x301,
    INVERTER_RUNTIME_ERROR_OVERLOAD      = 0x302,
    INVERTER_RUNTIME_ERROR_OVER_TEMP     = 0x303,
    INVERTER_RUNTIME_ERROR_FAN           = 0x305,
    INVERTER_RUNTIME_ERROR_SYSTEM        = 0x306,
} inverter_runtime_error_code_t;

uint16_t inverter_error_code_for_protection(protection_quantity_t quantity,
                                            protection_action_t action,
                                            float value);

const char *inverter_error_name(uint16_t code);
const char *inverter_error_action(uint16_t code);
const char *inverter_error_clear_instruction(uint16_t code);

#ifdef __cplusplus
}
#endif

#endif
