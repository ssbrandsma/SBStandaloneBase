#!/usr/bin/env python3
"""Regression tests for the offline investigation parser, not a CPU estimator."""
import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('cpu_diagnostic', ROOT/'tools/cpu_accounting_analyze.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def record(pid=7, name='odd (worker) name)', ticks=10, start=100, children=90000):
    fields = {3:'S', 4:1, 14:ticks, 15:4, 16:children, 17:children, 22:start}
    return f'{pid} ({name}) ' + ' '.join(str(fields.get(i, 0)) for i in range(3, 25))


def sample(time, cpu, records):
    tasks = { (p['pid'], p['start']):p for p in map(module.process_stat, records) }
    return dict(interval=10, parse_errors=0, processes=tasks, threads=tasks.copy(),
                files={'/proc/uptime':[f'{time} 33.72', f'{time} 33.72'],
                       '/proc/stat':[f'cpu {cpu}\ncpu0 {cpu}']})


class DiagnosticTests(unittest.TestCase):
    def test_names_and_fields_not_child_time(self):
        p = module.process_stat(record())
        self.assertEqual((p['comm'],p['user'],p['system'],p['start']),
                         ('odd (worker) name)',10,4,100))

    def test_rt_fields_are_overlapping(self):
        a = sample(10,'92923 0 13118 3370 2 49 38 0 88197 8875 0',[record()])
        b = sample(27.49,'94349 0 13441 3370 2 49 38 0 89623 9197 0',[record(ticks=110)])
        result = module.compare(a,b)
        self.assertEqual(result['total_base_ticks'],1749)
        self.assertEqual(result['processes']['matched_ticks'],100)
        self.assertEqual(result['idle_ticks'],0)
        self.assertEqual(result['unattributed_ticks_not_idle'],1649)
        self.assertIsNone(result['idle_sleeptime_delta'])

    def test_reuse_new_exited_not_matched(self):
        a = sample(10,'100 0 0 100',[record(),record(pid=8)])
        b = sample(20,'200 0 0 100',[record(start=999),record(pid=9)])
        r = module.compare(a,b)['processes']
        self.assertEqual((r['matched_ticks'],r['new'],r['vanished']),(0,2,2))

    def test_reset_rejected(self):
        with self.assertRaises(ValueError):
            module.compare(sample(10,'100 0 0 100',[]),sample(20,'1 0 0 1',[]))

    def test_boot_change_rejected(self):
        a, b = sample(10,'100 0 0 100',[]), sample(20,'200 0 0 100',[])
        a['files']['/proc/sys/kernel/random/boot_id']=['first']
        b['files']['/proc/sys/kernel/random/boot_id']=['second']
        with self.assertRaises(ValueError):
            module.compare(a,b)

    def test_timer_evidence_not_process_residual(self):
        a = sample(10,'100 0 0 100',[record()])
        b = sample(20,'1100 0 0 100',[record(ticks=210)])
        a['files']['/proc/timer_list']=[' .idle_sleeptime : 3000000000 nsecs']
        b['files']['/proc/timer_list']=[' .idle_sleeptime : 8000000000 nsecs']
        r = module.compare(a,b)
        self.assertEqual(r['idle_sleeptime_delta'],5000000000)
        self.assertEqual(r['unattributed_ticks_not_idle'],800)

    def test_raw_section_boundaries(self):
        text = 'INTERVAL 5\nSNAPSHOT before\nFILE /proc/uptime\n10 1\nEND_FILE\n'
        text += 'PROCESS '+record()+'\nTHREAD 7 '+record()+'\nEND_SNAPSHOT\n'
        s = module.snapshots(text)[0]
        self.assertEqual(s['files']['/proc/uptime'],['10 1'])
        self.assertEqual(len(s['processes']),1)
        self.assertEqual(len(s['threads']),1)


if __name__ == '__main__':
    unittest.main()
