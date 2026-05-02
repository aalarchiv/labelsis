include(${CMAKE_CURRENT_LIST_DIR}/srcs.cmake)

# usb_host.c + plite_unstick.c are target-only -- host CMake pulls
# only srcs.cmake; here in the idf component build we add the
# esp-idf-only sources.
list(APPEND PT_TRANSPORT_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/src/usb_host.c
    ${CMAKE_CURRENT_LIST_DIR}/src/plite_unstick.c
)

idf_component_register(
    SRCS ${PT_TRANSPORT_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        pt_protocol
        usb
        fatfs
        usb_host_msc
)
