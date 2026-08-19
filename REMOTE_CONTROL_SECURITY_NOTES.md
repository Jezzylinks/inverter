# Remote Control Security Notes

The ESP-IDF documentation reviewed for the remote inverter-control design establishes two relevant platform controls:

- Secure Boot v2 verifies the bootloader and application image signatures before execution. Espressif recommends combining it with flash encryption and disabling exposed debug and boot paths for production devices.
- ESP-TLS supports server verification with CA certificates, a global CA store, certificate bundles, or PSK verification. Skipping server verification is documented as insecure and should not be enabled for the cloud command channel.

Sources:

- <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/secure-boot-v2.html>
- <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_tls.html>
