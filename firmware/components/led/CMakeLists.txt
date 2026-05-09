# CMakeLists.txt for led component
# LED indicator using PWM or GPIO

idf_component_register(
    SRCS "led.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES esp_timer
)
