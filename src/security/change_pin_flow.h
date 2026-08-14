#ifndef CHANGE_PIN_FLOW_H
#define CHANGE_PIN_FLOW_H

#include <stdbool.h>
#include "pin_entry.h"
#include "lcd_config.h"
#include "security.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        CHANGE_PIN_IDLE = 0,
        CHANGE_PIN_VERIFY_OLD, // skipped if security_pin_change_required() (forced first-change)
        CHANGE_PIN_ENTER_NEW,
        CHANGE_PIN_CONFIRM_NEW,
        CHANGE_PIN_SUCCESS,  // transient, flash a message then return to CHANGE_PIN_IDLE
        CHANGE_PIN_MISMATCH, // new != confirm; transient, then back to ENTER_NEW
    } change_pin_phase_t;

    typedef enum
    {
        CHANGE_PIN_MODE_SET_NEW = 0,  // normal "Change PIN": verify old, pick + confirm new
        CHANGE_PIN_MODE_RESET_DEFAULT // "Reset PIN": verify old, then jump straight to 0000
                                      // + force-change flag, skipping ENTER_NEW/CONFIRM_NEW
    } change_pin_mode_t;

    typedef struct
    {
        change_pin_phase_t phase;
        change_pin_mode_t mode;
        pin_entry_ctx_t pin_ctx;                  // reused across sub-phases; reset on each transition
        uint8_t new_pin_staged[SECURITY_PIN_LEN]; // held between ENTER_NEW and CONFIRM_NEW
    } change_pin_ctx_t;

    /**
     * Call when the user opens the "Change PIN" settings menu item.
     * Equivalent to change_pin_start_ex(ctx, CHANGE_PIN_MODE_SET_NEW).
     * Skips straight to CHANGE_PIN_ENTER_NEW if a PIN change is currently
     * forced (i.e. still on the factory default), since there's no old PIN
     * worth protecting yet.
     */
    void change_pin_start(change_pin_ctx_t *ctx);

    /**
     * Call when the user opens "Reset PIN" (mode = CHANGE_PIN_MODE_RESET_DEFAULT)
     * or "Change PIN" (mode = CHANGE_PIN_MODE_SET_NEW) from the security menu.
     * Reset mode still requires the current PIN -- resetting without proving
     * you know the existing one would defeat the point of the lock.
     */
    void change_pin_start_ex(change_pin_ctx_t *ctx, change_pin_mode_t mode);

    /**
     * Feed one button press into the flow. Call this from your settings-menu
     * button handler whenever ctx->phase != CHANGE_PIN_IDLE.
     * Returns true once the flow has fully finished (success or user cancel),
     * at which point the caller should return to the parent settings menu.
     */
    bool change_pin_handle_button(change_pin_ctx_t *ctx, button_id_t btn);

    /** Render the two LCD lines for the current phase. */
    void change_pin_render(const change_pin_ctx_t *ctx,
                           char line1[LCD_LINE_SIZE],
                           char line2[LCD_LINE_SIZE]);

#ifdef __cplusplus
}
#endif

#endif // CHANGE_PIN_FLOW_H