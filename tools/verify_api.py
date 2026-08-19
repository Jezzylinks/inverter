#!/usr/bin/env python3
"""Verify the inverter's protected REST API and WebSocket protocol.

This script performs read-only checks by default. It never scans, connects,
disconnects, resets Wi-Fi, publishes MQTT messages, or starts/cancels OTA.

Examples:
    python3 tools/verify_api.py --base-url http://192.168.4.1 --pin 1234
    INVERTER_URL=http://inverter.local INVERTER_PIN=1234 \
        python3 tools/verify_api.py
    python3 tools/verify_api.py --self-test
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


READ_ENDPOINTS = (
    "/api/v1/status",
    "/api/v1/system",
    "/api/v1/inverter",
    "/api/v1/battery",
    "/api/v1/solar",
    "/api/v1/load",
    "/api/v1/grid",
    "/api/v1/wifi",
    "/api/v1/wifi/config",
    "/api/v1/services",
    "/api/v1/ota",
)


@dataclass
class HttpResult:
    status: int
    payload: Any


class VerificationError(RuntimeError):
    """Raised when a device contract check fails."""


def parse_json(raw: bytes) -> Any:
    if not raw:
        return {}
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise VerificationError(f"response was not valid JSON: {exc}") from exc


def request_json(base_url: str, path: str, pin: str | None, timeout: float) -> HttpResult:
    url = base_url.rstrip("/") + path
    headers = {"Accept": "application/json"}
    if pin:
        headers["X-Inverter-PIN"] = pin
    request = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return HttpResult(response.status, parse_json(response.read()))
    except urllib.error.HTTPError as exc:
        return HttpResult(exc.code, parse_json(exc.read()))
    except urllib.error.URLError as exc:
        raise VerificationError(f"GET {path} failed: {exc.reason}") from exc
    except TimeoutError as exc:
        raise VerificationError(f"GET {path} timed out") from exc


def require_object(path: str, payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise VerificationError(f"{path}: expected a JSON object")
    return payload


def validate_contract(path: str, payload: Any) -> None:
    data = require_object(path, payload)
    if path == "/api/v1/status":
        required = ("system_ready", "inverter_state", "wifi")
    elif path == "/api/v1/system":
        required = ("firmware_version", "hardware", "uptime_seconds", "lcd_columns", "lcd_rows")
    elif path == "/api/v1/inverter":
        required = ("state", "telemetry_valid", "load_percentage")
    elif path == "/api/v1/battery":
        required = ("telemetry_valid", "charging", "low", "critical")
    elif path == "/api/v1/solar":
        required = ("telemetry_valid", "active")
    elif path == "/api/v1/load":
        required = ("telemetry_valid", "percentage", "active")
    elif path == "/api/v1/grid":
        required = ("available", "state")
        if data.get("available") is False and not data.get("reason"):
            raise VerificationError(f"{path}: unavailable grid data must include reason")
    elif path == "/api/v1/wifi":
        required = ("mode", "state", "connected", "got_ip", "internet")
    elif path == "/api/v1/wifi/config":
        required = ("mode", "dhcp", "auto_reconnect", "reconnect_interval_ms", "ip",
                    "gateway", "netmask", "dns", "ap_ssid", "ap_channel",
                    "ap_max_connection", "requires_restart")
        for secret_key in ("password", "ap_password"):
            if secret_key in data:
                raise VerificationError(f"{path}: must not return {secret_key}")
    elif path == "/api/v1/services":
        required = ("http", "dashboard", "websocket", "mdns", "ntp", "mqtt_connected")
    elif path == "/api/v1/ota":
        required = ("state", "installed_version", "progress_percent", "in_progress")
    else:
        required = ()
    missing = [key for key in required if key not in data]
    if missing:
        raise VerificationError(f"{path}: missing fields: {', '.join(missing)}")


def validate_auth_gate(base_url: str, timeout: float) -> None:
    result = request_json(base_url, "/api/v1/status", None, timeout)
    if result.status != 401:
        raise VerificationError(
            f"authentication gate expected HTTP 401 without PIN, got {result.status}"
        )


def run_rest_checks(base_url: str, pin: str, timeout: float) -> int:
    passed = 0
    print("REST API checks")
    for path in READ_ENDPOINTS:
        result = request_json(base_url, path, pin, timeout)
        if result.status != 200:
            raise VerificationError(f"GET {path}: expected HTTP 200, got {result.status}: {result.payload}")
        validate_contract(path, result.payload)
        print(f"  PASS GET {path} ({result.status})")
        passed += 1
    return passed


def ws_url_from_base(base_url: str) -> tuple[str, int, str, bool]:
    value = base_url.rstrip("/")
    if value.startswith("https://"):
        secure = True
        rest = value[len("https://") :]
    elif value.startswith("http://"):
        secure = False
        rest = value[len("http://") :]
    else:
        raise VerificationError("--base-url must begin with http:// or https://")
    if "/" in rest:
        authority, suffix = rest.split("/", 1)
        path = "/" + suffix
    else:
        authority, path = rest, ""
    if ":" in authority and authority.rsplit(":", 1)[1].isdigit():
        host, port_text = authority.rsplit(":", 1)
        http_port = int(port_text)
    else:
        host, http_port = authority, 443 if secure else 80
    ws_port = http_port
    return host, ws_port, "/ws", secure


class WebSocketClient:
    def __init__(self, base_url: str, timeout: float) -> None:
        self.timeout = timeout
        self.host, self.port, self.path, self.secure = ws_url_from_base(base_url)
        raw = socket.create_connection((self.host, self.port), timeout=timeout)
        if self.secure:
            context = ssl.create_default_context()
            raw = context.wrap_socket(raw, server_hostname=self.host)
        self.sock = raw
        self.sock.settimeout(timeout)
        self._handshake()

    def _handshake(self) -> None:
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {self.path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        ).encode("ascii")
        self.sock.sendall(request)
        response = bytearray()
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise VerificationError("WebSocket handshake closed unexpectedly")
            response.extend(chunk)
            if len(response) > 16384:
                raise VerificationError("WebSocket handshake response was too large")
        header = bytes(response).split(b"\r\n\r\n", 1)[0].decode("latin1")
        first_line = header.split("\r\n", 1)[0]
        if " 101 " not in first_line:
            raise VerificationError(f"WebSocket handshake failed: {first_line}")
        expected = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
        ).decode("ascii")
        headers = {}
        for line in header.split("\r\n")[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.lower().strip()] = value.strip()
        if headers.get("sec-websocket-accept") != expected:
            raise VerificationError("WebSocket handshake accept key did not validate")

    def send_json(self, message: dict[str, Any]) -> None:
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        mask = os.urandom(4)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        length = len(masked)
        if length < 126:
            header = bytes((0x81, 0x80 | length))
        elif length < 65536:
            header = bytes((0x81, 0x80 | 126)) + struct.pack("!H", length)
        else:
            header = bytes((0x81, 0x80 | 127)) + struct.pack("!Q", length)
        self.sock.sendall(header + mask + masked)

    def recv_json(self, deadline: float) -> dict[str, Any]:
        while time.monotonic() < deadline:
            opcode, payload = self._recv_frame(deadline)
            if opcode == 0x9:
                self._send_control(0xA, payload)
                continue
            if opcode == 0x8:
                raise VerificationError("WebSocket closed before expected JSON response")
            if opcode != 0x1:
                continue
            try:
                value = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise VerificationError(f"WebSocket returned invalid JSON: {exc}") from exc
            if not isinstance(value, dict):
                raise VerificationError("WebSocket JSON response was not an object")
            return value
        raise VerificationError("timed out waiting for WebSocket JSON response")

    def _recv_exact(self, length: int, deadline: float) -> bytes:
        result = bytearray()
        while len(result) < length:
            if time.monotonic() >= deadline:
                raise VerificationError("timed out reading WebSocket frame")
            chunk = self.sock.recv(length - len(result))
            if not chunk:
                raise VerificationError("WebSocket closed while reading a frame")
            result.extend(chunk)
        return bytes(result)

    def _recv_frame(self, deadline: float) -> tuple[int, bytes]:
        first, second = self._recv_exact(2, deadline)
        opcode = first & 0x0F
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2, deadline))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8, deadline))[0]
        if length > 1_048_576:
            raise VerificationError("WebSocket frame exceeds 1 MiB test limit")
        masked = bool(second & 0x80)
        mask = self._recv_exact(4, deadline) if masked else b""
        payload = self._recv_exact(length, deadline)
        if masked:
            payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        return opcode, payload

    def _send_control(self, opcode: int, payload: bytes) -> None:
        if len(payload) > 125:
            raise VerificationError("control frame is too large")
        mask = os.urandom(4)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.sock.sendall(bytes((0x80 | opcode, 0x80 | len(payload))) + mask + masked)

    def close(self) -> None:
        try:
            self._send_control(0x8, b"")
        except OSError:
            pass
        self.sock.close()


def run_websocket_checks(base_url: str, pin: str, timeout: float) -> int:
    client = WebSocketClient(base_url, timeout)
    try:
        passed = 0
        client.send_json({"cmd": "authenticate", "pin": pin})
        authenticated = client.recv_json(time.monotonic() + timeout)
        if authenticated.get("type") != "authenticated" or authenticated.get("ok") is not True:
            raise VerificationError(f"WebSocket authentication failed: {authenticated}")
        print("  PASS authenticate")
        passed += 1

        client.send_json({"cmd": "subscribe"})
        subscription_status = client.recv_json(time.monotonic() + timeout)
        if subscription_status.get("type") != "status":
            raise VerificationError(f"subscribe did not return a status response: {subscription_status}")
        for field in ("state", "connected", "got_ip", "rssi"):
            if field not in subscription_status:
                raise VerificationError(f"subscribe status missing field {field!r}")
        print("  PASS subscribe and one-client status response")
        passed += 1

        client.send_json({"cmd": "get_status"})
        requested_status = client.recv_json(time.monotonic() + timeout)
        if requested_status.get("type") != "status":
            raise VerificationError(f"get_status did not return status: {requested_status}")
        print("  PASS get_status request/response")
        passed += 1
        return passed
    finally:
        client.close()


def self_test() -> int:
    payload = {"state": "connected", "connected": True, "got_ip": True, "rssi": -42}
    validate_contract("/api/v1/wifi", {"mode": "sta", **payload, "internet": "available"})
    validate_contract("/api/v1/wifi/config", {
        "mode": "sta", "dhcp": True, "auto_reconnect": True,
        "reconnect_interval_ms": 5000, "ip": "0.0.0.0", "gateway": "0.0.0.0",
        "netmask": "0.0.0.0", "dns": "0.0.0.0", "ap_ssid": "Inverter",
        "ap_channel": 6, "ap_max_connection": 4, "requires_restart": True,
    })
    frame_payload = b"test-frame"
    mask = b"abcd"
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(frame_payload))
    if bytes(byte ^ mask[index % 4] for index, byte in enumerate(masked)) != frame_payload:
        raise VerificationError("WebSocket mask/unmask self-test failed")
    print("SELF-TEST PASS: REST contract validation and WebSocket masking logic")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=os.getenv("INVERTER_URL", "http://192.168.4.1"),
                        help="Base device URL, default: %(default)s")
    parser.add_argument("--pin", default=os.getenv("INVERTER_PIN", ""),
                        help="Panel PIN; may also be supplied as INVERTER_PIN")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="Per-request/socket timeout in seconds, default: %(default)s")
    parser.add_argument("--rest-only", action="store_true", help="Skip WebSocket checks")
    parser.add_argument("--ws-only", action="store_true", help="Skip REST checks")
    parser.add_argument("--self-test", action="store_true", help="Run local checks without contacting hardware")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.self_test:
        try:
            return self_test()
        except VerificationError as exc:
            print(f"SELF-TEST FAIL: {exc}", file=sys.stderr)
            return 1
    if args.rest_only and args.ws_only:
        print("--rest-only and --ws-only cannot be used together", file=sys.stderr)
        return 2
    try:
        if not args.pin:
            validate_auth_gate(args.base_url, args.timeout)
            print("PASS authentication gate: protected API returns HTTP 401 without a PIN")
            print("No protected endpoint checks ran. Supply --pin or INVERTER_PIN for the full suite.")
            return 2
        total = 0
        if not args.ws_only:
            total += run_rest_checks(args.base_url, args.pin, args.timeout)
        if not args.rest_only:
            print("WebSocket checks")
            total += run_websocket_checks(args.base_url, args.pin, args.timeout)
        print(f"PASS: {total} API/protocol checks")
        return 0
    except (VerificationError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
