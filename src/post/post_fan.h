#ifndef POST_FAN_H
#define POST_FAN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Power-On Self-Test for the cooling fan.
     *
     * This inverter has no tachometer input -- fan speed is sensed as an
     * analog voltage on ADC_FAN (see adc_configs[] in main.c), already
     * continuously read and written to sys_state.fan.speed by adc_task.
     * The test drives GPIO_FAN_TEST high, waits for the fan to spin up
     * and for adc_task (running concurrently -- this must NOT be called
     * from adc_task's own context, or sys_state.fan.speed will never be
     * refreshed during the wait) to report a fresh reading, then checks
     * it against FAN_SPEED_THRESHOLD before turning the fan back off.
     *
     * @return true if fan speed rose above FAN_SPEED_THRESHOLD.
     * @return false otherwise (fan not spinning, wiring fault, or the
     *         GPIO/ADC pairing is disconnected).
     */
    bool post_fan_test(void);

#ifdef __cplusplus
}
#endif

#endif /* POST_FAN_H */
