#ifndef ADC_CONFIG_H
#define ADC_CONFIG_H

#include "sdkconfig.h"

/* The choice is defined in src/Kconfig.projbuild. menuconfig generates
 * exactly one of these CONFIG_ symbols. Do not select ADC mode with ad-hoc
 * source edits or PlatformIO build flags. */
#if defined(CONFIG_ADC_MANAGER_MODE_CONTINUOUS) && \
    defined(CONFIG_ADC_MANAGER_MODE_ONESHOT)
#error "ADC menuconfig selected both acquisition modes"
#elif defined(CONFIG_ADC_MANAGER_MODE_CONTINUOUS)
#define ADC_MANAGER_MODE_CONTINUOUS 0
#define ADC_MANAGER_MODE_ONESHOT    1
#define ADC_MANAGER_MODE            ADC_MANAGER_MODE_CONTINUOUS
#elif defined(CONFIG_ADC_MANAGER_MODE_ONESHOT)
#define ADC_MANAGER_MODE_CONTINUOUS 0
#define ADC_MANAGER_MODE_ONESHOT    1
#define ADC_MANAGER_MODE            ADC_MANAGER_MODE_ONESHOT
#else
#error "Select an ADC acquisition mode with idf.py menuconfig"
#endif

typedef enum
{
    ADC_MANAGER_MODE_CONTINUOUS_ENUM = ADC_MANAGER_MODE_CONTINUOUS,
    ADC_MANAGER_MODE_ONESHOT_ENUM = ADC_MANAGER_MODE_ONESHOT
} adc_manager_mode_t;

#endif /* ADC_CONFIG_H */
