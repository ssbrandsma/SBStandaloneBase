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
sbdata_is_mounted() {
    awk '$1 == "ubi0:sbdata" || $1 == "/dev/ubi0_5" { found=1 } END { exit found ? 0 : 1 }' "$MOUNTS" 2>/dev/null
}
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

stock_volume_set_77x_exact() {
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

stock_volume_set_901_exact() {
    stock_volume_exact 0 kernel 23 2967552 || return 1
    stock_volume_exact 1 cramfs 113 14579712 || return 1
    stock_volume_exact 2 ubifs 82 10579968 || return 1
    stock_volume_exact 3 kernel_bak 23 2967552 || return 1
    stock_volume_exact 4 cramfs_bak 106 13676544 || return 1
    for stock in "$UBI_CLASS"/ubi0_*; do
        test -d "$stock" || continue
        case ${stock##*_} in 0|1|2|3|4) ;; *) return 1 ;; esac
    done
}

number() { case ${1:-} in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac; }

virgin_layout_exact() {
    firmware=$(value "$ROOT/etc/squeezeos.version")
    layout_total=$(value "$UBI/total_eraseblocks")
    layout_bad=$(value "$UBI/bad_peb_count")
    layout_available=$(value "$UBI/avail_eraseblocks")
    test -d "$UBI" \
        && device_node "$UBI_DEVICE" \
        && test "$(value "$UBI/eraseblock_size")" = 129024 \
        && test "$(value "$UBI/min_io_size")" = 2048 \
        && test "$(value "$UBI/reserved_for_bad")" = 10 \
        && number "$layout_total" && number "$layout_bad" && number "$layout_available" \
        && test $((layout_total + layout_bad)) = 1014 \
        && test ! -e "$VOLUME" \
        && test -z "$(mounted_type "$MOUNT")" \
        && test -z "$(mounted_type "$SOURCE")" \
        || return 1
    case $firmware in
        '7.7.2 r9663'|'7.7.3 r16676') test $((layout_available + layout_bad)) = 660 && stock_volume_set_77x_exact ;;
        '9.0.1 r17084') test $((layout_available + layout_bad)) = 653 && stock_volume_set_901_exact ;;
        *) return 1 ;;
    esac
}

