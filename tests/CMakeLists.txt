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
  add_test(NAME storage_recovery_menu COMMAND ${LUA_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_recovery_menu.lua ${CMAKE_SOURCE_DIR})
  add_test(NAME storage_recovery_manager COMMAND ${LUA_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_storage_recovery_manager.lua
    ${CMAKE_SOURCE_DIR})
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
    add_test(NAME storage_firmware_recovery COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/storage/test_firmware_recovery.py
      ${CMAKE_SOURCE_DIR}/scripts/storage-setup.sh
      ${CMAKE_SOURCE_DIR}/scripts/storage-boot.sh $<TARGET_FILE:sb-storage-helper>
      ${CMAKE_SOURCE_DIR}/artifacts/sbubifs-authorized.ko)
    find_package(Threads REQUIRED)
    add_executable(test_catalog_merge repository/test_catalog_merge.c
      ../native/sbwebserver/src/repository.c ../native/sbwebserver/third_party/mongoose.c)
    target_include_directories(test_catalog_merge PRIVATE
      ../native/sbwebserver/src ../native/sbwebserver/third_party)
    target_compile_definitions(test_catalog_merge PRIVATE _GNU_SOURCE MG_ENABLE_EPOLL=0 MG_ENABLE_POLL=1)
    target_link_libraries(test_catalog_merge PRIVATE Threads::Threads)
    add_test(NAME catalog_merge COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/repository/test_catalog_merge.py $<TARGET_FILE:test_catalog_merge>)
    get_target_property(CATALOG_WEB_SOURCES sbwebserver SOURCES)
    set(CATALOG_TEST_SOURCES)
    foreach(source IN LISTS CATALOG_WEB_SOURCES)
      list(APPEND CATALOG_TEST_SOURCES "${CMAKE_SOURCE_DIR}/native/sbwebserver/${source}")
    endforeach()
    add_executable(sbwebserver_catalog_test ${CATALOG_TEST_SOURCES})
    target_include_directories(sbwebserver_catalog_test PRIVATE
      ../native/sbwebserver/src ../native/sbwebserver/third_party)
    target_compile_definitions(sbwebserver_catalog_test PRIVATE _GNU_SOURCE SB_CATALOG_TESTING
      MG_ENABLE_EPOLL=0 MG_ENABLE_POLL=1 MG_ENABLE_DIRLIST=0 MG_ENABLE_SSI=0)
    target_link_libraries(sbwebserver_catalog_test PRIVATE Threads::Threads)
    add_test(NAME catalog_sync COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/repository/test_catalog_sync.py $<TARGET_FILE:sbwebserver_catalog_test>
      ${CMAKE_SOURCE_DIR}/native/sbwebserver/web)
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
    add_test(NAME comet_server_switch COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/comet/test_server_switch.py $<TARGET_FILE:sbbase>)
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
    set_tests_properties(applet_repository svg_image_http remote_http
      radio_management_http web_access_pairing system_log_viewer process_monitor
      PROPERTIES ENVIRONMENT "SB_CATALOG_SYNC_DISABLED=1")
  endif()
endif()
