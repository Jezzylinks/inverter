#ifndef POST_ADC_H
#define POST_ADC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Power-On Self-Test for the ADC-sensed analog inputs.
     *
     * Checks that battery voltage reads within a plausible range for the
     * currently configured chemistry/voltage system (scaled from
     * sys_state.battery_profile, not a fixed 12V-class assumption), and
     * that inverter output voltage is near zero before the inverter has
     * been started.
     *
     * NOTE: unlike battery voltage and output voltage, this inverter has
     * no real current or temperature sensor wired into any ADC channel
     * yet (sys_state.inverter.output_current and .temperature are both
     * permanently pinned at their init values -- see the ERR_OVERLOAD/
     * ERR_OVER_TEMP protection-integration work). Testing those fields
     * would always trivially pass regardless of real conditions, so they
     * are deliberately left out of this POST rather than faked.
     *
     * @return true if all real, sensor-backed checks pass.
     * @return false otherwise.
     */
    bool post_adc_test(void);

#ifdef __cplusplus
}
#endif

#endif /* POST_ADC_H */
