#ifndef PIN_ENTRY_H
#define PIN_ENTRY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "security.h" // SECURITY_PIN_LEN

#ifdef __cplusplus
extern "C"
{
#endif

// Reuse whatever button enum the rest of the firmware already uses.
// Adjust this include/typedef to match your actual button_controller header.
#include "button_controller.h" // expected to define BUTTON_UP, BUTTON_DOWN, BUTTON_ENTER, BUTTON_BACK

    typedef enum
    {
        PIN_ENTRY_IN_PROGRESS = 0,
        PIN_ENTRY_SUBMITTED, // all 4 digits confirmed
        PIN_ENTRY_CANCELLED, // Back pressed on the first digit
    } pin_entry_result_t;

    typedef struct
    {
        uint8_t digit[SECURITY_PIN_LEN];  // current value of each digit slot (0-9)
        bool confirmed[SECURITY_PIN_LEN]; // whether Enter has locked in that slot
        uint8_t cursor;                   // index of the digit currently being edited
    } pin_entry_ctx_t;

    /** Reset the context to a blank entry starting at digit 0. */
    void pin_entry_reset(pin_entry_ctx_t *ctx);

    /**
     * Feed one button press into the state machine.
     * BUTTON_UP/BUTTON_DOWN adjust the digit at the cursor (wraps 0-9).
     * BUTTON_ENTER confirms the current digit and advances the cursor.
     * BUTTON_BACK un-confirms and steps back one digit, or cancels entirely
     * if already on the first digit.
     * Any other button id is ignored (returns PIN_ENTRY_IN_PROGRESS unchanged).
     */
    pin_entry_result_t pin_entry_handle_button(pin_entry_ctx_t *ctx, button_id_t btn);

    /** Copy out the entered digits. Only meaningful once result == PIN_ENTRY_SUBMITTED. */
    void pin_entry_get_pin(const pin_entry_ctx_t *ctx, uint8_t out_pin[SECURITY_PIN_LEN]);

    /**
     * Render the second LCD line for display, e.g. "3 * * _" while entering,
     * masking confirmed digits so a PIN isn't left glowing on screen.
     * buf_len should be at least 9 (4 digits + 3 separators + null terminator).
     */
    void pin_entry_render_line(const pin_entry_ctx_t *ctx, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif // PIN_ENTRY_H