# Existing-volume recovery is separate from initialization. In particular,
# available free LEBs are irrelevant: no UBI allocation or image write occurs.
recovery_eligible() {
    exact_sbdata && test -z "$(mounted_type "$SOURCE")" \
        && test ! -f "$REBOOT_MARKER" \
        && test "$(mounted_type "$ROOT/mnt/storage")" = ubifs \
        && test "$(value "$ROOT/proc/sys/kernel/osrelease")" = 2.6.26.8-rt16 \
        && grep -Eq 'Hardware[[:space:]]*:[[:space:]]*(Baby|Logitech MX25 Baby Board)' "$ROOT/proc/cpuinfo" \
        && test -d "$SOURCE/SetupAppletInstaller" \
        && test -d "$SOURCE/StandaloneBase" \
        && test -x "$HELPER" \
        && "$HELPER" verify-module "$BUNDLED_MODULE" >/dev/null 2>&1
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
    # Sysfs is authoritative for UBI volume existence and identity. SqueezeOS
    # may ship static /dev/ubi0_N nodes for volumes that do not exist yet.
    elif test -e "$VOLUME" && ! exact_sbdata; then state=ERROR
    elif test "$bind_active" = 1 && test "$sbdata_mounted" = 1 && test "$driver_loaded" = 1; then state=ACTIVE
    elif test -f "$REBOOT_MARKER"; then state=REBOOT_REQUIRED
    elif exact_sbdata || virgin_layout_exact; then state=AVAILABLE
    fi
    if test "$state" = AVAILABLE \
        && test -x "$HELPER" \
        && "$HELPER" verify-module "$BUNDLED_MODULE" >/dev/null 2>&1 \
        && "$HELPER" verify-sbdata-image "$SBDATA_IMAGE" >/dev/null 2>&1 \
        && test -x "$UBIUPDATEVOL" \
        && { exact_sbdata || { test -x "$UBIMKVOL" && test "$(value "$UBI/avail_eraseblocks")" -ge 600; }; }; then
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
    # Never offer destructive recovery for an active or unidentified volume.
    reinit_supported=0
    if test "$init_supported" = 1 && exact_sbdata \
        && ! sbdata_is_mounted \
        && test -z "$(mounted_type "$MOUNT")" \
        && test -z "$(mounted_type "$SOURCE")" \
        && test ! -f "$REBOOT_MARKER"; then reinit_supported=1; fi
    say "REINITIALIZATION_SUPPORTED=$reinit_supported"
    say "INITIALIZATION_REASON=$init_reason"
    say "RECOVERY_SUPPORTED=$(bool recovery_eligible)"
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
    operation_lock=$ROOT/tmp/sbstorage-operation.lock
    mkdir "$operation_lock" 2>/dev/null || { say ERROR=storage_operation_busy; exit 20; }
    trap 'rmdir "$operation_lock" 2>/dev/null || true' EXIT
    : >"$LOG_FILE" || exit 27
    stage CHECKING_SYSTEM
    if test "${2:-}" = erase-existing; then
        exact_sbdata || die 20 reinitialization_requires_exact_sbdata
        test ! -f "$REBOOT_MARKER" || die 20 reinitialization_reboot_pending
    fi
    test -d "$UBI" || die 20 unsupported_system_or_layout
    test "$(value "$UBI/min_io_size")" = 2048 || die 20 ubi_min_io_size_mismatch
    sbdata_is_mounted && die 20 sbdata_is_mounted
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

    if test -e "$VOLUME"; then
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
        virgin_layout_exact || die 28 unsupported_virgin_layout
        available=$(value "$UBI/avail_eraseblocks")
        case $available in ''|*[!0-9]*) die 28 unsupported_virgin_layout ;; esac
        test "$available" -ge 600 || die 29 insufficient_ubi_capacity
        test -x "$UBIMKVOL" || die 30 ubimkvol_unavailable

        stage CREATING_VOLUME
        "$UBIMKVOL" "$UBI_DEVICE" -n 5 -N sbdata -S 521 -t dynamic >>"$LOG_FILE" 2>&1 \
            || die 31 volume_creation_failure
        post_create_attempt=1
        post_create_max_attempts=${SB_STORAGE_POST_CREATE_MAX_ATTEMPTS:-10}
        while ! { exact_sbdata \
            && test "$(value "$UBI/avail_eraseblocks")" = $((available - 521)); }; do
            test "$post_create_attempt" -lt "$post_create_max_attempts" \
                || die 32 post_creation_validation_failure
            printf '%s Waiting for UBI volume readiness (attempt %s/%s)\n' \
                "$(date '+%Y-%m-%dT%H:%M:%S' 2>/dev/null || echo unknown-time)" \
                "$post_create_attempt" "$post_create_max_attempts" >>"$LOG_FILE"
            sleep 1
            post_create_attempt=$((post_create_attempt + 1))
        done
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

# Known firmware applets must never survive solely from the old firmware tree.
# Current firmware directories additionally take precedence below. Everything
# else is treated as a third-party applet and retained, including local caches.
firmware_applet() {
    case $1 in
        JogglerSkin|Linux|QVGA240squareSkin|QVGAportraitSkin|WQVGAlargeSkin|WQVGAsmallSkin) return 0 ;;
        AboutJive|AlarmSnooze|BlankScreen|ChooseMusicSource|Clock|CrashLog|CustomizeHomeMenu|DebugSkin|DebugTouch|Demo|Diagnostics|Experiments|HttpAuth|ImageViewer|InfoBrowser|LineIn|LogSettings|MacroPlay|NowPlaying|Playback|QVGAbaseSkin|QVGAlandscapeSkin|ScreenSavers|Screenshot|SelectPlayer|SelectSkin|SetupAppletInstaller|SetupDateTime|SetupFactoryReset|SetupFirmwareUpgrade|SetupLanguage|SetupNetTest|SetupNetworking|SetupSSH|SetupSoundEffects|SetupSqueezebox|SetupTZ|SetupWallpaper|SetupWelcome|SetupWifiRobustness|Shortcuts|SlimBrowser|SlimDiscovery|SlimMenus|Squeezebox|SqueezeboxBaby|TestAmbient|TestAudioRouting|TestDisplay|TestKeypad|TestTones|UdapControl|Spotify|SqueezeNetworkPIN) return 0 ;;
        *) return 1 ;;
    esac
}

