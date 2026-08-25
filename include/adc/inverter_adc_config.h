#ifndef INVERTER_ADC_CONFIG_H
#define INVERTER_ADC_CONFIG_H

#include "sdkconfig.h"

/* The choice is defined in src/Kconfig.projbuild. menuconfig generates
 * exactly one of these CONFIG_ symbols. Do not select ADC mode with ad-hoc
 * source edits or PlatformIO build flags. */
#if defined(CONFIG_INVERTER_ADC_MODE_CONTINUOUS) && \
    defined(CONFIG_INVERTER_ADC_MODE_ONESHOT)
#error "ADC menuconfig selected both acquisition modes"
#elif defined(CONFIG_INVERTER_ADC_MODE_CONTINUOUS)
#define INVERTER_ADC_MODE_CONTINUOUS 0
#define INVERTER_ADC_MODE_ONESHOT    1
#define INVERTER_ADC_MODE            INVERTER_ADC_MODE_CONTINUOUS
#elif defined(CONFIG_INVERTER_ADC_MODE_ONESHOT)
#define INVERTER_ADC_MODE_CONTINUOUS 0
#define INVERTER_ADC_MODE_ONESHOT    1
#define INVERTER_ADC_MODE            INVERTER_ADC_MODE_ONESHOT
#else
#error "Select an ADC acquisition mode with idf.py menuconfig"
#endif

typedef enum
{
    INVERTER_ADC_MODE_CONTINUOUS_ENUM = INVERTER_ADC_MODE_CONTINUOUS,
    INVERTER_ADC_MODE_ONESHOT_ENUM = INVERTER_ADC_MODE_ONESHOT
} inverter_adc_mode_t;

#endif /* INVERTER_ADC_CONFIG_H */
