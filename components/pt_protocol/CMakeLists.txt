include(${CMAKE_CURRENT_LIST_DIR}/srcs.cmake)

idf_component_register(
    SRCS ${PT_PROTOCOL_SRCS}
    INCLUDE_DIRS "include"
)
