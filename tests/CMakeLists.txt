add_executable(test_protocol protocol/test_protocol.c ../native/sbbase/src/protocol.c)
target_include_directories(test_protocol PRIVATE ../native/sbbase/include)
add_test(NAME protocol COMMAND test_protocol)
add_test(NAME storage COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_storage.sh $<TARGET_FILE:sb-storage-helper>)
add_test(NAME storage_boothelper COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_boothelper.sh ${CMAKE_SOURCE_DIR}/scripts/sbdata-boot.sh)
add_test(NAME extended_storage COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_extended_storage.sh
  ${CMAKE_SOURCE_DIR}/scripts/storage-setup.sh
  ${CMAKE_SOURCE_DIR}/scripts/storage-boot.sh
  $<TARGET_FILE:sb-storage-helper>
  ${CMAKE_SOURCE_DIR}/artifacts/sbubifs-authorized.ko)
find_program(LUA_EXECUTABLE NAMES lua lua5.1)
if(LUA_EXECUTABLE)
  add_test(NAME extended_storage_lua_state COMMAND ${LUA_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_extended_storage_state.lua
    ${CMAKE_SOURCE_DIR}/applet)
endif()
