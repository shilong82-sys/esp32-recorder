# CMakeLists.txt for storage component
# TF card (SDMMC / SPI) access, FATFS

idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs driver
)
