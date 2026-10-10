#!/bin/sh
set -eu
updater=$1
runner=${2:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

out=$($runner "$updater" plan 1014 139 82 521 40 64)
echo "$out" | grep -q '^max_target_lebs=638$'
echo "$out" | grep -q '^max_target_bytes=82317312$'
echo "$out" | grep -q '^destructive_execution_enabled=false$'
echo "$out" | grep -q '^early_boot_environment_required=true$'

set +e
$runner "$updater" execute >/dev/null 2>&1
status=$?
set -e
test "$status" = 78

printf 'schema=1\nstate=idle\n' >"$tmp/state"
$runner "$updater" status "$tmp/state" | grep -q '^state=idle$'
for transition in \
 'idle backup_written' \
 'backup_written backup_verified' \
 'backup_verified updater_armed' \
 'updater_armed destructive_started' \
 'destructive_started production_removed' \
 'production_removed production_created' \
 'production_created image_written' \
 'image_written restore_started' \
 'restore_started restore_verified' \
 'restore_verified complete'
do
 set -- $transition
 SB_STORAGE_SIMULATION=1 $runner "$updater" advance "$tmp/state" "$1" "$2" >/dev/null
done
$runner "$updater" status "$tmp/state" | grep -q '^state=complete$'

# No skipping, stale expected state, invalid state, or physical transition.
printf 'schema=1\nstate=idle\n' >"$tmp/state"
set +e
SB_STORAGE_SIMULATION=1 $runner "$updater" advance "$tmp/state" idle backup_verified >/dev/null 2>&1; a=$?
SB_STORAGE_SIMULATION=1 $runner "$updater" advance "$tmp/state" backup_written backup_verified >/dev/null 2>&1; b=$?
$runner "$updater" advance "$tmp/state" idle backup_written >/dev/null 2>&1; c=$?
printf 'schema=1\nstate=unknown\n' >"$tmp/bad"
$runner "$updater" status "$tmp/bad" >/dev/null 2>&1; d=$?
set -e
test "$a" = 4
test "$b" = 3
test "$c" = 78
test "$d" = 2
echo updater-tests-ok
