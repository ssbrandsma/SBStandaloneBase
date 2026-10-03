# Read-only `sbdata` preflight

The former `ubifs_test` growth procedure is retired. Never create a volume whose name contains `ubifs`; the stock `/linuxrc` substring lookup can select multiple devices and prevent boot.

The next hardware milestone is read-only only:

```sh
scp -O build-arm/sb-storage-helper root@RADIO_IP:/tmp/sb-storage-helper
ssh root@RADIO_IP 'chmod 755 /tmp/sb-storage-helper'
ssh root@RADIO_IP '/tmp/sb-storage-helper storage-status'
ssh root@RADIO_IP '/tmp/sb-storage-helper prepare'
ssh root@RADIO_IP 'cat /proc/version; cat /proc/filesystems; cat /proc/mounts; cat /proc/mtd'
ssh root@RADIO_IP 'for p in /sys/class/ubi/ubi0 /sys/class/ubi/ubi0/ubi0_*; do echo "[$p]"; for f in name type reserved_ebs usable_eb_size corrupted; do test -r "$p/$f" && echo "$f=$(cat "$p/$f")"; done; done'
ssh root@RADIO_IP 'ls -ld /dev/ubi0* /usr/share/jive/applets /etc/init.d/rcS.local /mnt/storage/standalonebase 2>&1'
```

These commands do not create, format, resize, mount, unmount, migrate, or reboot anything. Save the output for review. `install` and `expand` must both return exit code 78. No physical modification is approved by this document.
