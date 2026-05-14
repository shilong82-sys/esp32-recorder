# CMakeLists.txt for ringbuf component
# Lock-free ring buffer for audio data (FreeRTOS ringbuffer)

idf_component_register(
    SRCS "ringbuf.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_ringbuf
)
