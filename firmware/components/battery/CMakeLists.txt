# CMakeLists.txt for battery component
# Battery voltage monitoring via ADC

idf_component_register(
    SRCS "battery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc driver nvs_flash)
