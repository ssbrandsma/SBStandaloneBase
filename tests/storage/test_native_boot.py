"""Native multi-UBIFS activation fixtures; no real mount or module operations."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BOOT = Path(sys.argv[1]).resolve()
SETUP = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else BOOT.with_name('storage-setup.sh')
sys.argv = sys.argv[:1]


class NativeBootTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name, content in {'proc/mounts':'', 'proc/filesystems':'nodev ubifs\n',
                              'etc/squeezeos.version':'9.0.2 r17157\n',
                              'sys/class/ubi/ubi0_5/name':'sbdata',
                              'sys/class/ubi/ubi0_5/type':'dynamic',
                              'sys/class/ubi/ubi0_5/corrupted':'0',
                              'sys/class/ubi/ubi0_5/upd_marker':'0'}.items():
            self.write(name, content)
        for name in ['sys/class/bdi/ubifs_0_2', 'mnt/sbdata/applets/StandaloneBase',
                     'mnt/sbdata/applets/SetupAppletInstaller', 'usr/share/jive/applets/StandaloneBase', 'bin', 'tmp']:
            (self.root / name).mkdir(parents=True, exist_ok=True)
        self.write('mnt/sbdata/config/keep.json', '{"preserve":true}')
        self.write('bin/mount', '''#!/bin/sh
echo "$*" >>"$SB_STORAGE_ROOT/actions"
test "${FAIL_MOUNT:-0}" = 0 || exit 1
case "$*" in
  *bind*) printf 'ubi0:sbdata %s/usr/share/jive/applets ubifs rw 0 0\n' "$SB_STORAGE_ROOT" >>"$SB_STORAGE_ROOT/proc/mounts" ;;
  *) printf 'ubi0:sbdata %s/mnt/sbdata ubifs rw 0 0\n' "$SB_STORAGE_ROOT" >>"$SB_STORAGE_ROOT/proc/mounts" ;;
esac
''').chmod(0o755)
        for command in ['insmod', 'rmmod', 'umount', 'ubiformat', 'ubimkvol', 'ubirmvol', 'ubiupdatevol']:
            self.write('bin/' + command, '#!/bin/sh\necho forbidden >>"$SB_STORAGE_ROOT/forbidden"\nexit 1\n').chmod(0o755)

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def run_boot(self, **extra):
        env = dict(os.environ, SB_STORAGE_ROOT=str(self.root), PATH=str(self.root/'bin') + ':' + os.environ['PATH'], **extra)
        result = subprocess.run(['sh', str(BOOT)], env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root/'forbidden').exists(), result.stderr)
        self.assertEqual((self.root/'mnt/sbdata/config/keep.json').read_text(), '{"preserve":true}')
        return result.stderr

    def test_native_no_module_and_repeated_execution(self):
        self.assertIn('activation complete', self.run_boot())
        actions = (self.root/'actions').read_text()
        self.assertIn('-t ubifs', actions)
        self.assertIn('activation complete', self.run_boot())
        self.assertEqual(actions, (self.root/'actions').read_text())

    def test_existing_native_mount(self):
        self.write('proc/mounts', f'ubi0:sbdata {self.root}/mnt/sbdata ubifs rw 0 0\n')
        self.assertIn('activation complete', self.run_boot())
        self.assertNotIn('-t ubifs', (self.root/'actions').read_text())

    def test_missing_volume(self):
        (self.root/'sys/class/ubi/ubi0_5/name').unlink()
        self.assertIn('volume ID 5 is absent', self.run_boot())
        self.assertFalse((self.root/'actions').exists())

    def test_foreign_mount(self):
        self.write('proc/mounts', f'ubi0:ubifs {self.root}/mnt/sbdata ubifs rw 0 0\n')
        self.assertIn('occupied', self.run_boot())
        self.assertFalse((self.root/'actions').exists())

    def test_native_mount_failure_no_module_fallback(self):
        self.assertIn('no format attempted', self.run_boot(FAIL_MOUNT='1'))

    def test_unknown_legacy_kernel(self):
        (self.root/'sys/class/bdi/ubifs_0_2').rmdir()
        self.assertIn('compatibility is not established', self.run_boot())
        self.assertFalse((self.root/'actions').exists())

    def prepare_repair(self):
        import shutil
        for key, value in dict(alignment='1', reserved_ebs='521', data_bytes='67221504', usable_eb_size='129024').items():
            self.write('sys/class/ubi/ubi0_5/' + key, value)
        self.write('sys/class/ubi/ubi0/min_io_size', '2048')
        self.write('dev/ubi0_5', '')
        self.write('etc/init.d/rcS', '#!/bin/sh\n/etc/init.d/rcS.local\n')
        self.write('etc/init.d/rcS.local', '#!/bin/sh\necho keep-other-hook\n# STANDALONEBASE-EXTENDED-STORAGE\n/bin/sh /mnt/storage/standalonebase/storage-boot.sh || true\nexit 0\n')
        self.write('mnt/storage/standalonebase/config.json', '{"preserve":true}')
        self.write('proc/mounts', f'ubi0:sbdata {self.root}/mnt/sbdata ubifs rw 0 0\n')
        shutil.copyfile(BOOT, self.root/'usr/share/jive/applets/StandaloneBase/storage-boot.sh')
        self.write('usr/share/jive/applets/StandaloneBase/sb-storage-helper', '#!/bin/sh\nif test "$1" = verify-copy; then exec /usr/bin/cmp -s "$2" "$3"; fi\nexit 0\n').chmod(0o755)

    def repair(self):
        return subprocess.run(['sh', str(SETUP), 'repair'], capture_output=True, text=True, timeout=10,
            env=dict(os.environ, SB_STORAGE_ROOT=str(self.root),
                     SB_STORAGE_APPLET_DIR=str(self.root/'usr/share/jive/applets/StandaloneBase'),
                     PATH=str(self.root/'bin') + ':' + os.environ['PATH']))

    def test_repair_retains_configuration_and_is_idempotent(self):
        self.prepare_repair()
        result = self.repair()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        legacy = self.root/'mnt/storage/standalonebase'
        retained = self.root/'etc/standalonebase'
        self.assertTrue(legacy.is_symlink())
        self.assertEqual((legacy/'config.json').read_text(), '{"preserve":true}')
        self.assertEqual((retained/'storage-boot.sh').read_bytes(), BOOT.read_bytes())
        hook = (self.root/'etc/init.d/rcS.local').read_text()
        self.assertIn('keep-other-hook', hook)
        self.assertEqual(hook.count('# STANDALONEBASE-EXTENDED-STORAGE'), 1)
        self.assertIn('/etc/standalonebase/storage-boot.sh', hook)
        files = sorted(str(p) for p in (self.root/'etc').rglob('*'))
        self.assertEqual(self.repair().returncode, 0)
        self.assertEqual(hook, (self.root/'etc/init.d/rcS.local').read_text())
        self.assertEqual(files, sorted(str(p) for p in (self.root/'etc').rglob('*')))
        self.assertFalse((self.root/'forbidden').exists())
        self.assertFalse((self.root/'actions').exists(), 'repair must not bind beneath running Jive')

    def test_interrupted_or_conflicting_repair_stops_without_data_loss(self):
        self.prepare_repair()
        self.write('etc/standalonebase/config.json', '{"other":true}')
        result = self.repair()
        self.assertEqual(result.returncode, 27, result.stdout + result.stderr)
        self.assertEqual((self.root/'etc/standalonebase/config.json').read_text(), '{"other":true}')
        self.assertEqual((self.root/'mnt/storage/standalonebase/config.json').read_text(), '{"preserve":true}')
        self.assertFalse((self.root/'forbidden').exists())

    def test_interrupted_file_install_can_be_retried(self):
        self.prepare_repair()
        self.write('bin/cp', '''#!/bin/sh
case "$*" in *storage-boot.sh.new*) exit 1;; esac
exec /bin/cp "$@"
''').chmod(0o755)
        self.assertEqual(self.repair().returncode, 27)
        self.assertEqual((self.root/'mnt/storage/standalonebase/config.json').read_text(), '{"preserve":true}')
        (self.root/'bin/cp').unlink()
        self.assertEqual(self.repair().returncode, 0)
        self.assertEqual((self.root/'etc/standalonebase/storage-boot.sh').read_bytes(), BOOT.read_bytes())

    def test_upgrade_retention_policy(self):
        # Model the observed /init file filter, not another real firmware flash.
        import re
        self.prepare_repair()
        self.assertEqual(self.repair().returncode, 0)
        keep = [r'settings.lua$', r'/etc/', r'/root/']
        for relative in ['etc/standalonebase/storage-boot.sh', 'etc/standalonebase/config.json', 'etc/init.d/rcS.local']:
            physical = '/mnt/storage/' + relative
            self.assertTrue(any(re.search(pattern, physical) for pattern in keep))
        self.assertFalse(any(re.search(pattern, '/mnt/storage/mnt/storage/standalonebase/storage-boot.sh') for pattern in keep))

    def test_native_existing_mount_command_is_idempotent(self):
        self.prepare_repair()
        env = dict(os.environ, SB_STORAGE_ROOT=str(self.root),
                   SB_STORAGE_APPLET_DIR=str(self.root/'usr/share/jive/applets/StandaloneBase'),
                   PATH=str(self.root/'bin') + ':' + os.environ['PATH'])
        result = subprocess.run(['sh', str(SETUP), 'mount', 'unused-module'], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 10)
        self.assertFalse((self.root/'actions').exists())
        result = subprocess.run(['sh', str(SETUP), 'initialize', 'unused-module'], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 20)
        self.assertFalse((self.root/'forbidden').exists())


unittest.main()
