from pathlib import Path

text = Path("src/app_services.c").read_text()
assert "app_wifi_sync_menu_if_visible();" in text
assert "if (nvs_err == ESP_OK)" in text
assert "else if (controller_err != ESP_ERR_WIFI_CONN)" not in text
callback = text[text.index("static void app_wifi_status_callback"):text.rindex("static esp_err_t persist_u8")]
enable = callback.split("case APP_WIFI_OPERATION_CONNECT_SAVED:", 1)[0]
disable = callback.split("case APP_WIFI_OPERATION_DISABLE:", 1)[1].split("case APP_WIFI_OPERATION_DISCONNECT:", 1)[0]
assert "terminal = true" not in enable
assert "terminal = true" not in disable
print("Focused Wi-Fi state-flow assertions: PASS")
