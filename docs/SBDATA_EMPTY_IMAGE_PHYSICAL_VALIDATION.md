# `sbdata-empty.ubifs` physical validation procedure

This procedure is destructive only to the dedicated UBI0 volume ID 5 named
exactly `sbdata`. It must not be run without separate approval and serial-console
recovery access. It never targets the production `ubifs` volume.

The exact packaged image is:

```text
path:   /usr/share/jive/applets/StandaloneBase/sbdata-empty.ubifs
size:   1806336 bytes
SHA256: 34f26e33d80393c1f1f497e85dff6b78dbc6fc0e2df98ed1150ea45c6631065d
format: w4/r0, min I/O 2048, LEB 129024, max LEB count 521, LZO
```

The reproducible build script is `tools/ubi/make-empty-sbdata-image.sh`. Two
independent test rebuilds had the same size and decoded geometry but different
digests. Byte comparison localized the primary payload difference to the fresh
filesystem UUID (with corresponding CRC changes), which is expected from this
legacy tool. Runtime initialization accepts only the version-controlled digest
above, not an arbitrary rebuild.

## Stage 0: immutable preflight

Do not proceed if any command differs from the expected value.

```sh
cd /usr/share/jive/applets/StandaloneBase
./sb-storage-helper verify-sbdata-image ./sbdata-empty.ubifs
./sb-storage-helper verify-module ./sbubifs-authorized.ko

test -c /dev/ubi0_5
cat /sys/class/ubi/ubi0_5/name
cat /sys/class/ubi/ubi0_5/type
cat /sys/class/ubi/ubi0_5/reserved_ebs
cat /sys/class/ubi/ubi0_5/data_bytes
cat /sys/class/ubi/ubi0_5/corrupted
cat /sys/class/ubi/ubi0_5/upd_marker
cat /sys/class/ubi/ubi0_5/usable_eb_size
cat /sys/class/ubi/ubi0/min_io_size
grep -E 'ubi0:sbdata|/dev/ubi0_5' /proc/mounts
```

The native `verify-sbdata-image` result is authoritative for both image size
and SHA-256. Do not use the Radio's BusyBox `wc -c` for this binary: physical
validation showed an incorrect count of 1,806,309 bytes, while the actual and
helper-verified size is 1,806,336 bytes.

Required output:

```text
module_valid=true
image_valid=true
name=sbdata
type=dynamic
reserved_ebs=521
data_bytes=67221504
corrupted=0
upd_marker=0
usable_eb_size=129024
min_io_size=2048
no sbdata mount-table match
```

Also capture the production baseline:

```sh
cat /proc/mtd
cat /proc/mounts
cat /proc/filesystems
/usr/sbin/ubinfo /dev/ubi0_5
dmesg | tail -100
```

## Stage 1: separately approved image write

Copy the packaged image to RAM and reverify that exact copy:

```sh
cp /usr/share/jive/applets/StandaloneBase/sbdata-empty.ubifs /tmp/sbdata-empty.ubifs
/usr/share/jive/applets/StandaloneBase/sb-storage-helper \
    verify-sbdata-image /tmp/sbdata-empty.ubifs
```

Only after explicit approval run the one destructive command:

```sh
/usr/sbin/ubiupdatevol /dev/ubi0_5 /tmp/sbdata-empty.ubifs
```

Never use `-t`, `ubiformat`, `ubimkvol`, `ubirmvol`, or `ubinize`.

Immediately verify:

```sh
/usr/sbin/ubinfo /dev/ubi0_5
cat /sys/class/ubi/ubi0_5/corrupted
cat /sys/class/ubi/ubi0_5/upd_marker
```

Both health fields must be zero.

## Stage 2: first read-only mount

```sh
grep -q 'nodev.*sbubifs' /proc/filesystems || \
    insmod /usr/share/jive/applets/StandaloneBase/sbubifs-authorized.ko
grep sbubifs /proc/filesystems
mkdir -p /mnt/sbdata
mount -t sbubifs -o ro ubi0:sbdata /mnt/sbdata
mount
df -h
ls -la /mnt/sbdata
dmesg | tail -100
```

An approximately 510-LEB filesystem inside the 521-LEB UBI allocation is
expected and must not by itself be treated as an error. The earlier validated
profile reported 65,802,240 filesystem bytes and a 69-LEB journal.

## Stage 3: bounded read/write persistence

```sh
umount /mnt/sbdata
mount -t sbubifs ubi0:sbdata /mnt/sbdata
echo "StandaloneBase sbdata test" > /mnt/sbdata/test.txt
sync
cat /mnt/sbdata/test.txt
umount /mnt/sbdata
mount -t sbubifs ubi0:sbdata /mnt/sbdata
cat /mnt/sbdata/test.txt
sync
dmesg | tail -100
```

Capture the final mount table, `ubinfo`, kernel log, file contents, and free
space. Stop on any UBI, UBIFS, SBUBIFS, I/O, corruption, update-marker, or
kernel warning. Do not enable the applet initialization write until every stage
has passed with this exact image digest.
