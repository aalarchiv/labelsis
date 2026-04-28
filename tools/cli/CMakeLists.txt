add_executable(pt_send pt_send.c)
target_link_libraries(pt_send PRIVATE pt_protocol pt_transport pt_session)
