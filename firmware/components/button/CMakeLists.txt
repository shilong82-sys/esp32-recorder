# CMakeLists.txt for button component
# GPIO button with debounce logic

idf_component_register(
    SRCS "button.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer)
