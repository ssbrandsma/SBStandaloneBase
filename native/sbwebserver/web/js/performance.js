(() => {
    'use strict';
    const INTERVALS = [1, 2, 5, 10, 30, 60],
        WINDOW_SECONDS = 600;
    let active = false,
        timer = null,
        busy = false,
        interval = 2,
        previous = null,
        boot = '',
        netChoice = '',
        diskChoice = '',
        sortKey = 'cpu',
        sortDirection = -1,
        filter = '',
        processData = [];
    const history = {
            time: [],
            cpu: [],
            memory: [],
            rx: [],
            tx: [],
            read: [],
            write: []
        },
        plots = {};
    const bytes = value => {
        let n = Number(value) || 0,
            i = 0;
        const units = ['B', 'KiB', 'MiB', 'GiB'];
        while (n >= 1024 && i < 3) {
            n /= 1024;
            i++
        }
        return n.toFixed(i ? 1 : 0) + ' ' + units[i]
    };
    const rate = value => bytes(value) + '/s',
        totalCpu = cpu => Object.values(cpu).reduce((sum, value) => sum + Number(value || 0), 0),
        busyCpu = cpu => totalCpu(cpu) - Number(cpu.idle || 0) - Number(cpu.iowait || 0);
    const validDelta = (next, last) => Number.isFinite(next - last) && next >= last ? next - last : null;

    function addPoint(time, values, gap = false) {
        history.time.push(time);
        for (const key of ['cpu', 'memory', 'rx', 'tx', 'read', 'write']) history[key].push(gap ? null : values[key]);
        const cutoff = time - WINDOW_SECONDS;
        while (history.time.length && history.time[0] < cutoff)
            for (const key of Object.keys(history)) history[key].shift();
        for (const entry of Object.values(plots)) entry.plot.setData([history.time, ...entry.keys.map(key => history[key])])
    }

    function chart(id, key, color, label, percent = false, key2 = key === 'rx' ? 'tx' : key === 'read' ? 'write' : '', color2 = key === 'rx' ? '#2a65b7' : '#9b4cbe', label2 = key === 'rx' ? 'TX' : 'Write') {
        const element = document.querySelector('#' + id);
        if (!element || !window.uPlot) return;
        const keys = key2 ? [key, key2] : [key],
            series = [{}, {
                label,
                stroke: color,
                width: 2,
                fill: color + '18'
            }];
        if (key2) series.push({
            label: label2,
            stroke: color2,
            width: 2
        });
        const legendVisibility = {
            hooks: {
                init: [u => u.root.classList.add('legend-idle')],
                setCursor: [u => u.root.classList.toggle('legend-idle', u.cursor.idx == null)]
            }
        };
        plots[key] = {
            keys,
            plot: new uPlot({
                width: element.clientWidth || 300,
                height: 150,
                legend: {
                    show: true
                },
                cursor: {
                    show: true
                },
                scales: {
                    x: {
                        time: true
                    },
                    y: {
                        range: (u, min, max) => [0, Math.max(max || 1, percent ? 100 : 1)]
                    }
                },
                axes: [{}, {
                    values: (u, v) => v.map(x => percent ? x.toFixed(0) + '%' : rate(x))
                }],
                series,
                plugins: [legendVisibility]
            }, [history.time, ...keys.map(name => history[name])], element)
        }
    }

    function selection(list, name) {
        return list.find(item => item.name === name) || list.find(item => item.name !== 'lo') || list[0]
    }

    function metrics(snapshot, baseline) {
        if (!baseline || baseline.boot_id !== snapshot.boot_id) return null;
        if (Number(snapshot.clock_ticks_per_second) !== Number(baseline.clock_ticks_per_second)) return null;
        const elapsed = snapshot.uptime_seconds - baseline.uptime_seconds;
        if (!(elapsed > 0 && elapsed <= Math.max(interval * 5, 120))) return null;
        const total = validDelta(totalCpu(snapshot.cpu), totalCpu(baseline.cpu)),
            busyTicks = validDelta(busyCpu(snapshot.cpu), busyCpu(baseline.cpu));
        const m = snapshot.memory_kib,
            used = Math.max(0, m.total - m.free - m.buffers - m.cached),
            net = selection(snapshot.network, netChoice),
            oldNet = net && baseline.network.find(x => x.name === net.name),
            disk = selection(snapshot.disk.devices || [], diskChoice),
            oldDisk = disk && baseline.disk.devices?.find(x => x.name === disk.name);
        return {
            elapsed,
            cpu: total && busyTicks != null ? 100 * busyTicks / total : null,
            memory: m.total ? 100 * used / m.total : null,
            used,
            net,
            rx: oldNet ? validDelta(net.rx_bytes, oldNet.rx_bytes) / elapsed : null,
            tx: oldNet ? validDelta(net.tx_bytes, oldNet.tx_bytes) / elapsed : null,
            disk,
            read: oldDisk ? validDelta(disk.sectors_read, oldDisk.sectors_read) * 512 / elapsed : null,
            write: oldDisk ? validDelta(disk.sectors_written, oldDisk.sectors_written) * 512 / elapsed : null,
            totalTicks: total
        }
    }

    function calculateProcessData(snapshot, baseline, metric) {
        const old = new Map((baseline?.processes || []).map(p => [p.pid + ':' + p.start_ticks, p])),
            totalMemory = snapshot.memory_kib.total * 1024,
            elapsed = metric?.elapsed,
            ticksPerSecond = Number(snapshot.clock_ticks_per_second),
            wallTicks = metric?.totalTicks > 0 && elapsed > 0 && ticksPerSecond > 0 ? elapsed * ticksPerSecond : 0;
        return snapshot.processes.map(p => {
            const prior = old.get(p.pid + ':' + p.start_ticks),
                ticks = prior ? validDelta(p.user_ticks + p.system_ticks, prior.user_ticks + prior.system_ticks) : null,
                rss = Math.max(0, p.rss_pages * snapshot.page_size);
            return {
                ...p,
                cpu: wallTicks && ticks != null ? 100 * ticks / wallTicks : 0,
                memory: totalMemory ? 100 * rss / totalMemory : 0,
                rss
            }
        })
    }

    function text(id, value) {
        const node = document.querySelector('#' + id);
        if (node) node.textContent = value
    }

    function selectors(snapshot) {
        const refill = (select, items, current) => {
            const names = items.map(x => x.name);
            if (select.dataset.names !== names.join(',')) {
                select.dataset.names = names.join(',');
                select.innerHTML = items.map(x => '<option>' + esc(x.name) + '</option>').join('');
                select.value = names.includes(current) ? current : (items.find(x => x.name !== 'lo') || items[0] || {}).name || ''
            }
            return select.value
        };
        netChoice = refill(document.querySelector('#perf-network'), snapshot.network.filter(x => x.name !== 'lo'), netChoice);
        diskChoice = refill(document.querySelector('#perf-disk'), snapshot.disk.devices || [], diskChoice);
        const diskSelector = document.querySelector('#perf-disk');
        diskSelector.disabled = !snapshot.disk.available;
        diskSelector.hidden = !snapshot.disk.available
    }

    function sortProcesses(rows, key = sortKey, direction = sortDirection, query = filter) {
        const q = query.trim().toLowerCase();
        const filtered = q ? rows.filter(p => String(p.pid).includes(q) || p.comm.toLowerCase().includes(q) || p.cmd.toLowerCase().includes(q)) : [...rows];
        return filtered.sort((a, b) => {
            const av = a[key] ?? '',
                bv = b[key] ?? '';
            const primary = (typeof av === 'number' ? av - bv : String(av).localeCompare(String(bv))) * direction;
            return primary || a.pid - b.pid
        })
    }

    function updateSortHeaders() {
        document.querySelectorAll('[data-sort]').forEach(th => {
            const activeSort = th.dataset.sort === sortKey;
            th.classList.toggle('active-sort', activeSort);
            th.setAttribute('aria-sort', activeSort ? (sortDirection > 0 ? 'ascending' : 'descending') : 'none');
            const indicator = th.querySelector('.sort-indicator');
            if (indicator) indicator.textContent = activeSort ? (sortDirection > 0 ? '▲' : '▼') : ''
        })
    }

    function renderProcessRows() {
        const rows = sortProcesses(processData),
            body = document.querySelector('#perf-processes'),
            existing = new Map([...body.rows].map(row => [row.dataset.key, row])),
            seen = new Set;
        for (const p of rows) {
            const key = p.pid + ':' + p.start_ticks;
            seen.add(key);
            let row = existing.get(key);
            if (!row) {
                row = document.createElement('tr');
                row.dataset.key = key
            }
            row.innerHTML = '<td><strong>' + esc(p.comm) + '</strong></td><td>' + p.pid + '</td><td>' + p.ppid + '</td><td>' + esc(p.state) + '</td><td>' + p.cpu.toFixed(1) + '%</td><td>' + p.memory.toFixed(1) + '%</td><td>' + bytes(p.rss) + '</td><td>' + bytes(p.vsize_bytes) + '</td><td class="command-cell" title="' + esc(p.cmd) + '">' + esc(p.cmd) + '</td>';
            body.appendChild(row)
        }
        for (const row of [...body.rows])
            if (!seen.has(row.dataset.key)) row.remove();
        updateSortHeaders()
    }

    function processRows(snapshot, baseline, metric) {
        processData = calculateProcessData(snapshot, baseline, metric);
        renderProcessRows();
        text('perf-process-count', snapshot.processes.length + ' processes' + (snapshot.skipped_processes ? ' (' + snapshot.skipped_processes + ' skipped)' : ''))
    }

    function render(snapshot, baseline, metric) {
        selectors(snapshot);
        const m = snapshot.memory_kib,
            used = Math.max(0, m.total - m.free - m.buffers - m.cached);
        text('perf-cpu-value', metric?.cpu == null ? 'Waiting' : metric.cpu.toFixed(1) + '%');
        text('perf-memory-value', (100 * used / m.total).toFixed(1) + '%');
        text('perf-memory-detail', bytes(used * 1024) + ' estimated application · ' + bytes((m.cached + m.buffers) * 1024) + ' cache/buffers · ' + bytes(m.free * 1024) + ' free');
        text('perf-rx', metric?.rx == null ? 'Waiting' : rate(metric.rx));
        text('perf-tx', metric?.tx == null ? 'Waiting' : rate(metric.tx));
        text('perf-read', snapshot.disk.available ? (metric?.read == null ? 'Waiting' : rate(metric.read)) : 'Unavailable');
        text('perf-write', snapshot.disk.available ? (metric?.write == null ? 'Waiting' : rate(metric.write)) : snapshot.disk.reason || 'Unavailable');
        const diskChart = document.querySelector('#chart-read'),
            diskUnavailable = document.querySelector('#perf-disk-unavailable');
        diskChart.hidden = !snapshot.disk.available;
        diskUnavailable.hidden = snapshot.disk.available;
        if (snapshot.disk.available && !plots.read) chart('chart-read', 'read', '#d36b24', 'Read');
        text('perf-uptime', duration(snapshot.uptime_seconds));
        text('perf-updated', new Date().toLocaleTimeString());
        const status = document.querySelector('#perf-status');
        status.textContent = 'Live';
        status.className = 'status';
        processRows(snapshot, baseline, metric)
    }
    async function poll() {
        if (!active || busy || document.hidden) return;
        busy = true;
        try {
            const response = await fetch('/api/monitor/snapshot', {
                cache: 'no-store'
            });
            if (response.status === 401) {
                window.showPairing?.();
                return
            }
            if (!response.ok) throw new Error('HTTP ' + response.status);
            const snapshot = await response.json(),
                baseline = previous,
                metric = metrics(snapshot, baseline),
                now = Date.now() / 1000;
            if (boot && boot !== snapshot.boot_id) addPoint(now, {}, true);
            boot = snapshot.boot_id;
            if (metric) addPoint(now, metric);
            render(snapshot, baseline, metric);
            previous = snapshot
        } catch (error) {
            const status = document.querySelector('#perf-status');
            if (status) {
                status.textContent = 'Stale · ' + error.message;
                status.className = 'status offline'
            }
            previous = null
        } finally {
            busy = false;
            schedule()
        }
    }

    function schedule() {
        clearTimeout(timer);
        if (active && !document.hidden) timer = setTimeout(poll, interval * 1000)
    }

    function page() {
        active = true;
        app.innerHTML = '<div class="page-title perf-title"><div><h2>System Performance</h2><p>Live read-only measurements retained for ten minutes in this browser.</p></div><span id="perf-status" class="status">Loading</span></div><div class="toolbar perf-toolbar"><label>Polling interval <select id="perf-interval">' + INTERVALS.map(v => '<option value="' + v + '" ' + (v === interval ? 'selected' : '') + '>' + v + 's</option>').join('') + '</select></label><span>Updated: <strong id="perf-updated">Never</strong></span><span>Uptime: <strong id="perf-uptime">—</strong></span><span id="perf-process-count">0 processes</span></div><div class="perf-grid"><section class="card perf-card"><h3>CPU</h3><div id="perf-cpu-value" class="perf-value">Waiting</div><div id="chart-cpu"></div></section><section class="card perf-card"><h3>Memory <small>estimated application use</small></h3><div id="perf-memory-value" class="perf-value">Waiting</div><div id="perf-memory-detail" class="muted"></div><div id="chart-memory"></div></section><section class="card perf-card"><div class="perf-card-head"><h3>Network</h3><select id="perf-network"></select></div><div class="perf-pair"><span>RX <strong id="perf-rx">Waiting</strong></span><span>TX <strong id="perf-tx">Waiting</strong></span></div><div id="chart-rx"></div></section><section class="card perf-card"><div class="perf-card-head"><h3>Disk I/O</h3><select id="perf-disk"></select></div><div class="perf-pair"><span>Read <strong id="perf-read">Waiting</strong></span><span>Write <strong id="perf-write">Waiting</strong></span></div><div id="chart-read"></div></section></div><section class="card"><div class="process-head"><h3>Processes</h3><input id="perf-filter" type="search" placeholder="Filter name, PID or command"></div><div class="table-wrap"><table class="process-table"><thead><tr>' + [
            ['comm', 'Process'],
            ['pid', 'PID'],
            ['ppid', 'PPID'],
            ['state', 'State'],
            ['cpu', 'CPU %'],
            ['memory', 'Memory %'],
            ['rss', 'Resident'],
            ['vsize_bytes', 'Virtual'],
            ['cmd', 'Command line']
        ].map(([k, l]) => '<th data-sort="' + k + '">' + l + '</th>').join('') + '</tr></thead><tbody id="perf-processes"></tbody></table></div></section>';
        const diskChart = document.querySelector('#chart-read');
        diskChart.hidden = true;
        diskChart.insertAdjacentHTML('beforebegin', '<div id="perf-disk-unavailable" class="disk-unavailable" hidden><strong>Disk I/O statistics unavailable</strong><p>This Radio uses NAND/UBI storage, which may not expose conventional block-device throughput counters.</p></div>');
        document.querySelector('#perf-processes').closest('.table-wrap').classList.add('process-table-scroll');
        document.querySelectorAll('[data-sort]').forEach(th => th.insertAdjacentHTML('beforeend', ' <span class="sort-indicator" aria-hidden="true"></span>'));
        document.querySelector('#perf-interval').onchange = e => {
            interval = Number(e.target.value);
            schedule()
        };
        document.querySelector('#perf-network').onchange = e => {
            netChoice = e.target.value;
            previous = null;
            poll()
        };
        document.querySelector('#perf-disk').onchange = e => {
            diskChoice = e.target.value;
            previous = null;
            poll()
        };
        document.querySelector('#perf-filter').oninput = e => {
            filter = e.target.value;
            renderProcessRows()
        };
        document.querySelectorAll('[data-sort]').forEach(th => th.onclick = () => {
            const key = th.dataset.sort;
            if (sortKey === key) sortDirection *= -1;
            else {
                sortKey = key;
                sortDirection = ['comm', 'state', 'cmd'].includes(key) ? 1 : -1
            }
            renderProcessRows()
        });
        updateSortHeaders();
        chart('chart-cpu', 'cpu', '#1f78d1', 'CPU', true);
        chart('chart-memory', 'memory', '#7b61d1', 'Memory', true);
        chart('chart-rx', 'rx', '#078a68', 'RX');
        poll()
    }
    document.addEventListener('visibilitychange', () => {
        if (!active) return;
        if (document.hidden) {
            clearTimeout(timer);
            previous = null
        } else poll()
    });
    window.performancePage = page;
    window.performancePageCleanup = () => {
        active = false;
        clearTimeout(timer);
        timer = null;
        previous = null
    };
    window.sbMonitorCalculations = {
        validDelta,
        totalCpu,
        busyCpu,
        metrics,
        calculateProcessData,
        sortProcesses,
        intervals: INTERVALS,
        windowSeconds: WINDOW_SECONDS
    };
})();
