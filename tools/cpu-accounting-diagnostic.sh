#!/bin/sh
# Read-only BusyBox diagnostic. Output goes to stdout; no device files are written.
# Usage: sh cpu-accounting-diagnostic.sh [--light] [interval ...].
# Default intervals: 5 10 30; --light skips task enumeration for overhead control.
# Snapshots are bracketed by uptime/stat reads, not falsely described as atomic.
export LC_ALL=C
light=0
if [ "${1:-}" = --light ]; then light=1; shift; fi
if [ "$#" -eq 0 ]; then set -- 5 10 30; fi
for seconds do
    case "$seconds" in ''|*[!0-9]*) echo 'Invalid interval' >&2; exit 2;; esac
    if [ "$seconds" -lt 1 ] || [ "$seconds" -gt 60 ]; then
        echo 'Intervals must be 1..60 seconds' >&2; exit 2
    fi
done

file() {
    printf 'FILE %s\n' "$1"
    if [ -r "$1" ]; then cat "$1"; else echo UNSUPPORTED; fi
    printf '\nEND_FILE\n'
}

snapshot() {
    printf 'SNAPSHOT %s\n' "$1"
    file /proc/uptime
    file /proc/stat
    file /proc/sys/kernel/random/boot_id
    file /proc/loadavg
    file /proc/interrupts
    file /proc/softirqs
    file /proc/schedstat
    file /proc/sched_debug
    file /proc/timer_list
    if [ "$light" -eq 0 ]; then
        for directory in /proc/[0-9]*; do
            # read is a shell builtin: no fork per process/thread stat record.
            if IFS= read -r line 2>/dev/null < "$directory/stat"; then
                printf 'PROCESS %s\n' "$line"
            else
                printf 'RACE %s/stat\n' "$directory"
            fi
            for task in "$directory"/task/[0-9]*; do
                if IFS= read -r line 2>/dev/null < "$task/stat"; then
                    printf 'THREAD %s %s\n' "${directory##*/}" "$line"
                fi
                if [ -r "$task/schedstat" ]; then
                    IFS= read -r line < "$task/schedstat"
                    printf 'TASK_SCHEDSTAT %s %s %s\n' "${directory##*/}" "${task##*/}" "$line"
                fi
            done
        done
    fi
    file /proc/uptime
    file /proc/stat
    printf 'END_SNAPSHOT\n'
}

echo 'CPU_ACCOUNTING_DIAGNOSTIC 1'
echo "LIGHT_MODE $light"
uname -a
busybox 2>&1 | head -n 2
file /proc/cpuinfo
file /proc/cmdline
file /proc/sys/kernel/random/boot_id
file /sys/devices/system/clocksource/clocksource0/current_clocksource
file /sys/devices/system/clocksource/clocksource0/available_clocksource
printf 'KERNEL_CONFIG\n'
if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz
else
    echo UNSUPPORTED
fi
echo END_KERNEL_CONFIG
found=0
for path in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/* \
            /sys/devices/system/cpu/cpu[0-9]*/cpuidle/state*/*; do
    if [ -f "$path" ] && [ -r "$path" ]; then found=1; file "$path"; fi
done
[ "$found" -eq 1 ] || echo 'CPU frequency / cpuidle sysfs statistics: UNSUPPORTED'
for seconds do
    echo "INTERVAL $seconds"
    snapshot before
    sleep "$seconds"
    snapshot after
done
echo END_DIAGNOSTIC
