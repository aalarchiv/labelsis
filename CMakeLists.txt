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

project(pt700)
