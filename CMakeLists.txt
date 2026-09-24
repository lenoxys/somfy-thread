# SPDX-License-Identifier: Unlicense
# Standard esp-matter project glue. Requires the esp-matter repo, with its
# submodules installed, on ESP_MATTER_PATH, and esp-idf on IDF_PATH.
cmake_minimum_required(VERSION 3.5)

# Firmware version string comes from version.txt in release builds and git
# describe otherwise. The numeric Matter OTA version must increase per release.
set(PROJECT_VER_NUMBER 9)
file(WRITE "${CMAKE_BINARY_DIR}/matter-version.txt" "${PROJECT_VER_NUMBER}\n")

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Set ESP_MATTER_PATH to the path of the esp-matter repo")
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
    set(ENV{ESP_MATTER_DEVICE_PATH} ${ESP_MATTER_PATH}/device_hal/device/esp32c6_devkit_c)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${ESP_MATTER_PATH}/device_hal/device"
    ${extra_components_dirs_append})

idf_build_set_property(CXX_COMPILE_OPTIONS "-DCHIP_HAVE_CONFIG_H" APPEND)

project(somfy_thread)
