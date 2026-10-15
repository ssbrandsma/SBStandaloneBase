'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(process.argv[2], 'utf8');
const context = {
    window: {},
    document: { addEventListener() {} },
    console
};
vm.runInNewContext(source, context);
const sort = context.window.sbMonitorCalculations.sortProcesses;
const calculateMetrics = context.window.sbMonitorCalculations.metrics;
const calculateProcesses = context.window.sbMonitorCalculations.calculateProcessData;
const rows = [
    { pid: 30, ppid: 2, comm: 'same', state: 'S', cpu: 2, memory: 9.5, rss: 900, vsize_bytes: 100, cmd: 'worker beta' },
    { pid: 10, ppid: 20, comm: 'same', state: 'R', cpu: 11, memory: 2.1, rss: 100, vsize_bytes: 900, cmd: 'worker alpha' },
    { pid: 20, ppid: 3, comm: 'alpha', state: 'S', cpu: 2, memory: 12.2, rss: 500, vsize_bytes: 500, cmd: 'service' }
];

assert.deepStrictEqual(Array.from(sort(rows, 'cpu', -1, ''), x => x.pid), [10, 20, 30]);
assert.deepStrictEqual(Array.from(sort(rows, 'cpu', 1, ''), x => x.pid), [20, 30, 10]);
assert.deepStrictEqual(Array.from(sort(rows, 'memory', -1, ''), x => x.pid), [20, 30, 10]);
assert.deepStrictEqual(Array.from(sort(rows, 'rss', 1, ''), x => x.pid), [10, 20, 30]);
assert.deepStrictEqual(Array.from(sort(rows, 'ppid', 1, ''), x => x.pid), [30, 20, 10]);
assert.deepStrictEqual(Array.from(sort(rows, 'comm', 1, ''), x => x.pid), [20, 10, 30]);
assert.deepStrictEqual(Array.from(sort(rows, 'cpu', -1, 'worker'), x => x.pid), [10, 30]);

function procRow(pid, ticks, start = 100, comm = 'worker') {
    return {
        pid, ppid: 1, start_ticks: start, state: 'S', comm, cmd: comm,
        user_ticks: ticks, system_ticks: 0, vsize_bytes: 4096, rss_pages: 1
    };
}

function snapshot(uptime, cpu, processes) {
    return {
        boot_id: 'boot', uptime_seconds: uptime, clock_ticks_per_second: 100,
        page_size: 4096, cpu,
        memory_kib: { total: 1024, free: 100, buffers: 10, cached: 100 },
        network: [], disk: { available: false, devices: [] }, processes
    };
}

const cpu0 = { user: 100, nice: 0, system: 100, idle: 800, iowait: 0, irq: 0, softirq: 0, steal: 0 };
const cpu40 = { user: 250, nice: 0, system: 150, idle: 1100, iowait: 0, irq: 0, softirq: 0, steal: 0 };
const base = snapshot(10, cpu0, [procRow(1, 100), procRow(2, 200), procRow(3, 300)]);
const next = snapshot(15, cpu40, [procRow(1, 200), procRow(2, 250), procRow(3, 350)]);
const metric = calculateMetrics(next, base);
assert.strictEqual(metric.cpu, 40); // 200 busy ticks out of 500 total ticks.
let calculated = calculateProcesses(next, base, metric);
assert.strictEqual(calculated[0].cpu, 20); // 100 ticks / (5 seconds * 100 Hz).
assert.strictEqual(calculated.reduce((sum, p) => sum + p.cpu, 0), 40);

// Regression: an old tickless kernel may expose only 370 aggregate ticks over
// five wall-clock seconds. Dividing by that value produced 135.1%; wall time
// correctly reports a process that consumed 500 ticks as 100%.
const shortCpu = { user: 350, nice: 0, system: 101, idle: 919, iowait: 0, irq: 0, softirq: 0, steal: 0 };
const fullBase = snapshot(20, cpu0, [procRow(7, 100, 700, 'jive (ui)')]);
const fullNext = snapshot(25, shortCpu, [procRow(7, 600, 700, 'jive (ui)')]);
const shortMetric = calculateMetrics(fullNext, fullBase);
assert(Math.abs(100 * 500 / shortMetric.totalTicks - 135.135) < 0.01);
assert.strictEqual(calculateProcesses(fullNext, fullBase, shortMetric)[0].cpu, 100);

const almostNext = snapshot(25, cpu40, [procRow(7, 599, 700)]);
assert.strictEqual(calculateProcesses(almostNext, fullBase, calculateMetrics(almostNext, fullBase))[0].cpu, 99.8);

// A vanished process has no row; a reappearing process and a reused PID have
// no matching (pid,start_ticks) baseline and therefore start at zero.
assert.deepStrictEqual(Array.from(calculateProcesses(snapshot(15, cpu40, []), base, metric)), []);
assert.strictEqual(calculateProcesses(snapshot(15, cpu40, [procRow(1, 900, 999)]), base, metric)[0].cpu, 0);
assert.strictEqual(calculateProcesses(snapshot(15, cpu40, [procRow(9, 50, 900)]), base, metric)[0].cpu, 0);

// A different polling interval is safe because elapsed uptime defines the
// window. Sorting and filtering operate on derived rows and cannot alter it.
const tenSecond = snapshot(20, { ...cpu40, idle: 1600 }, [procRow(1, 300)]);
const tenMetric = calculateMetrics(tenSecond, base);
calculated = calculateProcesses(tenSecond, base, tenMetric);
assert.strictEqual(calculated[0].cpu, 20);
assert.strictEqual(sort(calculated, 'cpu', -1, 'worker')[0].cpu, 20);

const reset = snapshot(15, { user: 1, nice: 0, system: 1, idle: 1, iowait: 0, irq: 0, softirq: 0, steal: 0 }, [procRow(1, 200)]);
const resetMetric = calculateMetrics(reset, base);
assert.strictEqual(resetMetric.totalTicks, null);
assert.strictEqual(calculateProcesses(reset, base, resetMetric)[0].cpu, 0);
assert.strictEqual(calculateMetrics(next, null), null);
assert.strictEqual(calculateProcesses(next, null, null)[0].cpu, 0);

const incompatible = snapshot(15, cpu40, [procRow(1, 200)]);
incompatible.clock_ticks_per_second = 250;
assert.strictEqual(calculateMetrics(incompatible, base), null);

assert(source.includes('legend-idle') && source.includes('u.cursor.idx == null'));
assert(source.includes('perf-disk-unavailable') && source.includes('diskChart.hidden = !snapshot.disk.available'));
console.log('performance-logic-tests-ok');
