#include "security/change_pin_flow.h"

#include <string.h>
#include <stdio.h>
#include "lcd/lcd_flash_queue.h"
#include "app/button_controller.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "events/system_events.h"

static security_lockout_scope_t lockout_scope_for_context(const change_pin_ctx_t *ctx)
{
    return (ctx && ctx->mode == CHANGE_PIN_MODE_VERIFY_ONLY)
               ? SECURITY_LOCKOUT_OTA
               : SECURITY_LOCKOUT_GENERAL;
}

static void post_security_event(bool success)
{
    system_event_t evt = {0};
    evt.category = EVENT_CATEGORY_SYSTEM;
    evt.action = success ? EVENT_ACTION_SUCCESS : EVENT_ACTION_ERROR;
    evt.source = EVENT_SOURCE_SYSTEM;
    evt.priority = success ? EVENT_PRIORITY_NORMAL : EVENT_PRIORITY_LOW;
    evt.timestamp = xTaskGetTickCount();
    system_event_post(&evt);
}

void change_pin_start(change_pin_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    pin_entry_reset(&ctx->pin_ctx);

    if (security_pin_change_required())
    {
        // Still on the factory default (0000) -- nothing to verify yet,
        // go straight to picking a new one.
        ctx->phase = CHANGE_PIN_ENTER_NEW;
    }
    else
    {
        ctx->phase = CHANGE_PIN_VERIFY_OLD;
    }
}

void change_pin_start_ex(change_pin_ctx_t *ctx, change_pin_mode_t mode)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = mode;
    pin_entry_reset(&ctx->pin_ctx);

    if (security_pin_change_required())
    {
        // Still on the factory default (0000) -- nothing to verify yet.
        // In SET_NEW mode go straight to picking a new one; in
        // RESET_DEFAULT mode there's nothing to reset either, so just
        // treat it as already done.
        ctx->phase = (mode == CHANGE_PIN_MODE_SET_NEW)
                         ? CHANGE_PIN_ENTER_NEW
                         : (mode == CHANGE_PIN_MODE_VERIFY_ONLY
                                ? CHANGE_PIN_VERIFY_OLD
                                : CHANGE_PIN_SUCCESS);
    }
    else
    {
        ctx->phase = CHANGE_PIN_VERIFY_OLD;
    }
}

