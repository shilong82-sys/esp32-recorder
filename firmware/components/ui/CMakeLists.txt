idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "include"
    REQUIRES event_bus led state esp_timer
)
