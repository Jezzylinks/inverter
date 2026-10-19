#ifndef STACK_REPORT_H
#define STACK_REPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Log, for every known task, how much of its stack was NEVER used (the
 * high-water mark: smaller = closer to overflow) plus current/minimum free
 * heap. Use it once the firmware has been running through its normal
 * activities (menus, Wi-Fi, an OTA check) and size each stack to
 * (stack_size - min_free) + about 25 % headroom. Safe to call from any task. */
void stack_report_log(const char *stage);

#ifdef __cplusplus
}
#endif

#endif /* STACK_REPORT_H */
