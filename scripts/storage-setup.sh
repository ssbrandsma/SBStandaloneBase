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
CHECK_LOG=$ROOT/tmp/sbstorage-check.log
REBOOT_MARKER=$PERSIST_DIR/extended-storage-reboot-required
OPERATION=${1:-unknown}

# Use the existing syslog daemon (normally /var/log/messages on the Radio).
# Fixtures may provide their own logger; never send test messages to host syslog.
system_logger() {
    if test -n "$ROOT"; then
        test -x "$ROOT/bin/logger" || return 0
        "$ROOT/bin/logger" -t standalonebase-storage
    elif command -v logger >/dev/null 2>&1; then
        logger -t standalonebase-storage
    fi
}

system_log() {
    printf 'operation=%s pid=%s %s\n' "$OPERATION" "$$" "$*" | system_logger 2>/dev/null || true
}

system_log_file() {
    sed "s/^/operation=$OPERATION pid=$$ /" "$1" | system_logger 2>/dev/null || true
}

operation_log() {
    printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S' 2>/dev/null || echo unknown-time)" "$*" >>"$LOG_FILE"
    system_log "$*"
}

# Preserve the command's exit code while mirroring its output to both logs.
# No pipeline is used to execute the command, so logger cannot hide a failure.
log_command() (
    command_output=$(mktemp "$ROOT/tmp/sbstorage-command.XXXXXX") || exit 27
    trap 'rm -f "$command_output"' EXIT
    "$@" >"$command_output" 2>&1
    command_status=$?
    cat "$command_output" >>"$LOG_FILE"
    system_log_file "$command_output"
    exit "$command_status"
)

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

# Diagnostics run in subshells: their temporary variables must not alter the
# initialization/recovery transaction. These functions never mount or write UBI.
diagnostic_equal() {
    if test "$3" = "$2"; then diagnostic_result=PASS; else diagnostic_result=FAIL; fi
    printf '%s %s expected=[%s] actual=[%s]\n' "$diagnostic_result" "$1" "$2" "${3:-<missing>}"
}

diagnostic_test() {
    diagnostic_label=$1; shift
    if "$@"; then printf 'PASS %s\n' "$diagnostic_label"
    else printf 'FAIL %s\n' "$diagnostic_label"; fi
}

diagnostic_stock() (
    diagnostic_volume=$UBI_CLASS/ubi0_$1
    diagnostic_equal "ubi0_$1.name" "$2" "$(value "$diagnostic_volume/name")"
    diagnostic_equal "ubi0_$1.type" dynamic "$(value "$diagnostic_volume/type")"
    diagnostic_equal "ubi0_$1.reserved_ebs" "$3" "$(value "$diagnostic_volume/reserved_ebs")"
    diagnostic_equal "ubi0_$1.data_bytes" "$4" "$(value "$diagnostic_volume/data_bytes")"
    diagnostic_equal "ubi0_$1.corrupted" 0 "$(value "$diagnostic_volume/corrupted")"
    diagnostic_equal "ubi0_$1.upd_marker" 0 "$(value "$diagnostic_volume/upd_marker")"
)

