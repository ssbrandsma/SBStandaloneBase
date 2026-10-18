#!/usr/bin/env python3
"""Offline analysis of raw BusyBox CPU diagnostics; never labels a gap as idle.

The target needs only cpu-accounting-diagnostic.sh. Run this on a workstation:
python tools/cpu_accounting_analyze.py capture.txt
"""
import json
import re
import sys
from pathlib import Path


def process_stat(line):
    """Parse fields 14/15/22 after the final comm delimiter; omit child time."""
    left, right = line.index('('), line.rindex(')')
    pid = int(line[:left].strip())
    fields = line[right + 1:].split()
    if len(fields) < 20:
        raise ValueError('Truncated process stat')
    return dict(pid=pid, comm=line[left + 1:right], state=fields[0],
                user=int(fields[11]), system=int(fields[12]),
                start=int(fields[19]))


def cpu_stat(line):
    """Keep all raw fields, total only the eight nonoverlapping base counters."""
    fields = line.split()
    if fields[0] != 'cpu' or len(fields) < 5:
        raise ValueError('Malformed aggregate CPU stat')
    counters = [int(v) for v in fields[1:]]
    return counters


def snapshots(text):
    """Decode sectioned raw output, retaining both snapshot boundary reads."""
    result, sample, path, content = [], None, None, []
    interval = None
    for line in text.splitlines():
        if line.startswith('INTERVAL '):
            interval = int(line.split()[1])
        elif line.startswith('SNAPSHOT '):
            sample = dict(interval=interval, label=line.split()[1], files={},
                          processes={}, threads={}, parse_errors=0)
        elif sample is not None:
            if line.startswith('FILE '):
                path, content = line[5:], []
            elif line == 'END_FILE':
                sample['files'].setdefault(path, []).append('\n'.join(content))
                path = None
            elif path:
                content.append(line)
            elif line.startswith(('PROCESS ', 'THREAD ')):
                try:
                    if line.startswith('PROCESS '):
                        p = process_stat(line[8:])
                        sample['processes'][(p['pid'], p['start'])] = p
                    else:
                        tgid, record = line[7:].split(' ', 1)
                        p = process_stat(record)
                        sample['threads'][(int(tgid), p['pid'], p['start'])] = p
                except (ValueError, IndexError):
                    sample['parse_errors'] += 1
            elif line == 'END_SNAPSHOT':
                result.append(sample)
                sample = None
    return result


def timer_value(sample, name):
    raw = sample['files'].get('/proc/timer_list', [''])[0]
    match = re.search(r'\.' + re.escape(name) + r'\s*:\s*(\d+)', raw)
    return int(match[1]) if match else None


def compare(before, after):
    """Report independently measured counters and explicit endpoint limitations."""
    if before['files'].get('/proc/sys/kernel/random/boot_id') != after['files'].get('/proc/sys/kernel/random/boot_id'):
        raise ValueError('Boot identifier changed')
    def uptime(s):
        return [float(v.split()[0]) for v in s['files']['/proc/uptime']]

    a, b = uptime(before), uptime(after)
    elapsed = (b[0] + b[-1] - a[0] - a[-1]) / 2
    c0 = cpu_stat(before['files']['/proc/stat'][0].splitlines()[0])
    c1 = cpu_stat(after['files']['/proc/stat'][0].splitlines()[0])
    delta = [y - x for x, y in zip(c0, c1)]
    if elapsed <= 0 or b[0] <= a[0] or any(v < 0 for v in delta):
        raise ValueError('Nonmonotonic counters or elapsed time')
    total = sum(delta[:8])
    busy = total - delta[3] - (delta[4] if len(delta) > 4 else 0)
    report = dict(requested_seconds=before['interval'], elapsed_seconds=elapsed,
                  collection_span_seconds=[a[-1] - a[0], b[-1] - b[0]],
                  cpu_field_deltas=delta, total_base_ticks=total, busy_ticks=busy,
                  cpu_elapsed_seconds=b[0] - a[0],
                  tick_rate=total / (b[0] - a[0]), idle_ticks=delta[3],
                  parse_errors=before['parse_errors'] + after['parse_errors'])
    for kind in ('processes', 'threads'):
        old, new = before[kind], after[kind]
        shared = old.keys() & new.keys()
        rows = []
        for key in shared:
            ticks = new[key]['user'] + new[key]['system'] - old[key]['user'] - old[key]['system']
            if ticks < 0:
                raise ValueError('Task CPU counter reset')
            rows.append(dict(pid=new[key]['pid'], comm=new[key]['comm'], ticks=ticks))
        report[kind] = dict(matched_ticks=sum(r['ticks'] for r in rows),
                            matched=len(shared), new=len(new.keys() - old.keys()),
                            vanished=len(old.keys() - new.keys()),
                            top=sorted(rows, key=lambda r: (-r['ticks'], r['pid']))[:10])
    report['unattributed_ticks_not_idle'] = (
        busy - report['processes']['matched_ticks']
        if before['processes'] and after['processes'] else None)
    for name in ('idle_sleeptime', 'idle_calls', 'idle_sleeps', 'nr_events'):
        x, y = timer_value(before, name), timer_value(after, name)
        report[name + '_delta'] = y - x if x is not None and y is not None else None
    for sample in (before, after):
        raw = sample['files']['/proc/stat'][0].splitlines()
        aggregate = raw[0].split()[1:]
        per_cpu = [r.split()[1:] for r in raw if r.startswith('cpu0 ')]
        if per_cpu and aggregate != per_cpu[0]:
            report['aggregate_cpu0_mismatch'] = True
    return report


def main():
    samples = snapshots(Path(sys.argv[1]).read_text())
    if len(samples) % 2:
        raise ValueError('Incomplete diagnostic')
    print(json.dumps([compare(a, b) for a, b in zip(samples[::2], samples[1::2])], indent=2))


if __name__ == '__main__':
    main()