bool change_pin_handle_button(change_pin_ctx_t *ctx, button_id_t btn)
{
    switch (ctx->phase)
    {

    case CHANGE_PIN_VERIFY_OLD:
    {
        if (security_is_locked_out_for_scope(lockout_scope_for_context(ctx)))
        {
            if (btn == BTN_BACK)
            {
                ctx->phase = CHANGE_PIN_IDLE;
                return true;
            }
            return false;
        }

        pin_entry_result_t result = pin_entry_handle_button(&ctx->pin_ctx, btn);
        if (result == PIN_ENTRY_CANCELLED)
        {
            ctx->phase = CHANGE_PIN_IDLE;
            return true;
        }
        if (result == PIN_ENTRY_SUBMITTED)
        {
            uint8_t entered[SECURITY_PIN_LEN];
            pin_entry_get_pin(&ctx->pin_ctx, entered);

            if (security_verify_pin_for_scope(
                    entered, lockout_scope_for_context(ctx)))
            {
                if (ctx->mode == CHANGE_PIN_MODE_VERIFY_ONLY)
                {
                    post_security_event(true);
                    ctx->phase = CHANGE_PIN_SUCCESS;
                    return true;
                }
                if (ctx->mode == CHANGE_PIN_MODE_RESET_DEFAULT)
                {
                    const uint8_t default_pin[SECURITY_PIN_LEN] = SECURITY_DEFAULT_PIN;
                    esp_err_t err = security_set_pin(default_pin);
                    if (err == ESP_OK)
                    {
                        lcd_flash_info_to("PIN reset", "                ", 1500, LCD_SCREEN_SECURITY);
                        post_security_event(true);
                    }
                    else
                    {
                        lcd_flash_info_to("Reset failed", "                ", 1500, LCD_SCREEN_SECURITY);
                        post_security_event(false);
                    }
                    ctx->phase = CHANGE_PIN_IDLE;
                    return true;
                }
                pin_entry_reset(&ctx->pin_ctx);
                ctx->phase = CHANGE_PIN_ENTER_NEW;
            }
            else if (security_is_locked_out_for_scope(lockout_scope_for_context(ctx)))
            {
                // lcd_flash_enqueue("Locked out", FLASH_PRIORITY_HIGH, FLASH_DURATION_LONG);
                lcd_flash_info_to("PIN LOCKED", "Retry countdown  ", 1200, LCD_SCREEN_SECURITY);
                post_security_event(false);
                /* Keep the verify phase active so the security renderer shows
                 * the live scoped countdown; Back exits the option. */
                ctx->phase = CHANGE_PIN_VERIFY_OLD;
                return false;
            }
            else
            {
                lcd_flash_info_to("Wrong PIN", "Back To Return  ", 4000, LCD_SCREEN_SECURITY);
                post_security_event(false);
                pin_entry_reset(&ctx->pin_ctx); // stay, retry
            }
        }
        return false;
    }

    case CHANGE_PIN_ENTER_NEW:
    {
        pin_entry_result_t result = pin_entry_handle_button(&ctx->pin_ctx, btn);
        if (result == PIN_ENTRY_CANCELLED)
        {
            ctx->phase = CHANGE_PIN_IDLE;
            return true;
        }
        if (result == PIN_ENTRY_SUBMITTED)
        {
            pin_entry_get_pin(&ctx->pin_ctx, ctx->new_pin_staged);
            pin_entry_reset(&ctx->pin_ctx);
            ctx->phase = CHANGE_PIN_CONFIRM_NEW;
        }
        return false;
    }

    case CHANGE_PIN_CONFIRM_NEW:
    {
        pin_entry_result_t result = pin_entry_handle_button(&ctx->pin_ctx, btn);
        if (result == PIN_ENTRY_CANCELLED)
        {
            // Back out to re-entering the new PIN, not all the way out --
            // they've already gotten past verifying the old one.
            pin_entry_reset(&ctx->pin_ctx);
            ctx->phase = CHANGE_PIN_ENTER_NEW;
            return false;
        }
        if (result == PIN_ENTRY_SUBMITTED)
        {
            uint8_t confirm_pin[SECURITY_PIN_LEN];
            pin_entry_get_pin(&ctx->pin_ctx, confirm_pin);

            if (memcmp(confirm_pin, ctx->new_pin_staged, SECURITY_PIN_LEN) == 0)
            {
                esp_err_t err = security_set_pin(ctx->new_pin_staged);
                if (err == ESP_OK)
                {
                    lcd_flash_info_to("PIN updated", "                ", 2000, LCD_SCREEN_SECURITY);
                    post_security_event(true);
                }
                else
                {
                    // lcd_flash_enqueue("Save failed", FLASH_PRIORITY_HIGH, FLASH_DURATION_SHORT);
                    lcd_flash_warning_to("Save failed", "Back To Return  ", 3000, LCD_SCREEN_SECURITY);
                    post_security_event(false);
                }
                // Clear the staged copy either way -- don't leave a new PIN
                // sitting in RAM longer than needed.
                memset(ctx->new_pin_staged, 0, sizeof(ctx->new_pin_staged));
                ctx->phase = CHANGE_PIN_IDLE;
                return true;
            }
            else
            {
                lcd_flash_warning_to("Did not match", "                ", 4000, LCD_SCREEN_SECURITY);
                post_security_event(false);
                pin_entry_reset(&ctx->pin_ctx);
                ctx->phase = CHANGE_PIN_ENTER_NEW; // start the new-PIN pair over
            }
        }
        return false;
    }

    case CHANGE_PIN_IDLE:
    case CHANGE_PIN_SUCCESS:
    case CHANGE_PIN_MISMATCH:
    default:
        return true;
    }
}

void change_pin_render(const change_pin_ctx_t *ctx,
                       char line1[LCD_LINE_SIZE],
                       char line2[LCD_LINE_SIZE])
{
    switch (ctx->phase)
    {

    case CHANGE_PIN_VERIFY_OLD:
        if (security_is_locked_out_for_scope(lockout_scope_for_context(ctx)))
        {
            uint32_t remaining_s =
                (uint32_t)((security_lockout_remaining_ms_for_scope(
                    lockout_scope_for_context(ctx)) + 999) / 1000);
            snprintf(line1, LCD_LINE_SIZE, "Locked out");
            snprintf(line2, LCD_LINE_SIZE, "Retry in %lus", remaining_s);
        }
        else
        {
            snprintf(line1, LCD_LINE_SIZE, "Old PIN:");
            pin_entry_render_line(&ctx->pin_ctx, line2, LCD_LINE_SIZE);
        }
        break;

    case CHANGE_PIN_ENTER_NEW:
        snprintf(line1, LCD_LINE_SIZE, "New PIN:");
        pin_entry_render_line(&ctx->pin_ctx, line2, LCD_LINE_SIZE);
        break;

    case CHANGE_PIN_CONFIRM_NEW:
        snprintf(line1, LCD_LINE_SIZE, "Confirm PIN:");
        pin_entry_render_line(&ctx->pin_ctx, line2, LCD_LINE_SIZE);
        break;

    default:
        line1[0] = '\0';
        line2[0] = '\0';
        break;
    }
}