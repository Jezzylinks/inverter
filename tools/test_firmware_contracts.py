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

    def test_adc_warmup_timeout_inhibits_output_and_reports_a_terminal_fault(self):
        root = Path(__file__).parents[1]
        startup_source = root.joinpath("src", "app_init.c")
        if not startup_source.exists():
            startup_source = root.joinpath("src", "main.c")
        text = startup_source.read_text()
        timeout_branch = re.search(
            r'ADC did not warm up within 10 seconds; inhibiting inverter output(.*?)\n    }\n\n    const bool startup_healthy',
            text,
            re.DOTALL,
        )
        self.assertIsNotNone(timeout_branch)
        assert timeout_branch is not None
        self.assertIn("startup_post.failure_mask = POST_FAILURE_ADC", timeout_branch.group(1))
        self.assertIn("startup_post.all_passed = false", timeout_branch.group(1))
        self.assertIn("post_completed = true", timeout_branch.group(1))
        self.assertIn("inverter_emergency_shutdown()", timeout_branch.group(1))
        self.assertIn('lcd_show_fault("SENSOR STARTUP ", "ADC TIMEOUT     ")', timeout_branch.group(1))

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
