#ifndef SECURITY_H
#define SECURITY_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SECURITY_PIN_LEN 4
#define SECURITY_MAX_ATTEMPTS 5
#define SECURITY_LOCKOUT_MS (30 * 1000) // 30-second per-option lockout
#define SECURITY_DEFAULT_PIN {0, 0, 0, 0}

    typedef enum
    {
        SECURITY_LOCKOUT_GENERAL = 0,
        SECURITY_LOCKOUT_FACTORY_RESET,
        SECURITY_LOCKOUT_OTA,
        SECURITY_LOCKOUT_SCOPE_COUNT
    } security_lockout_scope_t;

    typedef struct
    {
        /* ── NVS-persisted ─────────────────────────────────────────── */
        uint8_t enabled;       /* 0 = security off, 1 = PIN required       */
        uint8_t auto_lock_min; /* idle minutes before re-lock (0 = never)  */

        /* ── RAM only ──────────────────────────────────────────────── */
        bool locked;          /* true = panel is locked, PIN required      */
        bool pin_verified;    /* true = PIN was verified this session       */
        uint8_t failed_boots; /* consecutive boots with wrong PIN attempts  */
    } security_ctx_t;

    /**
     * Must be called once during app init, after NVS is ready.
     * Loads the stored PIN hash/salt. If none exists (first boot),
     * provisions the default PIN (0000) and sets the force-change flag.
     */
    esp_err_t security_init(void);

    /**
     * Verify a 4-digit PIN against the stored hash.
     * Updates internal attempt counter / lockout state as a side effect.
     * Returns false immediately (without consuming an attempt) if currently locked out.
     */
    bool security_verify_pin(const uint8_t pin[SECURITY_PIN_LEN]);

    /** Verify the shared PIN while applying the selected option's lockout. */
    bool security_verify_pin_for_scope(const uint8_t pin[SECURITY_PIN_LEN],
                                       security_lockout_scope_t scope);

    /**
     * Hash and persist a new PIN to NVS. Clears the force-change flag.
     * Caller is responsible for having verified the old PIN first, if required.
     */
    esp_err_t security_set_pin(const uint8_t new_pin[SECURITY_PIN_LEN]);

    /** True if a PIN-entry attempt is currently blocked by lockout. */
    bool security_is_locked_out(void);

    /** Milliseconds remaining in the current lockout, 0 if not locked out. */
    int64_t security_lockout_remaining_ms(void);

    bool security_is_locked_out_for_scope(security_lockout_scope_t scope);
    int64_t security_lockout_remaining_ms_for_scope(security_lockout_scope_t scope);

    /** True if the PIN is still the factory default and must be changed. */
    bool security_pin_change_required(void);

    /**
     * Resets the in-RAM attempt counter and clears lockout state.
     * Called internally on successful verification; exposed in case
     * a higher-level policy (e.g. reboot) needs to force-clear it.
     */
    void security_reset_attempts(void);
    void security_reset_scope_attempts(security_lockout_scope_t scope);

    /** Number of failed attempts still available before lockout. */
    uint8_t security_attempts_remaining_for_scope(security_lockout_scope_t scope);

#ifdef __cplusplus
}
#endif

#endif // SECURITY_H