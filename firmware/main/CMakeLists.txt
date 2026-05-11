idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager recorder storage uploader led button battery logger
    event_bus state ui system_monitor audio
)
