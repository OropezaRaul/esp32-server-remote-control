cmake_minimum_required(VERSION 3.16)

# MicroLink lives in components/microlink/ as an upstream repo clone.
# Its actual components are nested: components/microlink/components/{microlink,wireguard_lwip}.
# We point EXTRA_COMPONENT_DIRS there so ESP-IDF picks them up alongside our app.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/microlink/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32_Remote_Control)