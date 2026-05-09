# CMakeLists.txt for wifi_manager component
# Handles WiFi station mode connection with reconnect logic

idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event nvs_flash)