layout_diagnostics() (
    printf 'STORAGE_ANALYSIS time=%s\n' "$(date '+%Y-%m-%dT%H:%M:%S' 2>/dev/null || echo unknown-time)"
    printf 'Uptime: %s\n' "$(value "$ROOT/proc/uptime")"
    printf 'No initialization, recovery, mount or flash write is performed by this analysis.\n'
    printf '%s\n' '--- Platform and common prerequisites ---'
    printf 'Firmware: %s\nKernel: %s\n' "$(value "$ROOT/etc/squeezeos.version")" "$(value "$ROOT/proc/sys/kernel/osrelease")"
    grep -E '^(Processor|Hardware|Revision)[[:space:]]*:' "$ROOT/proc/cpuinfo" 2>/dev/null || true
    diagnostic_test ubi0_directory test -d "$UBI"
    diagnostic_test ubi0_device_node device_node "$UBI_DEVICE"
    diagnostic_equal production_mount_type ubifs "$(mounted_type "$ROOT/mnt/storage")"
    diagnostic_test applet_source test -d "$SOURCE"
    diagnostic_test rcs_local_hook grep -q rcS.local "$ROOT/etc/init.d/rcS"
    diagnostic_test helper_executable test -x "$HELPER"
    diagnostic_test image_present test -f "$SBDATA_IMAGE"
    diagnostic_test module_present test -f "$BUNDLED_MODULE"
    diagnostic_test ubiupdatevol_executable test -x "$UBIUPDATEVOL"
    printf 'Reboot marker: %s\nMount at sbdata: [%s]\nMount at applets: [%s]\n' \
        "$(bool test -f "$REBOOT_MARKER")" "$(mounted_type "$MOUNT")" "$(mounted_type "$SOURCE")"
    if test -e "$VOLUME"; then
        printf '%s\n' '--- Existing sbdata identity checks ---'
        diagnostic_test sbdata_device_node device_node "$VOLUME_DEVICE"
        for diagnostic_pair in 'name sbdata' 'type dynamic' 'alignment 1' 'reserved_ebs 521' \
            'data_bytes 67221504' 'usable_eb_size 129024' 'corrupted 0' 'upd_marker 0'; do
            set -- $diagnostic_pair
            diagnostic_equal "ubi0_5.$1" "$2" "$(value "$VOLUME/$1")"
        done
        diagnostic_equal ubi0.min_io_size 2048 "$(value "$UBI/min_io_size")"
        diagnostic_equal recovery_kernel 2.6.26.8-rt16 "$(value "$ROOT/proc/sys/kernel/osrelease")"
        diagnostic_test recovery_hardware grep -Eq 'Hardware[[:space:]]*:[[:space:]]*(Baby|Logitech MX25 Baby Board)' "$ROOT/proc/cpuinfo"
        diagnostic_test recovery_core_applet test -d "$SOURCE/SetupAppletInstaller"
        diagnostic_test exact_existing_sbdata exact_sbdata
        diagnostic_test recovery_eligible recovery_eligible
        printf 'Free unallocated LEBs are not required when reusing an existing sbdata volume.\n'
    else
        printf '%s\n' '--- New-volume layout checks ---'
        diagnostic_equal ubi0.eraseblock_size 129024 "$(value "$UBI/eraseblock_size")"
        diagnostic_equal ubi0.min_io_size 2048 "$(value "$UBI/min_io_size")"
        diagnostic_equal ubi0.reserved_for_bad 10 "$(value "$UBI/reserved_for_bad")"
        diagnostic_equal sbdata_mount_absent '' "$(mounted_type "$MOUNT")"
        diagnostic_equal applet_bind_absent '' "$(mounted_type "$SOURCE")"
        diagnostic_total=$(value "$UBI/total_eraseblocks")
        diagnostic_bad=$(value "$UBI/bad_peb_count")
        diagnostic_free=$(value "$UBI/avail_eraseblocks")
        diagnostic_test total_eraseblocks_numeric number "$diagnostic_total"
        diagnostic_test bad_peb_count_numeric number "$diagnostic_bad"
        diagnostic_test available_lebs_numeric number "$diagnostic_free"
        if number "$diagnostic_total" && number "$diagnostic_bad"; then
            diagnostic_equal total_plus_bad 1014 "$((diagnostic_total + diagnostic_bad))"
        fi
        diagnostic_test minimum_600_available_lebs test "${diagnostic_free:-0}" -ge 600
        diagnostic_test ubimkvol_executable test -x "$UBIMKVOL"
        case $(value "$ROOT/etc/squeezeos.version") in
            '7.7.2 r9663'|'7.7.3 r16676')
                diagnostic_expected_free=660
                diagnostic_stock 0 kernel_bak 23 2967552
                diagnostic_stock 1 cramfs_bak 106 13676544
                diagnostic_stock 2 ubifs 82 10579968
                diagnostic_stock 3 kernel 23 2967552
                diagnostic_stock 4 cramfs 106 13676544 ;;
            '9.0.1 r17084')
                diagnostic_expected_free=653
                diagnostic_stock 0 kernel 23 2967552
                diagnostic_stock 1 cramfs 113 14579712
                diagnostic_stock 2 ubifs 82 10579968
                diagnostic_stock 3 kernel_bak 23 2967552
                diagnostic_stock 4 cramfs_bak 106 13676544 ;;
            *) diagnostic_expected_free=; printf 'FAIL firmware_profile: no validated new-volume profile for this exact firmware\n' ;;
        esac
        if test -n "$diagnostic_expected_free" && number "$diagnostic_free" && number "$diagnostic_bad"; then
            diagnostic_equal available_plus_bad "$diagnostic_expected_free" "$((diagnostic_free + diagnostic_bad))"
        fi
        for diagnostic_volume in "$UBI_CLASS"/ubi0_*; do
            test -d "$diagnostic_volume" || continue
            case ${diagnostic_volume##*_} in
                0|1|2|3|4) ;;
                *) printf 'FAIL unexpected_volume=%s\n' "${diagnostic_volume##*/}" ;;
            esac
        done
        diagnostic_test exact_virgin_layout virgin_layout_exact
    fi
    printf '%s\n' '--- UBI controller and volume inventory (maximum 16 volumes) ---'
    for diagnostic_field in total_eraseblocks avail_eraseblocks bad_peb_count reserved_for_bad eraseblock_size min_io_size; do
        printf 'ubi0.%s=%s\n' "$diagnostic_field" "$(value "$UBI/$diagnostic_field")"
    done
    diagnostic_count=0
    for diagnostic_volume in "$UBI_CLASS"/ubi0_*; do
        test -d "$diagnostic_volume" || continue
        diagnostic_count=$((diagnostic_count + 1))
        test "$diagnostic_count" -le 16 || { printf 'Inventory truncated\n'; break; }
        for diagnostic_field in name type alignment reserved_ebs data_bytes usable_eb_size corrupted upd_marker; do
            printf '%s.%s=%s\n' "${diagnostic_volume##*/}" "$diagnostic_field" "$(value "$diagnostic_volume/$diagnostic_field")"
        done
    done
    printf '%s\n' '--- MTD layout ---'
    head -n 32 "$ROOT/proc/mtd" 2>/dev/null || true
    printf '%s\n' '--- Mounts ---'
    head -n 64 "$MOUNTS" 2>/dev/null || true
    printf '%s\n' '--- Registered filesystems ---'
    head -n 64 "$FILESYSTEMS" 2>/dev/null || true
    printf '%s\n' '--- Device nodes and prerequisites ---'
    ls -ld "$UBI_DEVICE" "$VOLUME_DEVICE" "$HELPER" "$BUNDLED_MODULE" "$SBDATA_IMAGE" "$UBIMKVOL" "$UBIUPDATEVOL" 2>&1 || true
    printf '%s\n' '--- Normal storage capacity ---'
    df -Pk "$ROOT/mnt/storage" 2>&1 || true
)

