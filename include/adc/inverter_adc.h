#ifndef INVERTER_ADC_H
#define INVERTER_ADC_H

#ifdef __cplusplus
extern "C" {
#endif

/** Start the inverter-specific ADC task and its telemetry/safety pipeline. */
void adc_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* INVERTER_ADC_H */
