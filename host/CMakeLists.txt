cmake_minimum_required(VERSION 3.16)
project(pt700_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Pull in the same source lists the ESP-IDF components use, so we compile
# byte-identical code on both targets.
include(${CMAKE_CURRENT_SOURCE_DIR}/../components/pt_protocol/srcs.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../components/pt_transport/srcs.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../components/pt_session/srcs.cmake)

add_library(pt_protocol STATIC ${PT_PROTOCOL_SRCS})
target_include_directories(pt_protocol PUBLIC ${PT_PROTOCOL_INCLUDE_DIRS})

add_library(pt_transport STATIC ${PT_TRANSPORT_SRCS})
target_include_directories(pt_transport PUBLIC ${PT_TRANSPORT_INCLUDE_DIRS})
target_link_libraries(pt_transport PUBLIC pt_protocol)

add_library(pt_session STATIC ${PT_SESSION_SRCS})
target_include_directories(pt_session PUBLIC ${PT_SESSION_INCLUDE_DIRS})
target_link_libraries(pt_session PUBLIC pt_protocol pt_transport)

# Optional Linux/macOS userspace transport — if libusb-1.0 is present,
# pull pt_transport/src/libusb.c into the pt_transport library so the
# pt_send CLI and the libusb smoke test can use it. The ESP-IDF
# component build never sees this file.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB libusb-1.0)
endif()
if(LIBUSB_FOUND)
    target_sources(pt_transport PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../components/pt_transport/src/libusb.c)
    target_include_directories(pt_transport PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(pt_transport PUBLIC ${LIBUSB_LIBRARIES})
    set(PT_HAVE_LIBUSB ON)
    message(STATUS "libusb-1.0 ${LIBUSB_VERSION} found — pt_transport_libusb enabled")

    # pt_send CLI lives in tools/cli/ but is wired into the host build
    # so 'cmake --build build-host' also builds it.
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tools/cli
                     ${CMAKE_CURRENT_BINARY_DIR}/tools_cli)
else()
    set(PT_HAVE_LIBUSB OFF)
    message(STATUS "libusb-1.0 NOT found — pt_transport_libusb disabled")
endif()

# Test-only support library: oracle fixture loader.
add_library(fixture STATIC lib/fixture.c)
target_include_directories(fixture PUBLIC lib)
target_compile_definitions(fixture PRIVATE
    PT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../test/fixtures")

enable_testing()
add_subdirectory(tests)
