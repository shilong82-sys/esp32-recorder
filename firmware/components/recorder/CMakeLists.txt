# CMakeLists.txt for recorder component
# I2S microphone recording, WAV file generation

idf_component_register(
    SRCS "recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc
    PRIV_REQUIRES storage esp_timer
)
