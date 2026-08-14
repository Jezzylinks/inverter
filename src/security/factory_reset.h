#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

#include <stdint.h>
#include <stdbool.h>
#include "stdatomic.h"
#include "esp_err.h"
#include "pin_entry.h"
#include "lcd_config.h"
 // pin_entry_ctx_t
#include "security.h"  // SECURITY_PIN_LEN

#ifdef __cplusplus
extern "C"
{
#endif

  /*==============================================================================
    factory_reset_phase_t
    Order matters: IDLE must be 0 (default after memset/zero-init).
    FACTORY_RESET_PIN_ENTRY sits before CONFIRM so the PIN gate
    is always traversed before any destructive action is confirmed.
  ==============================================================================*/
  typedef enum
  {
    FACTORY_PHASE_IDLE = 0,
    FACTORY_RESET_PIN_ENTRY, // PIN verification gate
    FACTORY_PHASE_CONFIRM,   // "Are you sure?" screen
    FACTORY_PHASE_PROGRESS,  // action in progress (blocks all input)
    FACTORY_PHASE_DONE,      // action complete
  } factory_reset_phase_t;

  /*==============================================================================
    factory_reset_action_t
    Which destructive action the user selected from the list.
    FACTORY_ACTION_NONE = 0 so zero-init is always safe.
  ==============================================================================*/
  typedef enum
  {
    FACTORY_ACTION_NONE = 0,
    FACTORY_ACTION_RESET_ALL,
    FACTORY_ACTION_CLEAR_SETTINGS,
    FACTORY_ACTION_ERASE_LOGS,
  } factory_reset_action_t;

  /*==============================================================================
    factory_reset_ctx_t
    Button-task-owned context. All fields written by the button task only,
    except phase/action/progress_pct which are _Atomic so lcd_task can
    read them safely without the mutex.

    pin_ctx: digit-entry state machine for FACTORY_RESET_PIN_ENTRY.
             Owned entirely by the button task; lcd_task reads it under
             change_pin_mutex for rendering.
  ==============================================================================*/
  typedef struct
  {
    _Atomic factory_reset_phase_t phase;
    _Atomic uint8_t progress_pct;
    _Atomic factory_reset_action_t action;
    char pin_line[LCD_LINE_SIZE];
    pin_entry_ctx_t pin_ctx;

  } factory_reset_ctx_t;

  /*==============================================================================
    Public API
    These are called from button handlers (handle_enter_menu_button_event,
    handle_up/down/back equivalents) and from app_main init.
  ==============================================================================*/

  /**
   * Perform the full factory reset (erase all NVS, restore defaults, restart).
   * Called from handle_enter_menu_button_event() after FACTORY_PHASE_CONFIRM.
   * Runs synchronously -- caller must have set phase = FACTORY_PHASE_PROGRESS
   * before calling so lcd_task shows the progress screen.
   */
  void perform_factory_reset(void);

  /**
   * Clear only the settings namespace (preserve logs, PIN, WiFi credentials).
   */
  void clear_settings(void);

  /**
   * Erase the fault/error log namespace only.
   */
  void erase_logs(void);

  /**
   * Feed a button event into the factory reset PIN entry state machine.
   * Must only be called when:
   *   sys_state.menu_state == MENU_FACTORY_RESET  AND
   *   atomic_load(&sys_lcd.factory_reset.phase) == FACTORY_RESET_PIN_ENTRY
   *
   * Internally calls security_verify_pin() on PIN_ENTRY_SUBMITTED.
   * On success: advances phase to FACTORY_PHASE_CONFIRM.
   * On failure: flashes "Wrong PIN", resets digit entry, stays on PIN_ENTRY.
   * On lockout: flashes "Locked out", returns to FACTORY_PHASE_IDLE.
   * On cancel (Back on digit 0): returns to FACTORY_PHASE_IDLE.
   *
   * @param ctx  Pointer to sys_state.factory_reset
   * @param btn  The button that was pressed (BUTTON_UP/DOWN/ENTER/BACK)
   */
  void factory_reset_handle_pin_entry(factory_reset_ctx_t *ctx,
                                      button_id_t btn);

  /**
   * Enter the factory reset PIN gate for the given action.
   * Call this instead of setting phase = FACTORY_PHASE_CONFIRM directly.
   * Resets the digit entry context and sets phase = FACTORY_RESET_PIN_ENTRY.
   *
   * @param ctx     Pointer to sys_state.factory_reset
   * @param action  Which action was selected (RESET_ALL / CLEAR_SETTINGS / ERASE_LOGS)
   */
  void factory_reset_begin(factory_reset_ctx_t *ctx,
                           factory_reset_action_t action);

  void factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif // FACTORY_RESET_H