# Physical `sb-storage-helper` validation

## Test identity

- Date: 2026-10-04 (Europe/Amsterdam)
- Target: `root@192.168.1.222`
- Starting revision: `c7fef5f`
- Detection-fix revision: `8e3e377de05eb5d3f3218bb397692b83c0987aae`
- Local and retrieved executable SHA-256: `9377581d9e489e20bd205d8af075d0414724ca335a8db2c2a5707519befe31cc`
- Binary: ELF32 ARM, EABI5, soft-float, statically linked, stripped; no interpreter or dynamic section
- Transfer destination: `/tmp/sb-storage-helper` only

No NAND/UBI volume, persistent configuration, mount, boot file, applet, or system setting was changed. The device was not rebooted. The only device writes were the helper and its executable mode in RAM-backed `/tmp`.

## Initial device identification

```text
$ uname -a
Linux SqueezeboxRadio 2.6.26.8-rt16 #1 PREEMPT RT Fri Feb 14 09:02:51 PST 2014 armv5tejl GNU/Linux

$ cat /etc/squeezeos.version
7.7.3 r16676
root@ec2mbubld01.idc.logitech.com Fri Feb 14 09:25:26 PST 2014
Base build revision:  bad080aecfec8226a4c1699b29d32cbba4ba396b

$ cat /proc/mtd
dev:    size   erasesize  name
mtd0: 00080000 00020000 "redboot"
mtd1: 07ec0000 00020000 "ubi"
mtd2: 002d4800 0001f800 "kernel_bak"
mtd3: 00d0b000 0001f800 "cramfs_bak"
mtd4: 00a17000 0001f800 "ubifs"
mtd5: 002d4800 0001f800 "kernel"
mtd6: 00d0b000 0001f800 "cramfs"

$ cat /proc/mounts
rootfs / rootfs rw 0 0
/dev/root / cramfs ro 0 0
ubi0:ubifs /mnt/storage ubifs rw,noatime 0 0
none / unionfs rw,dirs=/mnt/storage=rw:/=ro 0 0
none /sys sysfs rw 0 0
none /proc proc rw 0 0
none /dev tmpfs rw,mode=755 0 0
devpts /dev/pts devpts rw,mode=600 0 0
ramfs /tmp ramfs rw 0 0
ramfs /var ramfs rw 0 0
debugfs /sys/kernel/debug debugfs rw 0 0
```

## Bugs found and corrected

The first run retained all three lines of `/etc/squeezeos.version`, causing a false `unsupported_firmware`, and looked for incorrect stock applet sentinel names. The source now compares only the first firmware line and validates the physically observed `SetupAppletInstaller`. Fixtures and the boot helper use the same sentinel. The helper was rebuilt, QEMU/fixture tests passed, and the replacement `/tmp` copy was retrieved byte-for-byte and matched SHA-256 before the final run. Safety checks were not relaxed.

## Final helper output

`storage-status` completed with exit code 0:

```text
schema=2
model=Logitech MX25 Baby Board
firmware=7.7.3 r16676
kernel=2.6.26.8-rt16
ubi_device=ubi0
available_lebs=660
volumes=0:kernel_bak,1:cramfs_bak,2:ubifs,3:kernel,4:cramfs
sbdata_present=false
sbdata_id=-1
sbdata_type=
sbdata_lebs=-1
sbdata_leb_size=-1
sbdata_corrupted=-1
sbdata_mounted=false
bind_mounted=false
applet_directory=present
standard_applets=present
standalone_base=present
original_total_bytes=8130560
original_free_bytes=5316608
sbdata_total_bytes=0
sbdata_free_bytes=0
boot_helper=missing
rcs_local=missing
rcs_integrated=false
installation_state=absent
```

`prepare` completed its read-only assessment and the SSH transport reported exit code 1 for the helper's non-ready result:

```text
schema=2
model=Logitech MX25 Baby Board
firmware=7.7.3 r16676
kernel=2.6.26.8-rt16
ubi_device=ubi0
available_lebs=660
volumes=0:kernel_bak,1:cramfs_bak,2:ubifs,3:kernel,4:cramfs
sbdata_present=false
sbdata_id=-1
sbdata_type=
sbdata_lebs=-1
sbdata_leb_size=-1
sbdata_corrupted=-1
sbdata_mounted=false
bind_mounted=false
applet_directory=present
standard_applets=present
standalone_base=present
original_total_bytes=8130560
original_free_bytes=5316608
sbdata_total_bytes=0
sbdata_free_bytes=0
boot_helper=missing
rcs_local=missing
rcs_integrated=false
installation_state=absent
prepare_ready=false
prepare_reason=missing_compatible_mkfs_ubifs
planned_volume=sbdata
planned_bytes=67108864
planned_lebs=521
minimum_free_reserve_lebs=64
install_enabled=false
```

Legacy `info` and `check` both reported `compatible=true`, `reason=supported_read_only`; both completed successfully.

## Independent UBI verification

`ubinfo -a` reported one UBI device, 1014 total LEBs, 660 available LEBs, 10 reserved physical eraseblocks, zero bad PEBs, 129024-byte LEBs, and 2048-byte minimum I/O. Every volume was dynamic and `State: OK`:

| ID | Expected | Actual | LEBs |
|---:|---|---|---:|
| 0 | kernel_bak | kernel_bak | 23 |
| 1 | cramfs_bak | cramfs_bak | 106 |
| 2 | ubifs | ubifs | 82 |
| 3 | kernel | kernel | 23 |
| 4 | cramfs | cramfs | 106 |

Exactly those five volumes exist. Neither `sbdata` nor `ubifs_test` exists. `/dev/ubi0` and `/dev/ubi0_0` through `_4` are character devices. `/proc/filesystems` includes UBIFS. `/usr/sbin/ubimkvol` and `/usr/sbin/ubiupdatevol` exist; `mkfs.ubifs` does not. `/etc/init.d/rcS.local` does not exist. `SetupAppletInstaller`, `StandaloneBase`, and the existing installed applets are visible.

The proposed 521 LEB allocation would leave 139 free LEBs (`660 - 521`), exceeding the configured 64-LEB reserve by 75 LEBs. This establishes capacity eligibility only.

## Verdict

PASS for read-only hardware discovery after the documented detection fix. Model, firmware, kernel, UBI identity, volume layout, corruption state, mount state, geometry, free capacity, applet visibility, and installation absence are reported correctly.

BLOCKED for any installation: no compatible ARM `mkfs.ubifs` is present, so `prepare` correctly returns `missing_compatible_mkfs_ubifs`. The device is eligible for further preparation but is not approved for flash migration.

## Remaining risks and next smallest test

Before the first NAND write, provide and independently validate a static ARMv5 historical `mkfs.ubifs`, confirm its output remains `w4/r0`, design an external backup/restore rehearsal, review exact `rcS.local` merge and rollback commands, and define a power-loss boundary. The next smallest hardware test is to copy a candidate formatter to `/tmp` only, verify its ABI/hash/version, and run only `--version` or help plus off-device image inspection. Do not create `sbdata`, format flash, mount it, edit boot files, migrate applets, or reboot.
