idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer freertos
    PRIV_REQUIRES ringbuf
)
