# CMakeLists.txt for button component
# GPIO button with debounce + event_bus publish

idf_component_register(
    SRCS "button.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer event_bus
)
