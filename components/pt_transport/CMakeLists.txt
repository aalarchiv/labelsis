include(${CMAKE_CURRENT_LIST_DIR}/srcs.cmake)

# usb_host.c is target-only -- host CMake doesn't see this file
# because it pulls only srcs.cmake; here in the idf component build
# we add it.
list(APPEND PT_TRANSPORT_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/usb_host.c)

idf_component_register(
    SRCS ${PT_TRANSPORT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES pt_protocol usb
)
