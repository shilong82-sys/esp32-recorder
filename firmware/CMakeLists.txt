# Top-level CMakeLists.txt for ESP32 AI Recorder firmware
# Processed by ESP-IDF's project.cmake

cmake_minimum_required(VERSION 3.16)

# Tell ESP-IDF where to find extra components
set(EXTRA_COMPONENT_DIRS components)

# Include ESP-IDF's project cmake (must come before project())
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Now declare the project (ESP-IDF handles the rest)
project(esp32-ai-recorder)
