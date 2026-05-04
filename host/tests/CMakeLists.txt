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

add_executable(test_encoder_vs_oracle test_encoder_vs_oracle.c)
target_link_libraries(test_encoder_vs_oracle PRIVATE pt_protocol fixture)
add_test(NAME test_encoder_vs_oracle COMMAND test_encoder_vs_oracle)

add_executable(test_mock_transport test_mock_transport.c)
target_link_libraries(test_mock_transport PRIVATE pt_protocol pt_transport)
add_test(NAME test_mock_transport COMMAND test_mock_transport)

add_executable(test_session test_session.c)
target_link_libraries(test_session PRIVATE pt_protocol pt_transport pt_session)
add_test(NAME test_session COMMAND test_session)

# Hardware-conditional smoke test for the libusb transport - needs a
# real PT-* plugged in. CTest treats exit 77 as SKIP, so this is a
# silent no-op without hardware.
if(PT_HAVE_LIBUSB)
    add_executable(smoke_libusb smoke_libusb.c)
    target_link_libraries(smoke_libusb PRIVATE pt_protocol pt_transport pt_session)
    add_test(NAME smoke_libusb COMMAND smoke_libusb)
    set_tests_properties(smoke_libusb PROPERTIES SKIP_RETURN_CODE 77)
endif()
