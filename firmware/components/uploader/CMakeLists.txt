# CMakeLists.txt for uploader component
# HTTP POST upload WAV files to Mac server

idf_component_register(
    SRCS "uploader.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_http_client nvs_flash storage recorder wifi_manager event_bus
)
