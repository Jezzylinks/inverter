#ifndef INVERTER_ADC_CONFIG_H
#define INVERTER_ADC_CONFIG_H

/*
 * ADC acquisition backend selected at compile time. Exactly one value is
 * selected by INVERTER_ADC_MODE; the application never branches on backend
 * details after this header is included.
 */
#define INVERTER_ADC_MODE_CONTINUOUS 0
#define INVERTER_ADC_MODE_ONESHOT    1

#ifndef INVERTER_ADC_MODE
#define INVERTER_ADC_MODE INVERTER_ADC_MODE_CONTINUOUS
#endif

#if INVERTER_ADC_MODE != INVERTER_ADC_MODE_CONTINUOUS && \
    INVERTER_ADC_MODE != INVERTER_ADC_MODE_ONESHOT
#error "INVERTER_ADC_MODE must be CONTINUOUS (0) or ONESHOT (1)"
#endif

typedef enum
{
    INVERTER_ADC_MODE_CONTINUOUS_ENUM = INVERTER_ADC_MODE_CONTINUOUS,
    INVERTER_ADC_MODE_ONESHOT_ENUM = INVERTER_ADC_MODE_ONESHOT
} inverter_adc_mode_t;

#endif /* INVERTER_ADC_CONFIG_H */