recover_existing() (
    # A subshell confines the cleanup trap and lock to this transaction.
    recovery_eligible || { status_snapshot; exit 0; }
    lock=$ROOT/tmp/sbstorage-operation.lock
    mkdir "$lock" 2>/dev/null || exit 0
    recovery_mount=0
    cleanup_recovery() {
        test "$recovery_mount" = 0 || umount "$MOUNT" >>"$LOG_FILE" 2>&1 || true
        rmdir "$lock" 2>/dev/null || true
    }
    trap cleanup_recovery EXIT
    trap 'exit 1' HUP INT TERM
    : >"$LOG_FILE" || exit 27
    stage CHECKING_SYSTEM
    # Recheck after acquiring the lock; refuse foreign mounts or pending work.
    recovery_eligible || die 20 recovery_preconditions_changed
    test -z "$(mounted_type "$MOUNT")" || die 20 recovery_volume_already_mounted
    test -f "$ROOT/etc/init.d/rcS" || die 27 recovery_rcs_missing
    /bin/sh -n "$ROOT/etc/init.d/rcS" || die 27 recovery_rcs_invalid
    if ! grep -q 'rcS.local' "$ROOT/etc/init.d/rcS"; then
        test "$(grep -c '^/etc/init.d/squeezeplay start$' "$ROOT/etc/init.d/rcS")" = 1 \
            || die 27 recovery_rcs_requires_review
    fi
    stage LOADING_DRIVER
    if ! has_fs; then
        insmod "$BUNDLED_MODULE" >>"$LOG_FILE" 2>&1 || die 22 recovery_module_incompatible
    fi
    has_fs || die 22 recovery_filesystem_not_registered
    stage MOUNTING
    mkdir -p "$MOUNT" || die 24 recovery_mountpoint_failure
    mount -t sbubifs -o ro ubi0:sbdata "$MOUNT" >>"$LOG_FILE" 2>&1 || die 24 recovery_readonly_mount_failed
    recovery_mount=1
    test "$(mounted_type "$MOUNT")" = sbubifs || die 24 recovery_mount_identity_failed
    test -d "$DEST/SetupAppletInstaller" && test -d "$DEST/StandaloneBase" \
        && test ! -L "$DEST" || die 26 recovery_old_tree_invalid
    # Keep interrupted transactions for manual review rather than guessing
    # which tree is authoritative or deleting a previous backup.
    for prior in "$MOUNT"/.firmware-recovery.*; do
        test ! -e "$prior" || die 26 recovery_previous_transaction_requires_review
    done
    required=$(du -sk "$SOURCE" "$DEST" | awk '{n += $1} END {print n + 4096}')
    free=$(df -Pk "$MOUNT" | awk 'NR==2 {print $4}')
    number "$required" && number "$free" && test "$free" -ge "$required" \
        || die 26 recovery_insufficient_space
    # Reject symlinks in an old custom tree before copying over it. Otherwise
    # an old symlink could redirect the new firmware copy outside staging.
    for old in "$DEST"/*; do
        test -e "$old" || continue
        name=${old##*/}
        firmware_applet "$name" && continue
        test -d "$old" && test ! -L "$old" || die 26 recovery_custom_tree_invalid
        test -z "$(find "$old" -type l -print)" || die 26 recovery_custom_symlinks_require_review
    done
    mount -o remount,rw "$MOUNT" >>"$LOG_FILE" 2>&1 || die 24 recovery_remount_failed
    transaction=$(mktemp -d "$MOUNT/.firmware-recovery.XXXXXX") || die 25 recovery_staging_failed
    stage COPYING_APPLETS
    mkdir "$transaction/new" || die 25 recovery_staging_failed
    for old in "$DEST"/*; do
        test -e "$old" || continue
        name=${old##*/}
        firmware_applet "$name" && continue
        cp -a "$old" "$transaction/new/" || die 25 recovery_custom_copy_failed
    done
    # Reinstalled applets win over their old copies. Core directories come
    # exclusively from the running firmware, never the previous image.
    cp -a "$SOURCE/." "$transaction/new/" || die 25 recovery_firmware_copy_failed
    stage VERIFYING_APPLETS
    find "$SOURCE" -type f -print | while IFS= read -r file; do
        "$HELPER" verify-copy "$file" "$transaction/new/${file#"$SOURCE/"}" || exit 1
    done || die 26 recovery_copy_verification_failed
    "$transaction/new/StandaloneBase/sb-storage-helper" verify-module "$BUNDLED_MODULE" \
        >>"$LOG_FILE" 2>&1 || die 26 recovery_helper_verification_failed
    # Flush staging before promoting. The previous tree is retained in full.
    sync
    mv "$DEST" "$transaction/previous-applets" || die 25 recovery_backup_failed
    if ! mv "$transaction/new" "$DEST"; then
        mv "$transaction/previous-applets" "$DEST" || die 25 recovery_rollback_requires_review
        die 25 recovery_promotion_failed
    fi
    stage INSTALLING_BOOT_SUPPORT
    mkdir -p "$PERSIST_DIR" || die 27 recovery_persistent_directory_failed
    cp "$BUNDLED_MODULE" "$PERSIST_MODULE.new" \
        && "$HELPER" verify-module "$PERSIST_MODULE.new" >>"$LOG_FILE" 2>&1 \
        && mv "$PERSIST_MODULE.new" "$PERSIST_MODULE" || die 27 recovery_module_install_failed
    cp "$APPLET_DIR/storage-boot.sh" "$PERSIST_BOOT.new" \
        && chmod 755 "$PERSIST_BOOT.new" \
        && mv "$PERSIST_BOOT.new" "$PERSIST_BOOT" || die 27 recovery_boot_install_failed
    if test -f "$RCS_LOCAL"; then
        cp -p "$RCS_LOCAL" "$transaction/rcS.local.before" || die 27 recovery_local_hook_backup_failed
    fi
    install_rcs_hook || die 27 recovery_hook_install_failed
    if ! grep -q 'rcS.local' "$ROOT/etc/init.d/rcS"; then
        rcs=$ROOT/etc/init.d/rcS
        cp -p "$rcs" "$transaction/rcS.before" || die 27 recovery_rcs_backup_failed
        awk '/^\/etc\/init.d\/squeezeplay start$/ {
            print "# STANDALONEBASE-EXTENDED-STORAGE"
            print "test ! -x /etc/init.d/rcS.local || /etc/init.d/rcS.local"
        } {print}' "$rcs" >"$rcs.sbstorage-new" || die 27 recovery_rcs_patch_failed
        /bin/sh -n "$rcs.sbstorage-new" && chmod 755 "$rcs.sbstorage-new" \
            && mv "$rcs.sbstorage-new" "$rcs" || die 27 recovery_rcs_patch_failed
    fi
    touch "$REBOOT_MARKER" || die 27 recovery_marker_failed
    # Completed backups do not block later firmware upgrades.
    mv "$transaction" "$MOUNT/firmware-backup-${transaction##*.}" || die 27 recovery_backup_finalize_failed
    sync
    stage COMPLETE
    printf 'EXIT_CODE=0\nREBOOT_REQUIRED=1\n' >>"$STATUS_FILE"
    say STATUS=REBOOT_REQUIRED
    say REBOOT_REQUIRED=1
)

case ${1:-} in
    check) status_snapshot ;;
    recover) recover_existing ;;
    mount)
        test "$#" = 2 || { echo "usage: storage-setup.sh mount PATH-TO-sbubifs-authorized.ko" >&2; exit 64; }
        mount_existing "$2"
        ;;
    initialize)
        test "$#" = 2 || { echo "usage: storage-setup.sh initialize PATH-TO-sbubifs-authorized.ko" >&2; exit 64; }
        initialize "$2"
        ;;
    reinitialize)
        test "$#" = 3 && test "$2" = --confirm-erase-sbdata || {
            echo "usage: storage-setup.sh reinitialize --confirm-erase-sbdata PATH-TO-sbubifs-authorized.ko" >&2; exit 64;
        }
        initialize "$3" erase-existing
        ;;
    *) echo "usage: storage-setup.sh check | recover | mount MODULE | initialize MODULE | reinitialize --confirm-erase-sbdata MODULE" >&2; exit 64 ;;
esac
