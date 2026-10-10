#!/bin/sh
set -eu
helper=$1
mkfs=$2
runner=${3:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/root"
printf 'write-verify-grow fixture\n' >"$tmp/root/sentinel.txt"
expected=$(sha256sum "$tmp/root/sentinel.txt" | cut -d' ' -f1)
"$mkfs" -r "$tmp/root" -m 2048 -e 129024 -c 521 -x lzo -o "$tmp/test.img"
out=$($runner "$helper" inspect-superblock "$tmp/test.img")
echo "$out" | grep -q '^valid=true$'
echo "$out" | grep -q '^format=w4/r0$'
echo "$out" | grep -q '^min_io_size=2048$'
echo "$out" | grep -q '^leb_size=129024$'
echo "$out" | grep -q '^max_leb_cnt=521$'
cp "$tmp/test.img" "$tmp/bad.img"
printf '\000' | dd of="$tmp/bad.img" bs=1 count=1 conv=notrunc 2>/dev/null
set +e
bad=$($runner "$helper" inspect-superblock "$tmp/bad.img")
status=$?
set -e
test "$status" = 2
echo "$bad" | grep -q '^valid=false$'
echo "$bad" | grep -q '^reason=not_ubifs_superblock$'
dd if="$tmp/test.img" of="$tmp/short.img" bs=64 count=1 2>/dev/null
set +e
$runner "$helper" inspect-superblock "$tmp/short.img" >/dev/null 2>&1
status=$?
set -e
test "$status" = 65
# Unsupported filesystem size must fail without leaving a usable image.
set +e
"$mkfs" -r "$tmp/root" -m 2048 -e 129024 -c 10 -x lzo -o "$tmp/too-small.img" >/dev/null 2>&1
status=$?
set -e
test "$status" != 0
# Repeated compact generation remains valid and bounded.
rm -f "$tmp/test.img"
"$mkfs" -r "$tmp/root" -m 2048 -e 129024 -c 521 -x lzo -o "$tmp/test.img"
out=$($runner "$helper" inspect-superblock "$tmp/test.img")
echo "$out" | grep -q '^leb_cnt=14$'
echo "$out" | grep -q '^max_leb_cnt=521$'
test -n "$expected"
echo image-tests-ok
