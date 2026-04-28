# Shared source list — included by both the ESP-IDF component build and the
# host CMake build so the same files compile in both contexts. Use
# CMAKE_CURRENT_LIST_DIR so paths resolve from this file's location.
set(PT_PROTOCOL_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/src/pt_protocol.c
)
set(PT_PROTOCOL_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/include
)
