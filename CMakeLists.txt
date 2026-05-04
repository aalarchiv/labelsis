cmake_minimum_required(VERSION 3.16)

# Build-time version. ESP-IDF picks PROJECT_VER up automatically and
# bakes it into the app description struct; api_info reads it back via
# esp_app_get_description()->version and the SPA shows it in About.
# Format: git describe with --dirty so dev builds advertise their
# working-tree state.
execute_process(
    COMMAND git describe --always --dirty --tags
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE  GIT_DESCRIBE_RC
    ERROR_QUIET
)
if(NOT GIT_DESCRIBE_RC EQUAL 0 OR NOT PROJECT_VER)
    set(PROJECT_VER "0.0.0-unknown")
endif()

# Re-run CMake configure when HEAD moves or the index changes so the
# --dirty suffix stays honest across commits and stage/unstage cycles.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD"
        "${CMAKE_CURRENT_LIST_DIR}/.git/index")
endif()

# Pull in ESP-IDF's project machinery. Source ~/esp/esp-idf/export.sh first
# (or run scripts/idf-env.sh) so IDF_PATH is set.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LabelSis needs the ESP USB-OTG host peripheral, which only the S2/S3
# carry. Plain esp32 (the IDF default when no `set-target` has run)
# silently lacks the `usb_host.h` headers and produces an obscure
# "Missing usb/usb_host.h" build error 30 seconds in. Catch it at
# configure time with a message that points at the fix instead.
if(NOT IDF_TARGET STREQUAL "esp32s2" AND NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR
        "LabelSis requires the ESP32-S2 or ESP32-S3 (USB-OTG host).\n"
        "Current IDF_TARGET=\"${IDF_TARGET}\" has no host peripheral.\n"
        "Fix: rm -rf build sdkconfig && idf.py set-target esp32s3")
endif()

project(labelsis)
