#!/bin/sh
# StandaloneBase Extended Storage inspection and initialization frontend.

ROOT=${SB_STORAGE_ROOT:-}
test -n "$ROOT" || PATH=/sbin:/bin:/usr/sbin:/usr/bin
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
APPLET_DIR=${SB_STORAGE_APPLET_DIR:-$SCRIPT_DIR}
HELPER=$APPLET_DIR/sb-storage-helper
BUNDLED_MODULE=$APPLET_DIR/sbubifs-authorized.ko
SBDATA_IMAGE=$APPLET_DIR/sbdata-empty.ubifs
PERSIST_DIR=$ROOT/mnt/storage/standalonebase
PERSIST_MODULE=$ROOT/mnt/storage/sbubifs-authorized.ko
PERSIST_BOOT=$PERSIST_DIR/storage-boot.sh
RCS_LOCAL=$ROOT/etc/init.d/rcS.local
UBI_CLASS=$ROOT/sys/class/ubi
UBI=$UBI_CLASS/ubi0
UBI_DEVICE=$ROOT/dev/ubi0
VOLUME=$ROOT/sys/class/ubi/ubi0_5
VOLUME_DEVICE=$ROOT/dev/ubi0_5
UBIMKVOL=$ROOT/usr/sbin/ubimkvol
UBIUPDATEVOL=$ROOT/usr/sbin/ubiupdatevol
MOUNT=$ROOT/mnt/sbdata
SOURCE=$ROOT/usr/share/jive/applets
DEST=$MOUNT/applets
MOUNTS=$ROOT/proc/mounts
FILESYSTEMS=$ROOT/proc/filesystems
STATUS_FILE=$ROOT/tmp/sbstorage.status
LOG_FILE=$ROOT/tmp/sbstorage.log
REBOOT_MARKER=$PERSIST_DIR/extended-storage-reboot-required

say() { printf '%s\n' "$*"; }
value() { test -r "$1" && sed -n '1p' "$1" 2>/dev/null || true; }
mounted_type() { awk -v target="$1" '$2 == target { print $3; exit }' "$MOUNTS" 2>/dev/null; }
has_fs() { grep -q '^[[:space:]]*nodev[[:space:]]*sbubifs$' "$FILESYSTEMS" 2>/dev/null; }
bool() { if "$@"; then say 1; else say 0; fi; }

device_node() { test -c "$1" || { test -n "$ROOT" && test -e "$1"; }; }

exact_sbdata() {
    test -d "$VOLUME" \
        && device_node "$VOLUME_DEVICE" \
        && test "$(value "$VOLUME/name")" = sbdata \
        && test "$(value "$VOLUME/type")" = dynamic \
        && test "$(value "$VOLUME/alignment")" = 1 \
        && test "$(value "$VOLUME/reserved_ebs")" = 521 \
        && test "$(value "$VOLUME/data_bytes")" = 67221504 \
        && test "$(value "$VOLUME/usable_eb_size")" = 129024 \
        && test "$(value "$VOLUME/corrupted")" = 0 \
        && test "$(value "$VOLUME/upd_marker")" = 0 \
        && test "$(value "$UBI/min_io_size")" = 2048
}

stock_volume_exact() {
    stock=$UBI_CLASS/ubi0_$1
    test -d "$stock" \
        && test "$(value "$stock/name")" = "$2" \
        && test "$(value "$stock/type")" = dynamic \
        && test "$(value "$stock/reserved_ebs")" = "$3" \
        && test "$(value "$stock/data_bytes")" = "$4" \
        && test "$(value "$stock/corrupted")" = 0 \
        && test "$(value "$stock/upd_marker")" = 0
}

