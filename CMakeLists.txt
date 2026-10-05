cmake_minimum_required(VERSION 3.16)

# Requires: IDF_PATH (ESP-IDF v5.2+ / v5.4) and ESP_MATTER_PATH (esp-matter release/v1.4 or newer)
set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)

# NOTE: device_hal/device is intentionally NOT included. This project drives
# its relay/button directly with GPIO and does not use esp-matter's board HAL
# (led_driver, button_driver), which would otherwise fail to resolve.
set(EXTRA_COMPONENT_DIRS
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    ${extra_components_dirs_append})

project(esp32_matter_light)

set(PROJECT_VER "1.0.0")
set(PROJECT_VER_NUMBER 1)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wformat=0" APPEND)
