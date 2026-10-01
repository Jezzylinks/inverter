#pragma once

/*
 * Application CPU affinity policy.
 *
 * Core 0 is reserved for application/system work whose latency may vary:
 * Wi-Fi, networking, NVS, UI, logging, OTA and background services.
 *
 * Core 1 is reserved for inverter real-time work:
 * ADC processing and protection enforcement.
 *
 * Keep the policy in one place so a future single-core build remains
 * compilable without scattering CPU-number assumptions throughout the
 * firmware.
 */
#if CONFIG_FREERTOS_UNICORE
#define APP_CORE_SYSTEM 0
#define APP_CORE_REALTIME 0
#else
#define APP_CORE_SYSTEM 0
#define APP_CORE_REALTIME 1
#endif
