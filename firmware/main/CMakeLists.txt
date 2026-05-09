# CMakeLists.txt for main component
# Registers the main app as an ESP-IDF component

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager recorder storage uploader led button battery logger
)
