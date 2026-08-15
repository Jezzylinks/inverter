#include "pin_entry.h"

#include <string.h>
#include <stdio.h>
#include "button_controller.h"
#include "factory_reset.h"
#include "lcd_state.h"
#include "system_state.h"
#include "lcd_flash_queue.h"
#include "esp_log.h"

extern lcd_render_state_t sys_lcd;
extern system_state_t sys_state;

void pin_entry_reset(pin_entry_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    // digit[] and confirmed[] already zeroed; cursor starts at 0.
}

pin_entry_result_t pin_entry_handle_button(pin_entry_ctx_t *ctx, button_id_t btn)
{
    switch (btn)
    {

    case BTN_UP:
        ctx->digit[ctx->cursor] = (ctx->digit[ctx->cursor] + 1) % 10;
        return PIN_ENTRY_IN_PROGRESS;

    case BTN_DOWN:
        ctx->digit[ctx->cursor] = (ctx->digit[ctx->cursor] + 9) % 10; // -1 mod 10
        return PIN_ENTRY_IN_PROGRESS;

    case BTN_ENTER:
        ctx->confirmed[ctx->cursor] = true;
        if (ctx->cursor == SECURITY_PIN_LEN - 1)
        {
            // last digit confirmed -> full PIN submitted
            return PIN_ENTRY_SUBMITTED;
        }
        ctx->cursor++;
        return PIN_ENTRY_IN_PROGRESS;

    case BTN_BACK:
        if (ctx->cursor == 0)
        {
            // nothing left to step back from -> cancel whole entry
            return PIN_ENTRY_CANCELLED;
        }
        ctx->confirmed[ctx->cursor] = false;
        ctx->cursor--;
        ctx->confirmed[ctx->cursor] = false; // re-open previous digit for editing
        return PIN_ENTRY_IN_PROGRESS;

    default:
        // Any other button (e.g. a menu/select button not relevant here) is ignored.
        return PIN_ENTRY_IN_PROGRESS;
    }
}

void pin_entry_get_pin(const pin_entry_ctx_t *ctx, uint8_t out_pin[SECURITY_PIN_LEN])
{
    memcpy(out_pin, ctx->digit, SECURITY_PIN_LEN);
}

void pin_entry_render_line(const pin_entry_ctx_t *ctx, char *buf, size_t buf_len)
{
    // Render each PIN position in a bracketed slot. Confirmed digits are
    // masked, while the digit currently being adjusted remains visible.
    char tmp[SECURITY_PIN_LEN][2];
    for (int i = 0; i < SECURITY_PIN_LEN; i++)
    {
        if (i == ctx->cursor && !ctx->confirmed[i])
        {
            tmp[i][0] = (char)('0' + ctx->digit[i]);
        }
        else if (ctx->confirmed[i])
        {
            tmp[i][0] = '*';
        }
        else
        {
            tmp[i][0] = '_';
        }
        tmp[i][1] = '\0';
    }

    snprintf(buf, buf_len, "[%s] [%s] [%s] [%s]",
             tmp[0], tmp[1], tmp[2], tmp[3]);
}