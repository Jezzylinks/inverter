#ifndef POST_MANAGER_H
#define POST_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POST_FAILURE_LCD (1U << 0)
#define POST_FAILURE_FAN (1U << 1)
#define POST_FAILURE_ADC (1U << 2)

typedef struct {
    bool lcd_ok;
    bool fan_ok;
    bool adc_ok;
    bool all_passed;
    uint32_t failure_mask;
    uint32_t duration_ms;
} post_result_t;

/**
 * Run all power-on self-tests. Call only after ADC sampling is live and from
 * a task other than adc_task. Every component is tested even after a failure.
 */
post_result_t post_run_all(void);
post_result_t post_get_last_result(void);

#ifdef __cplusplus
}
#endif

#endif /* POST_MANAGER_H */
