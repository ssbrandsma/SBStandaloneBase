add_executable(test_protocol protocol/test_protocol.c ../native/sbbase/src/protocol.c)
target_include_directories(test_protocol PRIVATE ../native/sbbase/include)
add_test(NAME protocol COMMAND test_protocol)
add_executable(test_nowplaying nowplaying/test_nowplaying.c ../native/sbbase/src/nowplaying.c)
target_include_directories(test_nowplaying PRIVATE ../native/sbbase/include)
add_test(NAME nowplaying_unit COMMAND test_nowplaying)
add_test(NAME squeezeplay_artwork_patch COMMAND sh
  ${CMAKE_SOURCE_DIR}/tests/comet/test_artwork_patch.sh
  ${CMAKE_SOURCE_DIR}/scripts/patch-squeezeplay-artwork.sh)
add_test(NAME storage COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_storage.sh $<TARGET_FILE:sb-storage-helper>)
add_test(NAME storage_boothelper COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_boothelper.sh ${CMAKE_SOURCE_DIR}/scripts/sbdata-boot.sh)
add_test(NAME extended_storage COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_extended_storage.sh
  ${CMAKE_SOURCE_DIR}/scripts/storage-setup.sh
  ${CMAKE_SOURCE_DIR}/scripts/storage-boot.sh
  $<TARGET_FILE:sb-storage-helper>
  ${CMAKE_SOURCE_DIR}/artifacts/sbubifs-authorized.ko
  ${CMAKE_SOURCE_DIR}/artifacts/ubi/sbdata-empty.ubifs)
find_program(LUA_EXECUTABLE NAMES lua lua5.1)
if(LUA_EXECUTABLE)
  add_test(NAME extended_storage_lua_state COMMAND ${LUA_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_extended_storage_state.lua
    ${CMAKE_SOURCE_DIR}/applet)
endif()
add_test(NAME storage_updater COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_updater.sh $<TARGET_FILE:sb-storage-updater>)
if(NOT WIN32)
  add_test(NAME applet_repository COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/repository/test_repository.sh $<TARGET_FILE:sbwebserver>)
endif()
if(NOT WIN32)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME comet_connections COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_connections.py $<TARGET_FILE:sbbase>)
    add_test(NAME comet_menustatus COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_menustatus.py $<TARGET_FILE:sbbase>)
    add_test(NAME nowplaying_poc COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_nowplaying.py $<TARGET_FILE:sbbase>)
    add_test(NAME nowplaying_api COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_nowplaying_api.py $<TARGET_FILE:sbbase>)
    add_test(NAME lpt_geometry COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_lpt_geometry.py)
    add_test(NAME superblock_patch COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_superblock_patch.py)
  endif()
endif()
