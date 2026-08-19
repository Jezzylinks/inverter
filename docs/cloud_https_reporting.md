# Direct HTTPS Cloud Reporting

## Purpose

The firmware can now report a bounded, read-only inverter telemetry snapshot directly to the cloud backend over HTTPS. This eliminates the need for a continuously connected MQTT gateway. The ESP32 remains the authority for electrical protection, measurement quality, hardware output, and OTA validation.

## Local provisioning

Cloud reporting is configured only through the local API and requires the existing device PIN. The reporting credential returned by the cloud enrollment endpoint is stored in ESP32 NVS and is never returned by the local API or mobile application.

| Route | Method | Authentication | Behavior |
|---|---:|---|---|
| `/api/v1/cloud` | `GET` | `X-Inverter-PIN` | Returns non-secret reporting configuration and health state. |
| `/api/v1/cloud/config` | `POST` | `X-Inverter-PIN` | Enables direct HTTPS reporting and stores the endpoint, hardware ID, one-time enrollment code, and interval. |

The configuration request has the following shape. `endpoint` must be an HTTPS origin, and `period_sec` must be at least 30 seconds.

```json
{
  "enabled": true,
  "endpoint": "https://cloud.example",
  "hardware_id": "esp32-6f14aa",
  "enrollment_code": "one-time-code-created-by-cloud-account",
  "period_sec": 60
}
```

## Cloud enrollment and reporting

The first report uses the one-time enrollment code as `X-Inverter-Enrollment` at `POST {endpoint}/api/device/v1/enroll`. A successful enrollment returns a device-reporting token, which is retained only in ESP32 NVS. Subsequent reports use `X-Inverter-Device-Token` at `POST {endpoint}/api/device/v1/telemetry`.

The reporter sends inverter state, firmware version, battery voltage/SOC, solar/load power, output measurements, load percentage, Wi-Fi RSSI, telemetry-validity state, and fault flags. Reporting runs in a separate bounded task and never blocks the ADC, protection, display, OTA, or button paths.

## Cloud account capabilities

The backend maps a claimed device to its owner, supports viewer sharing, stores telemetry and fault history, records preference-aware alert events, and logs remote-control requests. Cloud notifications are issued only for configured alert types and only to registered app-device tokens.

## Safe control boundary

Remote electrical controls remain deliberately unavailable. The cloud API records a **blocked control audit** instead of issuing a hardware command until the firmware provides all of the following: an authenticated command route, strict allow-list validation, local interlock checks, acknowledgement semantics, timeout behavior, replay protection, and a safe failure mode. This prevents a cloud feature from bypassing the ESP32’s existing protection system.
