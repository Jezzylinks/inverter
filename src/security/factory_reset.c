#include "factory_reset.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

#include "security.h"
#include "pin_entry.h"
#include "lcd_writer.h"   // lcd_flash_enqueue, FLASH_PRIORITY_*, FLASH_DURATION_*
#include "system_state.h" // sys_lcd, sys_state, sys_state_mutex
#include "lcd_state.h"    // LCD_SCREEN_FACTORY_RESET, factory_reset phase atomics
#include "lcd_flash_queue.h"

static const char *TAG = "FACTORY_RESET";

extern lcd_render_state_t sys_lcd;
extern SemaphoreHandle_t sys_state_mutex;

/*==============================================================================
  Internal helpers
==============================================================================*/

/**
 * handle_pin_entry_button() — static, only called within this file.
 * Feeds one button press into the PIN digit-entry state machine and
 * acts on the result:
 *   IN_PROGRESS -> nothing (digit adjusted or mid-entry)
 *   SUBMITTED   -> verify PIN, advance or flash error
 *   CANCELLED   -> Back on digit 0, return to IDLE
 */
static void handle_pin_entry_button(factory_reset_ctx_t *ctx,
                                    button_id_t btn)
{
    /* While locked out, only Back is meaningful (lets the user exit).
     * All other buttons are silently consumed so digit state can't
     * be mutated during a lockout window. */
    if (security_is_locked_out())
    {
        if (btn == BTN_BACK)
        {
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_IDLE);
        }
        return;
    }

    pin_entry_result_t result = pin_entry_handle_button(&ctx->pin_ctx, btn);

    switch (result)
    {
    case PIN_ENTRY_IN_PROGRESS:
        /* Digit adjusted or confirmed but not yet complete -- nothing to do. */
        break;

    case PIN_ENTRY_SUBMITTED:
    {
        uint8_t entered[SECURITY_PIN_LEN];
        pin_entry_get_pin(&ctx->pin_ctx, entered);

        if (security_verify_pin(entered))
        {
            /* Correct PIN -- allow the selected action to proceed. */
            ESP_LOGI(TAG, "PIN verified, advancing to CONFIRM");
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_CONFIRM);
        }
        else if (security_is_locked_out())
        {
            /* This failed attempt triggered the lockout. */
            ESP_LOGW(TAG, "PIN attempts exhausted, locked out");

            lcd_flash_info("Locked out      ", "Try later       ", 1500);
            atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_IDLE);
        }
        else
        {
            /* Wrong PIN, attempts remaining. */
            ESP_LOGW(TAG, "Wrong PIN entered");
            lcd_flash_info("Wrong PIN entered", "Try again       ", 1500);
            pin_entry_reset(&ctx->pin_ctx); /* stay on PIN_ENTRY, reset digits */
        }
        break;
    }

    case PIN_ENTRY_CANCELLED:
        /* Back on the first digit -- user chose to exit without resetting. */
        ESP_LOGI(TAG, "PIN entry cancelled");
        atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_IDLE);
        break;

    default:
        break;
    }
}

/*==============================================================================
  Public API
==============================================================================*/

void factory_reset_begin(factory_reset_ctx_t *ctx,
                         factory_reset_action_t action)
{
    ctx->action = action;
    atomic_store(&sys_lcd.factory_reset.action, action);

    /* Reset digit entry to a clean state before showing the PIN screen. */
    pin_entry_reset(&ctx->pin_ctx);

    /* Gate entry through PIN verification -- never jump straight to CONFIRM. */
    atomic_store(&sys_lcd.factory_reset.phase, FACTORY_RESET_PIN_ENTRY);
    sys_lcd.screen = LCD_SCREEN_FACTORY_RESET;

    ESP_LOGI(TAG, "factory_reset_begin: action=%d, waiting for PIN", action);
}

void factory_reset_handle_pin_entry(factory_reset_ctx_t *ctx,
                                    button_id_t btn)
{
    handle_pin_entry_button(ctx, btn);
}

void factory_reset(void)
{
    ESP_LOGI(TAG, "Performing full factory reset");
    atomic_store(&sys_lcd.factory_reset.progress_pct, 10);

    nvs_flash_erase();
    atomic_store(&sys_lcd.factory_reset.progress_pct, 60);

    nvs_flash_init();
    atomic_store(&sys_lcd.factory_reset.progress_pct, 100);

    atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
    ESP_LOGI(TAG, "Factory reset complete, restarting");
    esp_restart();
}

void clear_settings(void)
{
    ESP_LOGI(TAG, "Clearing settings namespace");
    atomic_store(&sys_lcd.factory_reset.progress_pct, 10);

    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    atomic_store(&sys_lcd.factory_reset.progress_pct, 100);
    atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
    ESP_LOGI(TAG, "Settings cleared");
}

void erase_logs(void)
{
    ESP_LOGI(TAG, "Erasing fault log namespace");
    atomic_store(&sys_lcd.factory_reset.progress_pct, 10);

    nvs_handle_t h;
    if (nvs_open("fault_log", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    atomic_store(&sys_lcd.factory_reset.progress_pct, 100);
    atomic_store(&sys_lcd.factory_reset.phase, FACTORY_PHASE_DONE);
    ESP_LOGI(TAG, "Logs erased");
}