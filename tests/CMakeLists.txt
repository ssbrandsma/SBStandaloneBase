add_executable(test_protocol protocol/test_protocol.c ../native/sbbase/src/protocol.c)
target_include_directories(test_protocol PRIVATE ../native/sbbase/include)
add_test(NAME protocol COMMAND test_protocol)
add_test(NAME storage COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_storage.sh $<TARGET_FILE:sb-storage-helper>)