write_check_report() (
    # One bounded snapshot, never an append-only polling log. Unchanged failed
    # checks skip the expensive inventory. Do not overwrite operation logs.
    diagnostic_stable=$(printf '%s\n' "$1" | sed '/^TOTAL_KB=/d; /^USED_KB=/d; /^FREE_KB=/d')
    diagnostic_signature=$(printf '%s\n' "$diagnostic_stable" | cksum 2>/dev/null) \
        || diagnostic_signature=$(printf '%s\n' "$diagnostic_stable" | tr '\n' '|')
    if test -n "$diagnostic_signature" && test "${2:-}" != analyze \
        && test "$(value "$CHECK_LOG")" = "SNAPSHOT=$diagnostic_signature"; then exit 0; fi
    umask 077
    diagnostic_tmp=$(mktemp "$ROOT/tmp/sbstorage-check.XXXXXX") || exit 1
    trap 'rm -f "$diagnostic_tmp"' EXIT
    {
        printf 'SNAPSHOT=%s\n%s\n' "${diagnostic_signature:-uncached}" "$1"
        layout_diagnostics
        if test -x "$HELPER"; then
            if printf '%s\n' "$1" | grep -q '^BUNDLED_MODULE_VALID=0$'; then
                printf '%s\n' '--- Module verifier failure details ---'
                "$HELPER" verify-module "$BUNDLED_MODULE" 2>&1 | head -n 20
            fi
            if printf '%s\n' "$1" | grep -q '^SBDATA_IMAGE_VALID=0$'; then
                printf '%s\n' '--- Image verifier failure details ---'
                "$HELPER" verify-sbdata-image "$SBDATA_IMAGE" 2>&1 | head -n 20
            fi
        fi
    } >"$diagnostic_tmp" 2>&1
    mv "$diagnostic_tmp" "$CHECK_LOG" || exit 1
    system_log 'CHECK_REPORT_BEGIN'
    system_log_file "$CHECK_LOG"
    system_log 'CHECK_REPORT_END'
)

