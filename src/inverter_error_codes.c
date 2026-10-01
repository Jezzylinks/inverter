#include "system/inverter_error_codes.h"

uint16_t inverter_error_code_for_protection(protection_quantity_t quantity,
                                            protection_action_t action,
                                            float value)
{
    (void)action;

    switch (quantity)
    {
    case PROT_QUANTITY_BATTERY_VOLTAGE:
        /* Preserve the established startup diagnostic codes so a battery
         * problem has one identity whether found before or during runtime. */
        return (value < 0.0f) ? INVERTER_START_ERROR_BATTERY_UNDERVOLTAGE
                              : 0U;

    case PROT_QUANTITY_AC_VOLTAGE:
        return INVERTER_RUNTIME_ERROR_AC_VOLTAGE;

    case PROT_QUANTITY_OUTPUT_CURRENT:
        return INVERTER_RUNTIME_ERROR_OVERLOAD;

    case PROT_QUANTITY_TEMPERATURE:
        return INVERTER_RUNTIME_ERROR_OVER_TEMP;

    default:
        return INVERTER_RUNTIME_ERROR_SYSTEM;
    }
}

const char *inverter_error_name(uint16_t code)
{
    switch (code)
    {
    case INVERTER_START_ERROR_BATTERY_UNDERVOLTAGE:
        return "Battery Undervoltage";
    case INVERTER_START_ERROR_BATTERY_OVERVOLTAGE:
        return "Battery Overvoltage";
    case INVERTER_RUNTIME_ERROR_AC_VOLTAGE:
        return "AC Voltage Fault";
    case INVERTER_RUNTIME_ERROR_OVERLOAD:
        return "Output Overload";
    case INVERTER_RUNTIME_ERROR_OVER_TEMP:
        return "Overtemperature";
    case INVERTER_RUNTIME_ERROR_FAN:
        return "Fan Failure";
    case INVERTER_RUNTIME_ERROR_SYSTEM:
        return "System Fault";
    default:
        return "Inverter Fault";
    }
}

const char *inverter_error_action(uint16_t code)
{
    switch (code)
    {
    case INVERTER_START_ERROR_BATTERY_UNDERVOLTAGE:
        return "Output inhibited";
    case INVERTER_START_ERROR_BATTERY_OVERVOLTAGE:
        return "Output inhibited";
    case INVERTER_RUNTIME_ERROR_OVERLOAD:
        return "Output disabled";
    case INVERTER_RUNTIME_ERROR_OVER_TEMP:
        return "Output disabled";
    case INVERTER_RUNTIME_ERROR_AC_VOLTAGE:
        return "Output disabled";
    case INVERTER_RUNTIME_ERROR_FAN:
        return "Output disabled";
    default:
        return "Check system";
    }
}

const char *inverter_error_clear_instruction(uint16_t code)
{
    switch (code)
    {
    case INVERTER_START_ERROR_BATTERY_UNDERVOLTAGE:
        return "Recharge battery";
    case INVERTER_START_ERROR_BATTERY_OVERVOLTAGE:
        return "Check battery";
    case INVERTER_RUNTIME_ERROR_OVERLOAD:
        return "Reduce load";
    case INVERTER_RUNTIME_ERROR_OVER_TEMP:
        return "Allow cooling";
    case INVERTER_RUNTIME_ERROR_AC_VOLTAGE:
        return "Check AC voltage";
    case INVERTER_RUNTIME_ERROR_FAN:
        return "Check fan";
    default:
        return "Check system";
    }
}
