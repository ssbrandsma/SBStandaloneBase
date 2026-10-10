add_executable(sb-storage-helper sb_storage_helper.c)
target_compile_options(sb-storage-helper PRIVATE -Wall -Wextra -Wpedantic)
add_executable(sb-storage-updater sb_storage_updater.c)
target_compile_options(sb-storage-updater PRIVATE -Wall -Wextra -Wpedantic)
