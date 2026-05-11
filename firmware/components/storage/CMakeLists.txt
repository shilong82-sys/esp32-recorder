# CMakeLists.txt for storage component
# TF card (SPI mode) access, FATFS

idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs driver sdmmc
)
