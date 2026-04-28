cmake_minimum_required(VERSION 3.16)
project(pt700_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Pull in the same source lists the ESP-IDF components use, so we compile
# byte-identical code on both targets.
include(${CMAKE_CURRENT_SOURCE_DIR}/../components/pt_protocol/srcs.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../components/pt_transport/srcs.cmake)

add_library(pt_protocol STATIC ${PT_PROTOCOL_SRCS})
target_include_directories(pt_protocol PUBLIC ${PT_PROTOCOL_INCLUDE_DIRS})

add_library(pt_transport STATIC ${PT_TRANSPORT_SRCS})
target_include_directories(pt_transport PUBLIC ${PT_TRANSPORT_INCLUDE_DIRS})
target_link_libraries(pt_transport PUBLIC pt_protocol)

# Test-only support library: oracle fixture loader.
add_library(fixture STATIC lib/fixture.c)
target_include_directories(fixture PUBLIC lib)
target_compile_definitions(fixture PRIVATE
    PT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../test/fixtures")

enable_testing()
add_subdirectory(tests)