stock_volume_set_exact() {
    stock_volume_exact 0 kernel_bak 23 2967552 || return 1
    stock_volume_exact 1 cramfs_bak 106 13676544 || return 1
    stock_volume_exact 2 ubifs 82 10579968 || return 1
    stock_volume_exact 3 kernel 23 2967552 || return 1
    stock_volume_exact 4 cramfs 106 13676544 || return 1
    for stock in "$UBI_CLASS"/ubi0_*; do
        test -d "$stock" || continue
        case ${stock##*_} in 0|1|2|3|4) ;; *) return 1 ;; esac
    done
}

virgin_layout_exact() {
    test "$(value "$ROOT/etc/squeezeos.version")" = '7.7.3 r16676' \
        && test -d "$UBI" \
        && device_node "$UBI_DEVICE" \
        && test "$(value "$UBI/total_eraseblocks")" = 1014 \
        && test "$(value "$UBI/eraseblock_size")" = 129024 \
        && test "$(value "$UBI/min_io_size")" = 2048 \
        && test "$(value "$UBI/bad_peb_count")" = 0 \
        && test "$(value "$UBI/reserved_for_bad")" = 10 \
        && test "$(value "$UBI/avail_eraseblocks")" = 660 \
        && stock_volume_set_exact \
        && test ! -e "$VOLUME" \
        && test ! -e "$VOLUME_DEVICE" \
        && test -z "$(mounted_type "$MOUNT")" \
        && test -z "$(mounted_type "$SOURCE")"
}

status_snapshot() {
    platform=$(value "$ROOT/proc/cpuinfo" | sed 's/^[^:]*:[[:space:]]*//')
    kernel=$(value "$ROOT/proc/sys/kernel/osrelease")
    firmware=$(value "$ROOT/etc/squeezeos.version")
    volume_present=0; volume_name=; volume_id=5
    test -r "$VOLUME/name" && volume_present=1 && volume_name=$(value "$VOLUME/name")
    volume_type=$(value "$VOLUME/type")
    volume_corrupted=$(value "$VOLUME/corrupted")
    volume_update_marker=$(value "$VOLUME/upd_marker")
    driver_loaded=0; has_fs && driver_loaded=1
    sbdata_mounted=0; test "$(mounted_type "$MOUNT")" = sbubifs && sbdata_mounted=1
    bind_active=0; test "$(mounted_type "$SOURCE")" = sbubifs && bind_active=1
    total=0; used=0; free=0
    if test "$sbdata_mounted" = 1 && command -v df >/dev/null 2>&1; then
        set -- $(df -k "$MOUNT" 2>/dev/null | awk 'NR==2 {print $2, $3, $4}')
        test "$#" = 3 && total=$1 && used=$2 && free=$3
    fi
    init_supported=0
    init_reason=preflight_requirements_not_met
    production=$ROOT/mnt/storage
    base_supported=1
    test -d "$UBI" || base_supported=0
    test "$(mounted_type "$production")" = ubifs || base_supported=0
    test -d "$SOURCE" || base_supported=0
    test -f "$ROOT/etc/init.d/rcS" && grep -q 'rcS.local' "$ROOT/etc/init.d/rcS" 2>/dev/null || base_supported=0
    state=UNAVAILABLE
    if test "$base_supported" != 1; then state=UNSUPPORTED
    elif { test -e "$VOLUME" || test -e "$VOLUME_DEVICE"; } && ! exact_sbdata; then state=ERROR
    elif test "$bind_active" = 1 && test "$sbdata_mounted" = 1 && test "$driver_loaded" = 1; then state=ACTIVE
    elif test -f "$REBOOT_MARKER"; then state=REBOOT_REQUIRED
    elif exact_sbdata || virgin_layout_exact; then state=AVAILABLE
    fi
    if test "$state" = AVAILABLE \
        && test -x "$HELPER" \
        && "$HELPER" verify-module "$BUNDLED_MODULE" >/dev/null 2>&1 \
        && "$HELPER" verify-sbdata-image "$SBDATA_IMAGE" >/dev/null 2>&1 \
        && test -x "$UBIUPDATEVOL" \
        && { exact_sbdata || test -x "$UBIMKVOL"; }; then
        init_supported=1
        if exact_sbdata; then init_reason=validated_packaged_image
        else init_reason=validated_virgin_layout; fi
    fi
    say "STATUS=$state"
    say "PLATFORM=$platform"
    say "KERNEL_VERSION=$kernel"
    say "FIRMWARE_VERSION=$firmware"
    say "MTD_LAYOUT_PRESENT=$(test -r "$ROOT/proc/mtd" && echo 1 || echo 0)"
    say "UBI_AVAILABLE=$(test -d "$UBI" && echo 1 || echo 0)"
    say "PRODUCTION_UBIFS=$(awk -v target="$production" '$2 == target && $3 == "ubifs" { print 1; found=1; exit } END { if (!found) print 0 }' "$MOUNTS" 2>/dev/null)"
    say "VOLUME_PRESENT=$volume_present"
    say "VOLUME_ID=$volume_id"
    say "VOLUME_NAME=$volume_name"
    say "VOLUME_TYPE=$volume_type"
    say "VOLUME_CORRUPTED=$volume_corrupted"
    say "VOLUME_UPDATE_MARKER=$volume_update_marker"
    say "AVAILABLE_LEBS=$(value "$ROOT/sys/class/ubi/ubi0/avail_eraseblocks")"
    say "APPLET_SOURCE_PRESENT=$(test -d "$SOURCE" && echo 1 || echo 0)"
    say "RCS_LOCAL_SUPPORTED=$(test -f "$ROOT/etc/init.d/rcS" && grep -q 'rcS.local' "$ROOT/etc/init.d/rcS" 2>/dev/null && echo 1 || echo 0)"
    say "BUNDLED_MODULE_PRESENT=$(test -f "$BUNDLED_MODULE" && echo 1 || echo 0)"
    say "BUNDLED_MODULE_VALID=$(test -x "$HELPER" && test -f "$BUNDLED_MODULE" && "$HELPER" verify-module "$BUNDLED_MODULE" >/dev/null 2>&1 && echo 1 || echo 0)"
    say "SBDATA_IMAGE_PRESENT=$(test -f "$SBDATA_IMAGE" && echo 1 || echo 0)"
    say "SBDATA_IMAGE_VALID=$(test -x "$HELPER" && test -f "$SBDATA_IMAGE" && "$HELPER" verify-sbdata-image "$SBDATA_IMAGE" >/dev/null 2>&1 && echo 1 || echo 0)"
    say "DRIVER_LOADED=$driver_loaded"
    say "SBDATA_MOUNTED=$sbdata_mounted"
    say "APPLET_BIND_ACTIVE=$bind_active"
    say "TOTAL_KB=$total"
    say "USED_KB=$used"
    say "FREE_KB=$free"
    say "ACTIVE=$(test "$state" = ACTIVE && echo 1 || echo 0)"
    say "AVAILABLE=$(test "$state" = AVAILABLE && echo 1 || echo 0)"
    say "INITIALIZATION_SUPPORTED=$init_supported"
    say "INITIALIZATION_REASON=$init_reason"
}

stage() {
    STAGE=$1
    printf 'STAGE=%s\n' "$STAGE" >"$STATUS_FILE"
    printf '%s STAGE=%s\n' "$(date '+%Y-%m-%dT%H:%M:%S' 2>/dev/null || echo unknown-time)" "$STAGE" >>"$LOG_FILE"
}

die() {
    code=$1; shift
    printf 'ERROR=%s\nEXIT_CODE=%s\n' "$*" "$code" >>"$STATUS_FILE"
    printf '%s ERROR=%s EXIT_CODE=%s\n' "$(date '+%Y-%m-%dT%H:%M:%S' 2>/dev/null || echo unknown-time)" "$*" "$code" >>"$LOG_FILE"
    exit "$code"
}

initialize_sbdata_filesystem() {
    printf '%s READY: all destructive preconditions and image identity verified\n' \
        "$(date '+%Y-%m-%dT%H:%M:%S' 2>/dev/null || echo unknown-time)" >>"$LOG_FILE"
    "$UBIUPDATEVOL" "$VOLUME_DEVICE" "$SBDATA_IMAGE" >>"$LOG_FILE" 2>&1
}

count_type() { find "$1" -type "$2" -print 2>/dev/null | wc -l | tr -d ' '; }

install_rcs_hook() {
    marker='# STANDALONEBASE-EXTENDED-STORAGE'
    tmp=$ROOT/tmp/rcS.local.sbstorage.$$
    if test -e "$RCS_LOCAL"; then cp -p "$RCS_LOCAL" "$tmp" || return 1; else printf '#!/bin/sh\n' >"$tmp" || return 1; fi
    if ! grep -qF "$marker" "$tmp"; then
        merged=$tmp.merged
        awk -v marker="$marker" '
            !done && $0 ~ /^[[:space:]]*exit[[:space:]]+0[[:space:]]*$/ {
                print ""; print marker; print "/bin/sh /mnt/storage/standalonebase/storage-boot.sh || true"; done=1
            }
            { print }
            END {
                if (!done) { print ""; print marker; print "/bin/sh /mnt/storage/standalonebase/storage-boot.sh || true" }
            }
        ' "$tmp" >"$merged" || { rm -f "$tmp" "$merged"; return 1; }
        mv "$merged" "$tmp" || { rm -f "$tmp" "$merged"; return 1; }
    fi
    cp "$tmp" "$RCS_LOCAL" || { rm -f "$tmp"; return 1; }
    chmod 755 "$RCS_LOCAL" || { rm -f "$tmp"; return 1; }
    rm -f "$tmp"
}

initialize() {
    supplied=$1
    : >"$LOG_FILE" || exit 27
    stage CHECKING_SYSTEM
    test -d "$UBI" || die 20 unsupported_system_or_layout
    test "$(value "$UBI/min_io_size")" = 2048 || die 20 ubi_min_io_size_mismatch
    awk '$1 == "ubi0:sbdata" || $1 == "/dev/ubi0_5" { found=1 } END { exit found ? 0 : 1 }' "$MOUNTS" 2>/dev/null && die 20 sbdata_is_mounted
    test -d "$SOURCE" || die 20 visible_applet_tree_missing
    test -x "$HELPER" || die 20 module_verifier_missing
    test -f "$supplied" || die 22 authorized_module_missing
    "$HELPER" verify-module "$supplied" >>"$LOG_FILE" 2>&1 || die 22 authorized_module_identity_mismatch
    test -x "$UBIUPDATEVOL" || die 23 ubiupdatevol_missing
    test -f "$SBDATA_IMAGE" || die 23 sbdata_image_missing
    "$HELPER" verify-sbdata-image "$SBDATA_IMAGE" >>"$LOG_FILE" 2>&1 || die 23 sbdata_image_identity_mismatch

    stage PREPARING_STORAGE
    test -f "$ROOT/etc/init.d/rcS" && grep -q 'rcS.local' "$ROOT/etc/init.d/rcS" 2>/dev/null || die 20 rcs_local_not_supported
    test -z "$(mounted_type "$SOURCE")" || die 10 already_active
    test -z "$(mounted_type "$MOUNT")" || die 20 sbdata_already_mounted

    if test -e "$VOLUME" || test -e "$VOLUME_DEVICE"; then
        test -r "$VOLUME/name" || die 21 sbdata_unavailable
        test "$(value "$VOLUME/name")" = sbdata || die 21 sbdata_identity_mismatch
        test "$(value "$VOLUME/type")" = dynamic || die 20 sbdata_not_dynamic
        test "$(value "$VOLUME/alignment")" = 1 || die 20 sbdata_alignment_mismatch
        test "$(value "$VOLUME/corrupted")" = 0 || die 20 sbdata_corrupt_or_unknown
        test "$(value "$VOLUME/upd_marker")" = 0 || die 20 sbdata_update_marker_set
        test "$(value "$VOLUME/reserved_ebs")" = 521 || die 20 sbdata_reserved_leb_mismatch
        test "$(value "$VOLUME/data_bytes")" = 67221504 || die 20 sbdata_data_bytes_mismatch
        test "$(value "$VOLUME/usable_eb_size")" = 129024 || die 20 sbdata_leb_size_mismatch
        device_node "$VOLUME_DEVICE" || die 21 sbdata_device_node_missing
    else
        test "$(value "$ROOT/etc/squeezeos.version")" = '7.7.3 r16676' || die 28 unsupported_virgin_layout
        device_node "$UBI_DEVICE" || die 28 unsupported_virgin_layout
        test "$(value "$UBI/total_eraseblocks")" = 1014 || die 28 unsupported_virgin_layout
        test "$(value "$UBI/eraseblock_size")" = 129024 || die 28 unsupported_virgin_layout
        test "$(value "$UBI/bad_peb_count")" = 0 || die 28 unsupported_virgin_layout
        test "$(value "$UBI/reserved_for_bad")" = 10 || die 28 unsupported_virgin_layout
        available=$(value "$UBI/avail_eraseblocks")
        case $available in ''|*[!0-9]*) die 28 unsupported_virgin_layout ;; esac
        test "$available" -ge 521 || die 29 insufficient_ubi_capacity
        test "$available" = 660 || die 28 unsupported_virgin_layout
        stock_volume_set_exact || die 28 unsupported_virgin_layout
        test -x "$UBIMKVOL" || die 30 ubimkvol_unavailable

        stage CREATING_VOLUME
        "$UBIMKVOL" "$UBI_DEVICE" -n 5 -N sbdata -S 521 -t dynamic >>"$LOG_FILE" 2>&1 \
            || die 31 volume_creation_failure
        exact_sbdata || die 32 post_creation_validation_failure
        test "$(value "$UBI/avail_eraseblocks")" = 139 || die 32 post_creation_validation_failure
    fi

    stage INITIALIZING_FILESYSTEM
    initialize_sbdata_filesystem || die 23 ubiupdatevol_failure

    test "$(value "$VOLUME/corrupted")" = 0 || die 23 post_update_corruption_detected
    test "$(value "$VOLUME/upd_marker")" = 0 || die 23 post_update_marker_set

    stage LOADING_DRIVER
    if ! has_fs; then insmod "$supplied" >>"$LOG_FILE" 2>&1 || die 22 module_load_failure; fi
    has_fs || die 22 sbubifs_not_registered

    stage MOUNTING
    mkdir -p "$MOUNT" || die 24 mountpoint_creation_failure
    mount -t sbubifs ubi0:sbdata "$MOUNT" >>"$LOG_FILE" 2>&1 || die 24 mount_failure
    test "$(mounted_type "$MOUNT")" = sbubifs || die 24 mount_verification_failure

    stage COPYING_APPLETS
    mkdir -p "$DEST" || die 25 destination_creation_failure
    cp -a "$SOURCE/." "$DEST/" >>"$LOG_FILE" 2>&1 || die 25 applet_migration_failure

    stage VERIFYING_APPLETS
    src_files=$(count_type "$SOURCE" f); dst_files=$(count_type "$DEST" f)
    src_dirs=$(count_type "$SOURCE" d); dst_dirs=$(count_type "$DEST" d)
    printf 'source_files=%s destination_files=%s source_dirs=%s destination_dirs=%s\n' "$src_files" "$dst_files" "$src_dirs" "$dst_dirs" >>"$LOG_FILE"
    test "$src_files" = "$dst_files" && test "$src_dirs" = "$dst_dirs" || die 26 migration_count_mismatch
    test -d "$DEST/AboutJive" -o -d "$DEST/SetupAppletInstaller" || die 26 core_applet_missing
    if test -d "$SOURCE/StandaloneBase"; then test -d "$DEST/StandaloneBase" || die 26 standalonebase_missing; fi
    test "$(mounted_type "$MOUNT")" = sbubifs || die 26 destination_not_on_sbdata

    stage INSTALLING_BOOT_SUPPORT
    mkdir -p "$PERSIST_DIR" || die 27 persistent_directory_failure
    cp "$supplied" "$PERSIST_MODULE" || die 27 module_install_failure
    "$HELPER" verify-module "$PERSIST_MODULE" >>"$LOG_FILE" 2>&1 || die 27 installed_module_identity_mismatch
    cp "$APPLET_DIR/storage-boot.sh" "$PERSIST_BOOT" || die 27 boot_helper_install_failure
    chmod 755 "$PERSIST_BOOT" || die 27 boot_helper_permission_failure
    touch "$REBOOT_MARKER" || die 27 reboot_marker_failure
    install_rcs_hook || { rm -f "$REBOOT_MARKER"; die 27 rcs_local_install_failure; }

    stage FINISHING
    sync
    stage COMPLETE
    printf 'EXIT_CODE=0\nREBOOT_REQUIRED=1\n' >>"$STATUS_FILE"
    printf 'STATUS=REBOOT_REQUIRED\nREBOOT_REQUIRED=1\n'
    exit 0
}

