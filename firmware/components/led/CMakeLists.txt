idf_component_register(
    SRCS "led.c" "led_strip_encoder.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)
