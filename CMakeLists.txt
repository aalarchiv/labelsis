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

# Board profile selection. Each board lives under boards/<name>/ and
# carries a board.h (LED pin, reset GPIO, friendly name), a per-board
# sdkconfig.defaults overlay (flash size, PSRAM, BT, partition file),
# and a partitions.csv sized for that board's flash.
#
# Pick a board with -D BOARD=<name> on the idf.py command line, or by
# setting the BOARD environment variable. The default tracks the
# best-tested board.
#
# After switching BOARD value, nuke build/ + sdkconfig (CMake caches
# the previous SDKCONFIG_DEFAULTS list, and IDF only consumes it on
# initial config):
#     rm -rf build sdkconfig
#     idf.py -D BOARD=<name> set-target <s2|s3>
#     idf.py build
if(NOT DEFINED BOARD)
    if(DEFINED ENV{BOARD})
        set(BOARD $ENV{BOARD})
    else()
        set(BOARD "devkitc_s3")
    endif()
endif()
set(BOARD "${BOARD}" CACHE STRING "LabelSis target board profile under boards/<name>/")
set(BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}" CACHE INTERNAL "")

if(NOT EXISTS "${BOARD_DIR}/board.h")
    file(GLOB _avail RELATIVE "${CMAKE_CURRENT_LIST_DIR}/boards"
         "${CMAKE_CURRENT_LIST_DIR}/boards/*/board.h")
    list(TRANSFORM _avail REPLACE "/board.h" "")
    string(REPLACE ";" ", " _avail "${_avail}")
    message(FATAL_ERROR
        "Unknown BOARD=\"${BOARD}\".\n"
        "Available boards: ${_avail}.\n"
        "Pass with: idf.py -D BOARD=<name> ...")
endif()

# SDKCONFIG_DEFAULTS must be set BEFORE include(project.cmake) so IDF
# merges the per-board overlay (flash/partitions/PSRAM/BT) into the
# initial sdkconfig.
list(APPEND SDKCONFIG_DEFAULTS
     "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
     "${BOARD_DIR}/sdkconfig.defaults")

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
        "Fix: rm -rf build sdkconfig && idf.py -D BOARD=${BOARD} set-target esp32s3")
endif()

message(STATUS "LabelSis: BOARD=${BOARD}  IDF_TARGET=${IDF_TARGET}")

project(labelsis)
