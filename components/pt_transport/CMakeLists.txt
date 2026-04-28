include(${CMAKE_CURRENT_LIST_DIR}/srcs.cmake)

idf_component_register(
    SRCS ${PT_TRANSPORT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES pt_protocol
)