mount_existing() {
    supplied=$1
    test -d "$ROOT/sys/class/ubi/ubi0" || exit 20
    test -r "$VOLUME/name" && test "$(value "$VOLUME/name")" = sbdata || exit 21
    test "$(value "$VOLUME/type")" = dynamic || exit 20
    test "$(value "$VOLUME/reserved_ebs")" = 521 || exit 20
    test "$(value "$VOLUME/data_bytes")" = 67221504 || exit 20
    test "$(value "$VOLUME/corrupted")" = 0 || exit 20
    test "$(value "$VOLUME/upd_marker")" = 0 || exit 20
    test -x "$HELPER" && "$HELPER" verify-module "$supplied" >/dev/null 2>&1 || exit 22
    if test "$(mounted_type "$MOUNT")" = sbubifs; then exit 10; fi
    if ! has_fs; then insmod "$supplied" >/dev/null 2>&1 || exit 22; fi
    has_fs || exit 22
    mkdir -p "$MOUNT" || exit 24
    mount -t sbubifs ubi0:sbdata "$MOUNT" >/dev/null 2>&1 || exit 24
    test "$(mounted_type "$MOUNT")" = sbubifs || exit 24
    status_snapshot
}

case ${1:-} in
    check) status_snapshot ;;
    mount)
        test "$#" = 2 || { echo "usage: storage-setup.sh mount PATH-TO-sbubifs-authorized.ko" >&2; exit 64; }
        mount_existing "$2"
        ;;
    initialize)
        test "$#" = 2 || { echo "usage: storage-setup.sh initialize PATH-TO-sbubifs-authorized.ko" >&2; exit 64; }
        initialize "$2"
        ;;
    *) echo "usage: storage-setup.sh check | mount PATH-TO-sbubifs-authorized.ko | initialize PATH-TO-sbubifs-authorized.ko" >&2; exit 64 ;;
esac
