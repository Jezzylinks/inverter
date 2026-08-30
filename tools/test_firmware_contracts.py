#!/usr/bin/env python3
"""Host-side contract tests for safety-critical firmware policies."""

import hashlib
import re
import unittest
from pathlib import Path


SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")


def parse_manifest_row(row: str):
    fields = [field.strip() for field in row.split(",")]
    if len(fields) != 4:
        raise ValueError("manifest requires version,url,sha256,size")
    version, url, digest, size_text = fields
    if not version or not url.startswith("https://"):
        raise ValueError("version and HTTPS URL are required")
    if not SHA256_RE.fullmatch(digest):
        raise ValueError("SHA-256 is mandatory")
    size = int(size_text)
    if size <= 0:
        raise ValueError("image size must be positive")
    return version, url, digest.lower(), size


def release_tuple(version: str):
    version = version.lstrip("vV")
    return tuple(int(part) if part.isdigit() else 0 for part in version.split("."))


def telemetry_valid(value: float, minimum: float, maximum: float) -> bool:
    return minimum <= value <= maximum


class FirmwareContracts(unittest.TestCase):
    def test_manifest_requires_digest_and_size(self):
        digest = hashlib.sha256(b"firmware").hexdigest()
        parsed = parse_manifest_row(
            f"1.2.3,https://example.invalid/fw.bin,{digest},8"
        )
        self.assertEqual(parsed[0], "1.2.3")
        self.assertEqual(parsed[3], 8)

    def test_manifest_rejects_missing_integrity_fields(self):
        with self.assertRaises(ValueError):
            parse_manifest_row("1.2.3,https://example.invalid/fw.bin,,8")
        with self.assertRaises(ValueError):
            parse_manifest_row("1.2.3,https://example.invalid/fw.bin," + "a" * 64 + ",0")

    def test_version_must_increase(self):
        self.assertGreater(release_tuple("v1.4.1"), release_tuple("1.4.0"))
        self.assertLessEqual(release_tuple("1.4.0"), release_tuple("1.4.0"))
        self.assertLessEqual(release_tuple("1.3.9"), release_tuple("1.4.0"))

    def test_battery_range_rejects_impossible_values(self):
        self.assertTrue(telemetry_valid(24.0, 5.0, 60.0))
        self.assertFalse(telemetry_valid(0.0, 5.0, 60.0))
        self.assertFalse(telemetry_valid(80.0, 5.0, 60.0))

    def test_reset_pin_context_starts_at_zero(self):
        digit = 0
        cursor = 0
        confirmed = [False] * 4
        self.assertEqual(digit, 0)
        self.assertEqual(cursor, 0)
        self.assertEqual(confirmed, [False, False, False, False])

    def test_wifi_kconfig_exposes_exactly_three_architectures_and_four_client_cap(self):
        kconfig = Path(__file__).parents[1].joinpath("src", "Kconfig.projbuild").read_text()
        for symbol in ("INVERTER_WIFI_MODE_STA", "INVERTER_WIFI_MODE_AP", "INVERTER_WIFI_MODE_APSTA"):
            self.assertIn(f"config {symbol}", kconfig)
        self.assertIn('range 1 4', kconfig)
        self.assertIn('default INVERTER_WIFI_MODE_APSTA', kconfig)

    def test_system_error_codes_are_unique_and_include_internet_failure(self):
        text = Path(__file__).parents[1].joinpath("include", "system", "system_error_codes.h").read_text()
        values = re.findall(r"SYSTEM_ERROR_[A-Z0-9_]+\s*=\s*0x([0-9A-Fa-f]+)", text)
        self.assertEqual(len(values), len(set(values)))
        self.assertIn("SYSTEM_ERROR_WIFI_INTERNET_UNAVAILABLE", text)
        self.assertIn("SYSTEM_ERROR_PROTECTION_INVALID_TELEMETRY", text)

    def test_firmware_uses_explicit_esp_error_handling(self):
        root = Path(__file__).parents[1]
        firmware_files = list(root.joinpath("src").rglob("*.c")) + list(root.joinpath("include").rglob("*.h"))
        offenders = [
            str(path.relative_to(root))
            for path in firmware_files
            if "ESP_ERROR_CHECK(" in path.read_text()
        ]
        self.assertEqual(offenders, [])

    def test_adc_warmup_timeout_inhibits_output_and_reports_a_terminal_fault(self):
        root = Path(__file__).parents[1]
        startup_source = root.joinpath("src", "app_init.c")
        if not startup_source.exists():
            startup_source = root.joinpath("src", "main.c")
        text = startup_source.read_text()
        start = text.index("const bool adc_failed =")
        end = text.index("const bool startup_healthy", start)
        failure_branch = text[start:end]
        self.assertIn("POST_FAILURE_ADC", failure_branch)
        self.assertIn("POST_FAILURE_LCD", failure_branch)
        self.assertIn(".all_passed = false", failure_branch)
        self.assertIn("post_completed = true", failure_branch)
        self.assertNotIn("inverter_emergency_shutdown()", failure_branch)
        self.assertIn("Startup begins with the inverter OFF and system_ready false", failure_branch)
        self.assertIn('"ADC INIT FAIL   "', failure_branch)
        self.assertIn('"ADC TIMEOUT     "', failure_branch)
        self.assertIn('lcd_show_fault("SENSOR STARTUP ", fault)', failure_branch)

    def test_fan_tach_does_not_collide_with_battery_adc_or_declared_peripherals(self):
        root = Path(__file__).parents[1]
        hardware = root.joinpath("include", "hardware", "hardware_config.h").read_text()
        self.assertIn("#define GPIO_FAN_TACH GPIO_NUM_23", hardware)
        self.assertIn("GPIO35 is reserved for Battery Voltage", hardware)
        self.assertIn('"Fan tach GPIO collides with another peripheral"', hardware)
        self.assertIn('"Fan tach GPIO collides with a button GPIO"', hardware)

    def test_adc_startup_timeout_publishes_terminal_failure_before_app_timeout(self):
        root = Path(__file__).parents[1]
        adc = root.joinpath("src", "adc", "inverter_adc.c").read_text()
        self.assertIn("ADC_STARTUP_FAILURE_TIMEOUT_MS 2000U", adc)
        self.assertIn("startup_failure_reported", adc)
        self.assertIn("required telemetry did not become valid/fresh during startup deadline", adc)
        self.assertIn("adc_signal_failed(", adc)
        self.assertIn("!telemetry_ready && !readiness_reported", adc)
        self.assertIn("xEventGroupSetBits(sys_event_group, APP_EVENT_ADC_READY)", adc)
        self.assertIn("telemetry_health_required_ready", adc)

    def test_adc_and_lcd_selection_is_menuconfig_backed_and_exclusive(self):
        root = Path(__file__).parents[1]
        kconfig = root.joinpath("src", "Kconfig.projbuild").read_text()
        self.assertIn("choice INVERTER_ADC_MODE", kconfig)
        self.assertIn("default INVERTER_ADC_MODE_CONTINUOUS", kconfig)
        self.assertIn("config INVERTER_ADC_MODE_ONESHOT", kconfig)
        self.assertIn("choice INVERTER_LCD_GEOMETRY", kconfig)
        self.assertIn("default INVERTER_LCD_20X4", kconfig)

        config = root.joinpath("include", "adc", "inverter_adc_config.h").read_text()
        self.assertIn('include "sdkconfig.h"', config)
        self.assertIn("CONFIG_INVERTER_ADC_MODE_CONTINUOUS", config)
        self.assertIn("CONFIG_INVERTER_ADC_MODE_ONESHOT", config)
        self.assertIn("ADC menuconfig selected both acquisition modes", config)
        self.assertIn("Select an ADC acquisition mode with idf.py menuconfig", config)

        lcd_config = root.joinpath("include", "lcd", "menu_config.h").read_text()
        self.assertIn('include "sdkconfig.h"', lcd_config)
        self.assertIn("CONFIG_INVERTER_LCD_16X2", lcd_config)
        self.assertIn("CONFIG_INVERTER_LCD_20X4", lcd_config)

        platformio = root.joinpath("platformio.ini").read_text()
        self.assertIn("-t menuconfig", platformio)
        self.assertNotIn("esp32dev-continuous-16x2", platformio)
        self.assertNotIn("-DINVERTER_ADC_MODE=", platformio)

    def test_watchdog_registered_self_deletes_unregister_first(self):
        root = Path(__file__).parents[1]
        watchdog = root.joinpath("src", "task_watchdog.c").read_text()
        header = root.joinpath("include", "system", "task_watchdog.h").read_text()
        adc = root.joinpath("src", "adc", "inverter_adc.c").read_text()
        lcd_events = root.joinpath("src", "lcd_event_receiver.c").read_text()
        ota = root.joinpath("src", "ota", "ota_service.c").read_text()
        app_runtime = root.joinpath("src", "app_runtime.c").read_text()
        self.assertIn("void task_watchdog_unregister(void)", watchdog)
        self.assertIn("void task_watchdog_unregister_task(TaskHandle_t task_handle)", watchdog)
        self.assertIn("void task_watchdog_unregister(void);", header)
        self.assertIn("void task_watchdog_unregister_task(TaskHandle_t task_handle);", header)
        self.assertIn("task_watchdog_unregister_task(task);\n        vTaskDelete(task);", root.joinpath("src", "button_controller.c").read_text())
        self.assertIn("app_buttons_deinit();\n    task_watchdog_unregister();", root.joinpath("src", "main.c").read_text())
        continuous = root.joinpath("src", "adc", "adc_continuous.c").read_text()
        self.assertIn("esp_task_wdt_reconfigure(&twdt_config)", app_runtime)
        self.assertIn("#define ADC_CONTINUOUS_FRAME_SIZE 256U", continuous)
        self.assertIn(".conv_frame_size = ADC_CONTINUOUS_FRAME_SIZE", continuous)
        self.assertIn("#define ADC_CONTINUOUS_SAMPLE_FREQ_HZ 20000U", continuous)
        self.assertIn(".timeout_ms = 15000", app_runtime)
        self.assertIn("adc_signal_failed(esp_err_to_name(init_result));\n        task_watchdog_unregister();\n        vTaskDelete(NULL);", adc)
        self.assertIn("task_watchdog_unregister();\n    vTaskDelete(NULL);", lcd_events)
        self.assertIn("task_watchdog_unregister();\n        vTaskDelete(NULL);", ota)

    def test_buzzer_is_initialized_independently_and_uses_nonconflicting_ledc_resources(self):
        root = Path(__file__).parents[1]
        buzzer = root.joinpath("src", "utility", "buzzer.c").read_text()
        dispatcher = root.joinpath("src", "events", "event_dispatcher.c").read_text()
        main = root.joinpath("src", "main.c").read_text()
        header = root.joinpath("include", "utility", "buzzer.h").read_text()
        self.assertIn("#define BUZZER_LEDC_TIMER LEDC_TIMER_2", buzzer)
        self.assertIn("#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_0", buzzer)
        self.assertNotIn("#define BUZZER_LEDC_TIMER LEDC_TIMER_0", buzzer)
        self.assertIn("buzzer_init_result = buzzer_init()", main)
        self.assertIn("Buzzer unavailable; continuing without sound", main)
        self.assertIn("xQueueSend(g_event_subscriber_queue[subscriber], event, 0)", dispatcher)
        self.assertIn("buzzer_record_dispatch_result(false)", dispatcher)
        self.assertIn("buzzer_record_dispatch_result(true)", dispatcher)
        self.assertIn("buzzer_self_test", header)
        self.assertIn("buzzer_get_diagnostic", header)

    def test_button_diagnostics_are_opt_in_and_cover_signal_path(self):
        root = Path(__file__).parents[1]
        kconfig = root.joinpath("src", "Kconfig.projbuild").read_text()
        controller_header = root.joinpath("include", "app", "button_controller.h").read_text()
        controller = root.joinpath("src", "button_controller.c").read_text()
        app_input = root.joinpath("src", "app_input.c").read_text()
        main = root.joinpath("src", "main.c").read_text()
        self.assertIn("config INVERTER_BUTTON_DIAGNOSTICS", kconfig)
        self.assertIn("default n", kconfig)
        self.assertIn("button_diagnostic_t", controller_header)
        self.assertIn("button_controller_get_diagnostic", controller_header)
        self.assertIn("isr_queue_full", controller)
        self.assertIn('"DIAG %s gpio=', controller)
        self.assertIn('"EVENT %s button=', controller)
        self.assertIn('"CALLBACK %s event=', app_input)
        self.assertIn("while (sys_state.system_ready || !startup_healthy)", main)
        self.assertIn("retaining button/event tasks while output remains inhibited", main)

    def test_buttons_remain_available_in_startup_fault_but_inverter_enable_is_locked(self):
        root = Path(__file__).parents[1]
        app_input = root.joinpath("src", "app_input.c").read_text()
        for handler in (
            "handle_enter_menu_button_event",
            "handle_up_button_event",
            "handle_down_button_event",
            "handle_back_button_event",
        ):
            start = app_input.index(f"void {handler}")
            next_handler = app_input.find("\nvoid ", start + 6)
            body = app_input[start:] if next_handler < 0 else app_input[start:next_handler]
            self.assertNotIn("if (!sys_state.system_ready)", body)
        self.assertIn("require_system_ready_for_inverter_action", app_input)
        self.assertIn('"inverter enable"', app_input)
        self.assertIn('"SYSTEM NOT READY"', app_input)
        self.assertIn('"CONTROL LOCKED  "', app_input)
        self.assertIn("if (require_system_ready_for_inverter_action(\"inverter enable\"))", app_input)
        self.assertIn("shutdown_inverter();", app_input)

    def test_progress_bars_use_filled_lcd_blocks(self):
        root = Path(__file__).parents[1]
        lcd_task = root.joinpath("src", "lcd_task.c").read_text()
        lcd_header = root.joinpath("include", "lcd", "lcd.h").read_text()
        lcd_source = root.joinpath("src", "lcd.c").read_text()
        self.assertIn("CHAR_PROGRESS_BLOCK", lcd_task)
        self.assertIn("(char)CHAR_PROGRESS_BLOCK", lcd_task)
        self.assertNotIn("? '#' : '-'", lcd_task)
        self.assertIn("#define CHAR_PROGRESS_BLOCK 2", lcd_header)
        self.assertIn("BAR_2 / PROGRESS_BLOCK", lcd_source)
        self.assertIn("0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F", lcd_source)

    def test_continuous_backend_uses_esp32_supported_sample_rate(self):
        root = Path(__file__).parents[1]
        source = root.joinpath("src", "adc", "adc_continuous.c").read_text()
        self.assertIn("ADC_CONTINUOUS_SAMPLE_FREQ_HZ 20000U", source)
        self.assertNotIn("ADC_CONTINUOUS_SAMPLE_FREQ_HZ 2000U", source)
        self.assertIn("ADC_CONTINUOUS_FRAME_COUNT 8U", source)
        self.assertIn(".channel = channels[i] & 0x7U", source)
        self.assertIn("adc_continuous_register_event_callbacks", source)
        self.assertIn("on_conv_done = continuous_on_conv_done", source)
        self.assertIn("SOC_ADC_DIGI_DATA_BYTES_PER_CONV", source)
        self.assertIn("ADC_CONTINUOUS_STARTUP_GRACE_MS 500U", source)
        self.assertIn("continuous_switch_to_oneshot", source)
        self.assertIn("adc_continuous_deinit(context->handle)", source)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(2))", source)
        self.assertIn("pending EOF ISR", source)
        self.assertIn("adc_oneshot_new_unit", source)
        self.assertIn("switched safely to ADC1 Oneshot fallback", source)

    def test_common_adc_snapshot_and_ready_contract_is_backend_neutral(self):
        root = Path(__file__).parents[1]
        header = root.joinpath("include", "adc", "inverter_adc.h").read_text()
        adapter = root.joinpath("src", "adc", "inverter_adc.c").read_text()
        self.assertIn("inverter_adc_snapshot_t", header)
        self.assertIn("inverter_adc_backend_state_t", header)
        self.assertIn("INVERTER_ADC_BACKEND_FALLBACK", header)
        self.assertIn("inverter_adc_measurement_t", header)
        self.assertIn("inverter_adc_backend_status_t", header)
        self.assertIn("inverter_adc_get_backend_status", header)
        self.assertIn("inverter_adc_get_measurement", header)
        self.assertIn("inverter_adc_get_snapshot", header)
        self.assertIn("inverter_adc_is_ready", header)
        self.assertIn("s_state = INVERTER_ADC_STATE_READY", adapter)
        self.assertIn("telemetry_health_required_ready", adapter)
        self.assertIn("APP_EVENT_ADC_READY", adapter)
        self.assertIn("measurement_record_read_error", adapter)
        self.assertIn("backend_status_refresh", adapter)
        self.assertIn("snapshot.backend_degraded", adapter)

    def test_post_can_run_only_after_adc_and_lcd_ready_events(self):
        text = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        wait_end = text.index("if (adc_ready && lcd_ready)")
        post_call = text.index("startup_post = post_run_all()")
        self.assertGreater(post_call, wait_end)
        self.assertIn("inverter_adc_start()", text)
        self.assertLess(text.index("inverter_adc_start()"), text.index("post_run_all()"))

    def test_post_adc_refuses_unready_or_stale_snapshot(self):
        text = Path(__file__).parents[1].joinpath("src", "post", "post_adc.c").read_text()
        self.assertIn("inverter_adc_is_ready()", text)
        self.assertIn("snapshot.required_data_valid", text)
        self.assertIn("snapshot.fresh", text)
        self.assertIn("ADC snapshot is not ready/fresh", text)

    def test_post_result_propagates_completion_on_pass_and_failure(self):
        text = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        result_writer = re.search(
            r"static void post_show_result_and_notify\(.*?\n}\nvoid app_main",
            text,
            re.DOTALL,
        )
        self.assertIsNotNone(result_writer)
        assert result_writer is not None
        body = result_writer.group(0)
        self.assertIn(
            "lcd_show_startup_status(LCD_STARTUP_STAGE_HARDWARE, true,",
            body,
        )
        self.assertIn("result.all_passed", body)
        self.assertIn("POST result propagated", body)

    def test_loading_expiry_preserves_completed_post_screen(self):
        text = Path(__file__).parents[1].joinpath("src", "lcd_task.c").read_text()
        loading_expiry = re.search(
            r'if \(elapsed >= snap\.loading\.duration_ms\)(.*?)\n            }\n            break;',
            text,
            re.DOTALL,
        )
        self.assertIsNotNone(loading_expiry)
        assert loading_expiry is not None
        self.assertIn("sys_lcd.screen == LCD_SCREEN_LOADING", loading_expiry.group(1))
        self.assertIn("sys_lcd.loading.start_ms == snap.loading.start_ms", loading_expiry.group(1))
        self.assertIn("sys_lcd.screen = sys_lcd.loading.next_screen", loading_expiry.group(1))


if __name__ == "__main__":
    unittest.main()
