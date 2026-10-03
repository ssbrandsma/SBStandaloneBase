# Storage backup and recovery notes

The writable overlay contains configuration, installed applets, permissions, and symlinks. A valid backup must be streamed off-device, checksummed, listed, and test-restored. `tar -p` preserves modes; running as root preserves ownership, and the default archive representation preserves symlinks. ACLs and extended attributes require separate verification because the Radio's BusyBox tar may not support them.

The boot script mounts `ubi0:ubifs` and places it above the read-only CramFS using UnionFS. Consequently, the production volume cannot be treated as an ordinary inactive data mount while the system is running. An eventual production procedure should execute before the overlay becomes the active root or from a reviewed recovery environment.

The observed factory-reset branch unmounts storage and invokes `flash_eraseall` using an MTD variable. Its exact variable assignment and relationship to UBI must be recovered from the matching firmware source/image before relying on it. Factory reset and firmware upgrade may erase or selectively remove overlay files; neither is currently a supported rollback mechanism.

Recovery design requirements for the next milestone are: serial-console access, a verified bootable recovery path, exact MTD/UBI mapping, sufficient external storage, a test-restored archive, power-loss planning, and commands that resolve protected volumes by both ID and name immediately before execution.
