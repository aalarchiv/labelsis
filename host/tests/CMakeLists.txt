add_executable(test_placeholder test_placeholder.c)
target_link_libraries(test_placeholder PRIVATE pt_protocol pt_transport)
add_test(NAME test_placeholder COMMAND test_placeholder)

add_executable(test_oracle_fixtures test_oracle_fixtures.c)
target_link_libraries(test_oracle_fixtures PRIVATE fixture)
add_test(NAME test_oracle_fixtures COMMAND test_oracle_fixtures)

add_executable(test_encoder test_encoder.c)
target_link_libraries(test_encoder PRIVATE pt_protocol)
add_test(NAME test_encoder COMMAND test_encoder)

add_executable(test_status_decode test_status_decode.c)
target_link_libraries(test_status_decode PRIVATE pt_protocol)
add_test(NAME test_status_decode COMMAND test_status_decode)

add_executable(test_packbits test_packbits.c)
target_link_libraries(test_packbits PRIVATE pt_protocol)
add_test(NAME test_packbits COMMAND test_packbits)

add_executable(test_bitmap test_bitmap.c)
target_link_libraries(test_bitmap PRIVATE pt_protocol)
add_test(NAME test_bitmap COMMAND test_bitmap)
