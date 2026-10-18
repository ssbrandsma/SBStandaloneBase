#!/usr/bin/env python3
"""Host-only recovery fixtures. No real mounts, modules, or flash operations."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SETUP, BOOT, HELPER, MODULE = map(lambda p: Path(p).resolve(), sys.argv[1:5])
sys.argv = sys.argv[:1]


class RecoveryTests(unittest.TestCase):
    def write(self, path, text):
        path = self.root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'usr/share/jive/applets'
        self.old = self.root / 'mnt/sbdata/applets'
        self.write('proc/sys/kernel/osrelease', '2.6.26.8-rt16\n')
        self.write('etc/squeezeos.version', '9.0.2 r17146\n')
        self.write('proc/cpuinfo', 'Hardware : Logitech MX25 Baby Board\n')
        self.write('proc/filesystems', 'nodev\tubifs\n')
        self.write('proc/mounts', f'ubi0:ubifs {self.root}/mnt/storage ubifs rw 0 0\n')
        self.write('etc/init.d/rcS', '#!/bin/sh\n/bin/mount -a\n/etc/init.d/squeezeplay start\n')
        self.write('etc/init.d/rcS.local', '#!/bin/sh\necho keep-custom-hook\nexit 0\n')
        self.write('mnt/storage/standalonebase/config.json', '{"keep":true}')
        self.write('tmp/.sentinel', '')
        self.write('dev/ubi0_5', '')
        attrs = dict(name='sbdata', type='dynamic', alignment='1', reserved_ebs='521',
                     data_bytes='67221504', usable_eb_size='129024', corrupted='0', upd_marker='0')
        for key, value in attrs.items():
            self.write('sys/class/ubi/ubi0_5/' + key, value + '\n')
        self.write('sys/class/ubi/ubi0/min_io_size', '2048\n')
        self.write('sys/class/ubi/ubi0/avail_eraseblocks', '129\n')
        self.write('usr/share/jive/applets/SetupAppletInstaller/core.lua', 'new firmware')
        self.write('usr/share/jive/applets/StandaloneBase/new.lua', 'new base')
        self.write('usr/share/jive/applets/StandaloneRadio/new.lua', 'new radio')
        self.write('mnt/sbdata/applets/SetupAppletInstaller/core.lua', 'old firmware')
        self.write('mnt/sbdata/applets/Spotify/old.lua', 'obsolete stock applet')
        self.write('mnt/sbdata/applets/StandaloneBase/new.lua', 'old base')
        self.write('mnt/sbdata/applets/StandaloneRadio/new.lua', 'old radio')
        self.write('mnt/sbdata/applets/StandaloneRadio/cache/logo.png', 'cached logo')
        self.write('mnt/sbdata/applets/CustomPlugin/plugin.lua', 'custom')
        self.write('mnt/sbdata/cache/outside-applets', 'preserve data')
        for src, name in [(HELPER, 'sb-storage-helper'), (MODULE, 'sbubifs-authorized.ko'),
                          (BOOT, 'storage-boot.sh')]:
            shutil.copy2(src, self.source / 'StandaloneBase' / name)
        scripts = {
            'insmod': 'test "${FAIL_MODULE:-0}" = 0 || exit 1\necho "nodev sbubifs" >>"$SB_STORAGE_ROOT/proc/filesystems"',
            'mount': 'echo "$*" >>"$SB_STORAGE_ROOT/tmp/mount.calls"\n'
                     'test "${FAIL_MOUNT:-0}" = 0 || exit 1\n'
                     'test "$1" != -t || echo "ubi0:sbdata $SB_STORAGE_ROOT/mnt/sbdata sbubifs ro 0 0" >>"$SB_STORAGE_ROOT/proc/mounts"',
            'umount': 'echo "$*" >>"$SB_STORAGE_ROOT/tmp/umount.calls"',
            'df': 'printf "Filesystem 1024-blocks Used Available Capacity Mounted\\nmock 64000 12000 %s 20%% mount\\n" "${FREE_KB:-52000}"',
            'cp': 'test "${FAIL_COPY:-0}" = 0 || exit 1\nexec /bin/cp "$@"',
            'sync': 'exit 0',
            'ubimkvol': 'echo BAD >>"$SB_STORAGE_ROOT/tmp/destructive.calls"; exit 99',
            'ubiupdatevol': 'echo BAD >>"$SB_STORAGE_ROOT/tmp/destructive.calls"; exit 99',
        }
        for name, body in scripts.items():
            self.write('bin/' + name, '#!/bin/sh\n' + body + '\n').chmod(0o755)
        self.env = dict(os.environ, SB_STORAGE_ROOT=str(self.root),
                        SB_STORAGE_APPLET_DIR=str(self.source / 'StandaloneBase'),
                        PATH=str(self.root / 'bin') + ':' + os.environ['PATH'])

    def run_script(self, command='recover', **env):
        result = subprocess.run(['sh', str(SETUP), command], env=dict(self.env, **env),
                                capture_output=True, text=True, timeout=15)
        self.assertFalse((self.root / 'tmp/destructive.calls').exists())
        return result

    def test_recovery_preserves_data_refreshes_core_and_requires_reboot(self):
        before = self.run_script('check')
        self.assertIn('RECOVERY_SUPPORTED=1', before.stdout)
        self.assertFalse((self.root / 'tmp/mount.calls').exists(), 'check must be read-only')
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('STATUS=REBOOT_REQUIRED', result.stdout)
        self.assertEqual((self.old / 'SetupAppletInstaller/core.lua').read_text(), 'new firmware')
        self.assertFalse((self.old / 'Spotify').exists())
        self.assertEqual((self.old / 'StandaloneRadio/new.lua').read_text(), 'new radio')
        self.assertEqual((self.old / 'StandaloneRadio/cache/logo.png').read_text(), 'cached logo')
        self.assertEqual((self.old / 'CustomPlugin/plugin.lua').read_text(), 'custom')
        backups = list((self.root / 'mnt/sbdata').glob('firmware-backup-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / 'previous-applets/SetupAppletInstaller/core.lua').read_text(), 'old firmware')
        self.assertTrue((self.root / 'mnt/sbdata/cache/outside-applets').exists())
        self.assertIn('keep-custom-hook', (self.root / 'etc/init.d/rcS.local').read_text())
        rcs = (self.root / 'etc/init.d/rcS').read_text()
        self.assertLess(rcs.index('rcS.local'), rcs.index('/etc/init.d/squeezeplay start'))
        self.assertTrue((self.root / 'mnt/storage/sbubifs-authorized.ko').exists())
        self.assertTrue((self.root / 'mnt/storage/standalonebase/storage-boot.sh').exists())
        native = subprocess.run([str(HELPER), 'storage-status'], env=self.env,
                                capture_output=True, text=True, check=True).stdout
        self.assertIn('boot_helper=present', native)
        self.assertIn('rcs_integrated=true', native)
        self.assertNotIn('bind', (self.root / 'tmp/mount.calls').read_text())
        calls = (self.root / 'tmp/mount.calls').read_bytes()
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual((self.root / 'tmp/mount.calls').read_bytes(), calls, 'repeat must not write')

    def test_preserves_existing_rcs_integration(self):
        self.write('etc/init.d/rcS', '#!/bin/sh\n/etc/init.d/rcS.local\n/etc/init.d/squeezeplay start\n')
        before = (self.root / 'etc/init.d/rcS').read_bytes()
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual((self.root / 'etc/init.d/rcS').read_bytes(), before)

    def test_corrupt_or_wrong_geometry_never_mounts(self):
        self.write('sys/class/ubi/ubi0_5/corrupted', '1\n')
        self.run_script()
        self.assertFalse((self.root / 'tmp/mount.calls').exists())
        self.write('sys/class/ubi/ubi0_5/corrupted', '0\n')
        self.write('sys/class/ubi/ubi0_5/reserved_ebs', '522\n')
        self.run_script()
        self.assertFalse((self.root / 'tmp/mount.calls').exists())

    def test_busy_operation_is_not_started(self):
        (self.root / 'tmp/sbstorage-operation.lock').mkdir()
        self.run_script()
        self.assertFalse((self.root / 'tmp/mount.calls').exists())

    def test_native_copy_comparison(self):
        left = self.write('tmp/left', 'x' * 9000)
        right = self.write('tmp/right', 'x' * 9000)
        def compare():
            return subprocess.run([str(HELPER), 'verify-copy', str(left), str(right)],
                                  capture_output=True).returncode
        self.assertEqual(compare(), 0)
        right.write_text('x' * 8999 + 'y')
        self.assertEqual(compare(), 2)
        right.unlink()
        self.assertEqual(compare(), 2)

    def test_interrupted_transaction_requires_review(self):
        (self.root / 'mnt/sbdata/.firmware-recovery.previous').mkdir()
        self.assertEqual(self.run_script().returncode, 26)
        self.assertEqual((self.old / 'SetupAppletInstaller/core.lua').read_text(), 'old firmware')

    def test_module_incompatibility_preserves_tree(self):
        self.assertEqual(self.run_script(FAIL_MODULE='1').returncode, 22)
        self.assertEqual((self.old / 'SetupAppletInstaller/core.lua').read_text(), 'old firmware')
        self.assertFalse((self.root / 'tmp/mount.calls').exists())

    def test_full_volume_and_failed_copy_preserve_original(self):
        self.assertEqual(self.run_script(FREE_KB='1').returncode, 26)
        self.assertEqual((self.old / 'SetupAppletInstaller/core.lua').read_text(), 'old firmware')
        # Reset the mock mount record before the independent retry.
        self.write('proc/mounts', f'ubi0:ubifs {self.root}/mnt/storage ubifs rw 0 0\n')
        self.assertEqual(self.run_script(FAIL_COPY='1').returncode, 25)
        self.assertEqual((self.old / 'SetupAppletInstaller/core.lua').read_text(), 'old firmware')

    def test_unknown_rcs_is_not_patched(self):
        before = '#!/bin/sh\necho unknown boot layout\n'
        self.write('etc/init.d/rcS', before)
        self.assertEqual(self.run_script().returncode, 27)
        self.assertEqual((self.root / 'etc/init.d/rcS').read_text(), before)
        self.assertFalse((self.root / 'tmp/mount.calls').exists())

    def test_old_custom_symlink_requires_review(self):
        (self.old / 'CustomPlugin/link').symlink_to('/tmp')
        self.assertEqual(self.run_script().returncode, 26)
        self.assertEqual((self.old / 'SetupAppletInstaller/core.lua').read_text(), 'old firmware')

    def test_active_volume_is_not_recovered(self):
        self.write('proc/mounts', f'ubi0:ubifs {self.root}/mnt/storage ubifs rw 0 0\n'
                   f'ubi0:sbdata {self.source} sbubifs rw 0 0\n')
        self.run_script()
        self.assertFalse((self.root / 'tmp/mount.calls').exists())


unittest.main()