check_storage() {
    diagnostic_snapshot=$(status_snapshot)
    printf '%s\n' "$diagnostic_snapshot"
    write_check_report "$diagnostic_snapshot" "${1:-}" || {
        printf 'Storage diagnostic report could not be saved\n' >&2
        system_log 'ERROR=diagnostic_report_write_failed'
    }
    printf 'DIAGNOSTIC_LOG=/tmp/sbstorage-check.log\n'
    if test "${1:-}" = analyze; then cat "$CHECK_LOG"; fi
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
    operation_log "STAGE=$STAGE"
}

die() {
    code=$1; shift
    printf 'ERROR=%s\nEXIT_CODE=%s\n' "$*" "$code" >>"$STATUS_FILE"
    operation_log "ERROR=$* EXIT_CODE=$code"
    exit "$code"
}

initialize_sbdata_filesystem() {
    operation_log 'ERASE_BEGIN volume=ubi0_5 name=sbdata; all destructive preconditions and image identity verified'
    log_command "$UBIUPDATEVOL" "$VOLUME_DEVICE" "$SBDATA_IMAGE" || return $?
    operation_log 'ERASE_COMPLETE filesystem image written to sbdata'
}

count_type() { find "$1" -type "$2" -print 2>/dev/null | wc -l | tr -d ' '; }

