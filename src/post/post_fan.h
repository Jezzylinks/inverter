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
     * This is a real 4-wire PWM fan with a tachometer (Yellow wire),
     * driven via fan_driver.c: commands a test PWM duty cycle, lets the
     * fan physically spin up, measures RPM from the tachometer's pulse
     * period (not a fixed on/off GPIO command with no feedback), and
     * confirms it rose above FAN_SPEED_THRESHOLD_RPM before turning the
     * fan back off.
     *
     * Sequence: fan on -> 300ms stabilize -> measure for 500ms -> off.
     *
     * @return true if measured RPM exceeded FAN_SPEED_THRESHOLD_RPM.
     * @return false otherwise (fan not spinning, stalled, or the
     *         PWM/tach wiring is faulty).
     */
    bool post_fan_test(void);

#ifdef __cplusplus
}
#endif

#endif /* POST_FAN_H */
