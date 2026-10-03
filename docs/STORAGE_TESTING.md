# Isolated storage validation

These commands are a proposal for manual review. They are not run by any build, test, applet, or installer. Every modifying command must be approved separately. Never substitute `ubifs`, `kernel`, `kernel_bak`, `cramfs`, or `cramfs_bak` for `ubifs_test`.

## 1. Backup outside the Radio

From a Linux/WSL workstation, while the Radio is otherwise idle:

```sh
mkdir -p radio-backup
ssh root@RADIO_IP 'sync; tar -C /mnt/storage -cpf - .' >radio-backup/mnt-storage.tar
sha256sum radio-backup/mnt-storage.tar >radio-backup/mnt-storage.tar.sha256
tar -tf radio-backup/mnt-storage.tar >/dev/null
ssh root@RADIO_IP 'find /mnt/storage -xdev -printf "%y %m %u %g %s %p -> %l\n"' >radio-backup/manifest.txt
```

Keep the archive and checksum off-device. Before any production milestone, restore this archive to a disposable UBIFS volume or equivalent test environment and compare the manifest. Do not treat archive creation alone as verified recovery.

## 2. Revalidate the isolated target

Read-only commands immediately before every modifying phase:

```sh
cat /sys/class/ubi/ubi0/ubi0_5/name
cat /sys/class/ubi/ubi0/ubi0_5/type
cat /sys/class/ubi/ubi0/ubi0_5/reserved_ebs
cat /sys/class/ubi/ubi0/ubi0_5/usable_eb_size
grep -w ubifs_test /proc/mounts || true
cat /sys/class/ubi/ubi0/avail_eraseblocks
```

Stop unless the discovered path (not an assumed ID) has name `ubifs_test`, type `dynamic`, is unmounted, and capacity matches the reviewed plan. Resolve the ID by scanning every `ubi0_*` name file each time.

## 3. Create a compatible image on the workstation

Use a pinned mtd-utils release in a container after recording its version. Substitute the values read from the Radio; do not copy the examples blindly.

```sh
MIN_IO=2048
LEB_SIZE=129024
MAX_LEBS=521
mkdir -p empty-root
mkfs.ubifs -r empty-root -m "$MIN_IO" -e "$LEB_SIZE" -c "$MAX_LEBS" -o ubifs-test.img
sha256sum ubifs-test.img
```

The `-c 521` maximum is essential to testing later growth. Confirm the tool does not enable authentication, encryption, or newer incompatible format features. Record `mkfs.ubifs --version` and inspect the image with a compatible `ubireader`/dump tool before transfer.

## 4. Separately approved flash phases

After repeating section 2, transfer the image and, only with explicit approval, write it to the named test volume:

```sh
ubiupdatevol /dev/ubi0_5 /tmp/ubifs-test.img
mkdir -p /mnt/ubifs-test
mount -t ubifs ubi0:ubifs_test /mnt/ubifs-test
printf 'StandaloneBase storage validation\n' >/mnt/ubifs-test/sentinel.txt
sha256sum /mnt/ubifs-test/sentinel.txt >/mnt/ubifs-test/sentinel.sha256
sync
umount /mnt/ubifs-test
```

Revalidate name, ID, type, mount state, corruption marker, and free PEBs again. In a separately approved phase, enlarge only `ubifs_test`:

```sh
ubirsvol /dev/ubi0 -N ubifs_test -s 8MiB
mount -t ubifs ubi0:ubifs_test /mnt/ubifs-test
df -k /mnt/ubifs-test
sha256sum -c /mnt/ubifs-test/sentinel.sha256
```

Record kernel messages, sysfs sizes, `df`, mount behavior, and whether growth occurs on first mount, remount, or reboot. Perform a clean reboot-persistence check only after the checksum passes. Deletion of the test volume is a later, separately approved action.

## Failure stops

Stop on ambiguous naming, a mounted target, any corruption/update marker, unexpected UBI count, reduced free PEBs, I/O/ECC errors, mount failure, capacity not increasing, checksum mismatch, or any command resolving to a protected volume. Do not attempt repair or production resize during this milestone.