verify_recovered_tree() {
    find "$SOURCE" -type f -print | while IFS= read -r file; do
        "$HELPER" verify-copy "$file" "$transaction/new/${file#"$SOURCE/"}" || {
            printf 'VERIFY_COPY_FAILED file=%s\n' "$file" >&2
            exit 1
        }
    done
}

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
    mkdir "$operation_lock" 2>/dev/null || { say ERROR=storage_operation_busy; system_log 'ERROR=storage_operation_busy EXIT_CODE=20'; exit 20; }
    trap 'rmdir "$operation_lock" 2>/dev/null || true' EXIT
    : >"$LOG_FILE" || exit 27
    stage CHECKING_SYSTEM
    operation_log "OPERATION_BEGIN type=$OPERATION"
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
    log_command "$HELPER" verify-module "$supplied" || die 22 authorized_module_identity_mismatch
    test -x "$UBIUPDATEVOL" || die 23 ubiupdatevol_missing
    test -f "$SBDATA_IMAGE" || die 23 sbdata_image_missing
    log_command "$HELPER" verify-sbdata-image "$SBDATA_IMAGE" || die 23 sbdata_image_identity_mismatch

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
        log_command "$UBIMKVOL" "$UBI_DEVICE" -n 5 -N sbdata -S 521 -t dynamic \
            || die 31 volume_creation_failure
        post_create_attempt=1
        post_create_max_attempts=${SB_STORAGE_POST_CREATE_MAX_ATTEMPTS:-10}
        while ! { exact_sbdata \
            && test "$(value "$UBI/avail_eraseblocks")" = $((available - 521)); }; do
            test "$post_create_attempt" -lt "$post_create_max_attempts" \
                || die 32 post_creation_validation_failure
            operation_log "Waiting for UBI volume readiness (attempt $post_create_attempt/$post_create_max_attempts)"
            sleep 1
            post_create_attempt=$((post_create_attempt + 1))
        done
    fi

    stage INITIALIZING_FILESYSTEM
    initialize_sbdata_filesystem || die 23 ubiupdatevol_failure

    test "$(value "$VOLUME/corrupted")" = 0 || die 23 post_update_corruption_detected
    test "$(value "$VOLUME/upd_marker")" = 0 || die 23 post_update_marker_set

    stage LOADING_DRIVER
    if ! has_fs; then log_command insmod "$supplied" || die 22 module_load_failure; fi
    has_fs || die 22 sbubifs_not_registered

    stage MOUNTING
    mkdir -p "$MOUNT" || die 24 mountpoint_creation_failure
    log_command mount -t sbubifs ubi0:sbdata "$MOUNT" || die 24 mount_failure
    test "$(mounted_type "$MOUNT")" = sbubifs || die 24 mount_verification_failure

    stage COPYING_APPLETS
    mkdir -p "$DEST" || die 25 destination_creation_failure
    operation_log "COPY_APPLETS source=$SOURCE destination=$DEST"
    log_command cp -a "$SOURCE/." "$DEST/" || die 25 applet_migration_failure

    stage VERIFYING_APPLETS
    src_files=$(count_type "$SOURCE" f); dst_files=$(count_type "$DEST" f)
    src_dirs=$(count_type "$SOURCE" d); dst_dirs=$(count_type "$DEST" d)
    operation_log "source_files=$src_files destination_files=$dst_files source_dirs=$src_dirs destination_dirs=$dst_dirs"
    test "$src_files" = "$dst_files" && test "$src_dirs" = "$dst_dirs" || die 26 migration_count_mismatch
    test -d "$DEST/AboutJive" -o -d "$DEST/SetupAppletInstaller" || die 26 core_applet_missing
    if test -d "$SOURCE/StandaloneBase"; then test -d "$DEST/StandaloneBase" || die 26 standalonebase_missing; fi
    test "$(mounted_type "$MOUNT")" = sbubifs || die 26 destination_not_on_sbdata

    stage INSTALLING_BOOT_SUPPORT
    mkdir -p "$PERSIST_DIR" || die 27 persistent_directory_failure
    log_command cp "$supplied" "$PERSIST_MODULE" || die 27 module_install_failure
    log_command "$HELPER" verify-module "$PERSIST_MODULE" || die 27 installed_module_identity_mismatch
    log_command cp "$APPLET_DIR/storage-boot.sh" "$PERSIST_BOOT" || die 27 boot_helper_install_failure
    chmod 755 "$PERSIST_BOOT" || die 27 boot_helper_permission_failure
    touch "$REBOOT_MARKER" || die 27 reboot_marker_failure
    log_command install_rcs_hook || { rm -f "$REBOOT_MARKER"; die 27 rcs_local_install_failure; }

    stage FINISHING
    sync
    stage COMPLETE
    operation_log 'OPERATION_COMPLETE EXIT_CODE=0 REBOOT_REQUIRED=1'
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
    recovery_eligible || { system_log 'RECOVERY_SKIPPED preconditions_not_met'; check_storage; exit 0; }
    lock=$ROOT/tmp/sbstorage-operation.lock
    mkdir "$lock" 2>/dev/null || { system_log 'RECOVERY_SKIPPED storage_operation_busy'; exit 0; }
    recovery_mount=0
    cleanup_recovery() {
        test "$recovery_mount" = 0 || log_command umount "$MOUNT" || true
        rmdir "$lock" 2>/dev/null || true
    }
    trap cleanup_recovery EXIT
    trap 'exit 1' HUP INT TERM
    : >"$LOG_FILE" || exit 27
    stage CHECKING_SYSTEM
    operation_log 'OPERATION_BEGIN type=recover (non-destructive)'
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
        log_command insmod "$BUNDLED_MODULE" || die 22 recovery_module_incompatible
    fi
    has_fs || die 22 recovery_filesystem_not_registered
    stage MOUNTING
    mkdir -p "$MOUNT" || die 24 recovery_mountpoint_failure
    log_command mount -t sbubifs -o ro ubi0:sbdata "$MOUNT" || die 24 recovery_readonly_mount_failed
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
    operation_log "RECOVERY_SPACE required_kb=$required available_kb=$free"
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
    log_command mount -o remount,rw "$MOUNT" || die 24 recovery_remount_failed
    transaction=$(mktemp -d "$MOUNT/.firmware-recovery.XXXXXX") || die 25 recovery_staging_failed
    stage COPYING_APPLETS
    mkdir "$transaction/new" || die 25 recovery_staging_failed
    for old in "$DEST"/*; do
        test -e "$old" || continue
        name=${old##*/}
        firmware_applet "$name" && continue
        operation_log "COPY_CUSTOM_APPLET name=$name"
        log_command cp -a "$old" "$transaction/new/" || die 25 recovery_custom_copy_failed
    done
    # Reinstalled applets win over their old copies. Core directories come
    # exclusively from the running firmware, never the previous image.
    operation_log "COPY_FIRMWARE_APPLETS source=$SOURCE destination=$transaction/new"
    log_command cp -a "$SOURCE/." "$transaction/new/" || die 25 recovery_firmware_copy_failed
    stage VERIFYING_APPLETS
    log_command verify_recovered_tree || die 26 recovery_copy_verification_failed
    log_command "$transaction/new/StandaloneBase/sb-storage-helper" verify-module "$BUNDLED_MODULE" \
        || die 26 recovery_helper_verification_failed
    # Flush staging before promoting. The previous tree is retained in full.
    sync
    operation_log 'PROMOTING_APPLETS retaining previous tree for rollback'
    log_command mv "$DEST" "$transaction/previous-applets" || die 25 recovery_backup_failed
    if ! log_command mv "$transaction/new" "$DEST"; then
        log_command mv "$transaction/previous-applets" "$DEST" || die 25 recovery_rollback_requires_review
        die 25 recovery_promotion_failed
    fi
    stage INSTALLING_BOOT_SUPPORT
    mkdir -p "$PERSIST_DIR" || die 27 recovery_persistent_directory_failed
    log_command cp "$BUNDLED_MODULE" "$PERSIST_MODULE.new" \
        && log_command "$HELPER" verify-module "$PERSIST_MODULE.new" \
        && mv "$PERSIST_MODULE.new" "$PERSIST_MODULE" || die 27 recovery_module_install_failed
    log_command cp "$APPLET_DIR/storage-boot.sh" "$PERSIST_BOOT.new" \
        && chmod 755 "$PERSIST_BOOT.new" \
        && mv "$PERSIST_BOOT.new" "$PERSIST_BOOT" || die 27 recovery_boot_install_failed
    if test -f "$RCS_LOCAL"; then
        cp -p "$RCS_LOCAL" "$transaction/rcS.local.before" || die 27 recovery_local_hook_backup_failed
    fi
    log_command install_rcs_hook || die 27 recovery_hook_install_failed
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
    operation_log 'OPERATION_COMPLETE EXIT_CODE=0 REBOOT_REQUIRED=1'
    printf 'EXIT_CODE=0\nREBOOT_REQUIRED=1\n' >>"$STATUS_FILE"
    say STATUS=REBOOT_REQUIRED
    say REBOOT_REQUIRED=1
)

case ${1:-} in
    check) check_storage ;;
    analyze) check_storage analyze ;;
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
    *) echo "usage: storage-setup.sh check | analyze | recover | mount MODULE | initialize MODULE | reinitialize --confirm-erase-sbdata MODULE" >&2; exit 64 ;;
esac
