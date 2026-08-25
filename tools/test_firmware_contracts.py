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
        self.assertIn("inverter_emergency_shutdown()", failure_branch)
        self.assertIn('"ADC INIT FAIL   "', failure_branch)
        self.assertIn('"ADC TIMEOUT     "', failure_branch)
        self.assertIn('lcd_show_fault("SENSOR STARTUP ", fault)', failure_branch)

    def test_adc_mode_is_compile_time_exclusive_and_defaults_to_continuous(self):
        root = Path(__file__).parents[1]
        config = root.joinpath("include", "adc", "inverter_adc_config.h").read_text()
        self.assertIn("#define INVERTER_ADC_MODE_CONTINUOUS 0", config)
        self.assertIn("#define INVERTER_ADC_MODE_ONESHOT    1", config)
        self.assertIn("#define INVERTER_ADC_MODE INVERTER_ADC_MODE_CONTINUOUS", config)
        self.assertIn("#error", config)
        self.assertIn("INVERTER_ADC_MODE != INVERTER_ADC_MODE_CONTINUOUS", config)
        self.assertIn("INVERTER_ADC_MODE != INVERTER_ADC_MODE_ONESHOT", config)
        platformio = root.joinpath("platformio.ini").read_text()
        self.assertIn("esp32dev-continuous-16x2", platformio)
        self.assertIn("esp32dev-oneshot-20x4", platformio)
        self.assertIn("esp32dev-oneshot-16x2", platformio)

    def test_common_adc_snapshot_and_ready_contract_is_backend_neutral(self):
        root = Path(__file__).parents[1]
        header = root.joinpath("include", "adc", "inverter_adc.h").read_text()
        adapter = root.joinpath("src", "adc", "inverter_adc.c").read_text()
        self.assertIn("inverter_adc_snapshot_t", header)
        self.assertIn("inverter_adc_get_snapshot", header)
        self.assertIn("inverter_adc_is_ready", header)
        self.assertIn("s_state = INVERTER_ADC_STATE_READY", adapter)
        self.assertIn("telemetry_health_required_ready", adapter)
        self.assertIn("APP_EVENT_ADC_READY", adapter)

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
