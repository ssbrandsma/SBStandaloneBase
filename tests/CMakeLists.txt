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
  add_test(NAME artwork_lua COMMAND ${LUA_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/images/test_artwork.lua ${CMAKE_SOURCE_DIR})
  add_test(NAME image_helper_lua COMMAND ${LUA_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/images/test_image_helper.lua ${CMAKE_SOURCE_DIR})
  add_test(NAME extended_storage_lua_state COMMAND ${LUA_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_extended_storage_state.lua
    ${CMAKE_SOURCE_DIR}/applet)
  add_test(NAME server_migration_lua COMMAND ${LUA_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_server_migration.lua
    ${CMAKE_SOURCE_DIR})
  add_test(NAME network_address_lua COMMAND ${LUA_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_network_address.lua
    ${CMAKE_SOURCE_DIR})
endif()

find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
  add_test(NAME process_monitor_frontend COMMAND ${NODE_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/monitor/test_performance_logic.js
    ${CMAKE_SOURCE_DIR}/native/sbwebserver/web/js/performance.js)
endif()
add_test(NAME storage_updater COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_updater.sh $<TARGET_FILE:sb-storage-updater>)
add_executable(test_remote_framebuffer remote/test_framebuffer.c ../native/sbwebserver/src/remote_framebuffer.c)
target_include_directories(test_remote_framebuffer PRIVATE ../native/sbwebserver/src)
add_test(NAME remote_framebuffer COMMAND test_remote_framebuffer)
if(NOT WIN32)
  add_test(NAME applet_repository COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/repository/test_repository.sh $<TARGET_FILE:sbwebserver>)
endif()
if(NOT WIN32)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_executable(test_public_address images/test_public_address.c)
    add_test(NAME image_public_address COMMAND test_public_address)
    add_test(NAME svg_renderer COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/images/test_renderer.py $<TARGET_FILE:svgpngrenderer>)
    add_test(NAME svg_image_http COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/images/test_image_http.py $<TARGET_FILE:sbwebserver>
      $<TARGET_FILE:svgpngrenderer> ${CMAKE_SOURCE_DIR}/native/sbwebserver/web)
    if(TARGET sbproxy)
      find_package(CURL REQUIRED)
      find_package(Threads REQUIRED)
      add_executable(sbproxy_test ../native/sbproxy/sbproxy.c
        ../native/sbproxy/hls_playlist.c ../native/sbproxy/hls_audio.c ../native/sbproxy/hls_relay.c
        ../native/sbproxy/hls_id3.c ../native/sbproxy/hls_metadata.c)
      target_compile_definitions(sbproxy_test PRIVATE _GNU_SOURCE SB_PROXY_TESTING)
      target_link_libraries(sbproxy_test PRIVATE CURL::libcurl Threads::Threads)
      add_executable(test_hls images/test_hls.c ../native/sbproxy/hls_playlist.c ../native/sbproxy/hls_audio.c ../native/sbproxy/hls_id3.c)
      add_executable(test_hls_id3 images/test_hls_id3.c ../native/sbproxy/hls_id3.c)
      add_test(NAME hls_id3 COMMAND test_hls_id3)
      add_executable(test_hls_wait images/test_hls_wait.c)
      target_link_libraries(test_hls_wait PRIVATE Threads::Threads)
      add_test(NAME hls_metadata_wait COMMAND test_hls_wait)
      target_link_libraries(test_hls PRIVATE CURL::libcurl)
      add_test(NAME hls_native COMMAND test_hls)
      add_test(NAME hls_streaming COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/images/test_hls_http.py $<TARGET_FILE:sbproxy_test>)
      set_tests_properties(hls_streaming PROPERTIES ENVIRONMENT "SB_HLS_TEST_TIMEOUT=1")
      add_test(NAME artwork_proxy_streaming COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/images/test_artwork_proxy.py $<TARGET_FILE:sbproxy_test>)
      add_test(NAME image_proxy_guard COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/images/test_proxy_guard.py $<TARGET_FILE:sbproxy>)
    endif()
    add_test(NAME comet_connections COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_connections.py $<TARGET_FILE:sbbase>)
    add_test(NAME comet_menustatus COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_menustatus.py $<TARGET_FILE:sbbase>)
    add_test(NAME nowplaying_poc COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_nowplaying.py $<TARGET_FILE:sbbase>)
    add_test(NAME nowplaying_api COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_nowplaying_api.py $<TARGET_FILE:sbbase>)
    add_test(NAME lpt_geometry COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_lpt_geometry.py)
    add_test(NAME superblock_patch COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_superblock_patch.py)
    add_test(NAME remote_http COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/remote/test_remote_http.py $<TARGET_FILE:sbwebserver> ${CMAKE_SOURCE_DIR}/native/sbwebserver/web)
    add_test(NAME radio_management_http COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/radio/test_radio_http.py $<TARGET_FILE:sbwebserver> ${CMAKE_SOURCE_DIR}/native/sbwebserver/web)
    add_test(NAME web_access_pairing COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/auth/test_web_pairing.py $<TARGET_FILE:sbwebserver> ${CMAKE_SOURCE_DIR}/native/sbwebserver/web)
    add_test(NAME system_log_viewer COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/logs/test_logs_http.py $<TARGET_FILE:sbwebserver> ${CMAKE_SOURCE_DIR}/native/sbwebserver/web)
    add_test(NAME process_monitor COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/monitor/test_monitor_http.py $<TARGET_FILE:sbwebserver> ${CMAKE_SOURCE_DIR}/native/sbwebserver/web)
    add_test(NAME cpu_accounting_diagnostic COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/monitor/test_cpu_diagnostic.py)
  endif()
endif()
