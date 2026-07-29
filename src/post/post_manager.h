#ifndef POST_MANAGER_H
#define POST_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        bool lcd_ok;
        bool fan_ok;
        bool adc_ok;
        bool all_passed;
    } post_result_t;

    /**
     * @brief Run every Power-On Self-Test in order and collect the results.
     *
     * Order matters: LCD is tested first, since a failure there means
     * nothing else can be shown on screen (the caller should fall back
     * to logging only). ADC and fan are tested next -- call this only
     * after adc_task is already running and has taken at least one real
     * sample, and call it from a task OTHER than adc_task itself, or
     * sys_state.fan.speed will never refresh during post_fan_test()'s
     * wait.
     *
     * A failing test does not stop the remaining tests from running --
     * all results are collected so a technician gets the full picture
     * from one boot.
     *
     * @return post_result_t with each test's pass/fail and an overall
     *         all_passed flag.
     */
    post_result_t post_run_all(void);

    /**
     * @brief Result of the most recent post_run_all() call.
     *
     * Exposed so other code (e.g. check_safety_conditions()) can factor
     * POST results into whether it's safe to power the inverter on,
     * without re-running the tests.
     */
    post_result_t post_get_last_result(void);

#ifdef __cplusplus
}
#endif

#endif /* POST_MANAGER_H */
