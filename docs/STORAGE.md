# Storage management assessment

The active proposal is a separate dynamic UBI volume named exactly `sbdata`; see `SBDATA.md`. Production volumes `kernel`, `kernel_bak`, `cramfs`, `cramfs_bak`, and `ubifs` are immutable. Any other name containing `ubifs` is explicitly rejected because the stock `/linuxrc` lookup can make such a volume boot-critical.

`sb-storage-helper info` and `check` remain compatible read-only interfaces. `storage-status` reports the production and optional `sbdata` topology, filesystem capacities, applet visibility, bind state, boot integration, and an `absent`, `partial`, or `complete` installation state. `prepare` performs read-only preflight and prints a proposed 64 MiB/521-LEB plan with a 64-LEB safety reserve. `verify` validates an existing installation without changing it.

`install` is deliberately implemented only as a blocked interface and exits 78. `expand` also exits 78 permanently. There is no resize ioctl or arbitrary target-volume path in the helper. `inspect-superblock FILE` remains available for read-only UBIFS diagnostics.

The historical builder in `tools/ubi` pins mtd-utils commit `bb56df1e1a84304ec4a14169e4cdc41116ed4256` (2009-06-05, mkfs.ubifs 1.3). Its offline image uses minimum I/O 2048, LEB size 129024, LZO, `w4/r0`, and `max_leb_cnt=521`. It is evidence for the proposed format, not authorization to write flash. Docker remains the canonical clean build; the checked artifact was produced by the equivalent pinned WSL build because Docker Desktop was unavailable.

Mainline Linux 2.6.27 UBIFS growth is capped by `max_leb_cnt`. The Radio's 2.6.26.8-rt16 UBIFS is a vendor backport whose matching source has not been located. The inspected 9.0.1 production filesystem has `max_leb_cnt=82`, confirming that its original filesystem cannot simply consume a larger UBI allocation. These findings motivated the separate-volume design.

Primary sources:

- <https://github.com/torvalds/linux/blob/v2.6.26/include/mtd/ubi-user.h>
- <https://github.com/torvalds/linux/blob/v2.6.26/drivers/mtd/ubi/cdev.c>
- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/sb.c>
- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/super.c>
