# SBData NAND-test recovery boundary

This document records the only recovery operation currently verified from the
running SqueezeOS userspace. It is not a complete offline recovery procedure.

If Linux still boots, `sbdata` is unmounted, and both sysfs and `ubinfo -a`
identify exactly one dynamic 521-LEB volume named exactly `sbdata`, a separately
authorized rollback may remove only that volume:

```sh
umount /mnt/sbdata 2>/dev/null || true
# Stop if it was mounted and the unmount failed.
ubinfo -a
# Independently verify the discovered ubi0_N name, type, size and health.
/usr/sbin/ubirmvol /dev/ubi0 -N sbdata
```

Do not substitute an ID, do not remove automatically after another command
fails, and do not proceed if any identity check is ambiguous. After removal,
verify the original volume map, 660 available LEBs, `/proc/mtd`, production
mount and the single `grep ubifs /proc/mtd` result.

If Linux does not boot, there is currently no approved recovery procedure.
Although the RedBoot partition and COM5 console exist, the exact commands,
load transport, compatible images, addresses and restoration sequence have not
been established or rehearsed. Do not improvise RedBoot commands, invoke the
stock factory-reset branch, erase MTD1, or write the online UBIFS dump until a
separate recovery milestone validates those details.

Required before the first NAND test:

1. Obtain or capture the exact bootloader, kernel, CramFS and UBI recovery
   material needed for this hardware and firmware.
2. Document verified RedBoot entry and boot-to-recovery commands without
   modifying RedBoot configuration.
3. Establish how recovery Linux attaches `mtd1` and restores the UBI layout.
4. Make an offline/atomic production UBIFS backup or prove a safe restoration
   method for the existing online dump and file archive.
5. Rehearse restoration on non-production media or equivalent hardware.

Until those items are complete, `docs/SBDATA_FIRST_NAND_TEST.md` remains
blocked and is not authorization to execute its Phase B commands.
