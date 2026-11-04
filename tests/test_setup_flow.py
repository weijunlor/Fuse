"""Exercise the real shell coordinator against isolated dependency/build stubs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]


class SetupFlowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='fuse-setup-flow-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'project with spaces'
        scripts = self.root / 'security/scripts'
        scripts.mkdir(parents=True)
        shutil.copyfile(REPO / 'run-setup.sh', self.root / 'run-setup.sh')
        (scripts / 'setup_wsl.sh').write_text('echo dependencies >> "$SETUP_LOG"\nexit "${DEPENDENCY_EXIT:-0}"\n')
        (scripts / 'env.sh').write_text('export SETUP_ENV_READY=yes\n')
        (scripts / 'setup_host.py').write_text('import os\nwith open(os.environ["SETUP_LOG"],"a") as f: f.write("host\\n")\n')
        binaries = self.root / 'bin'
        binaries.mkdir()
        make = binaries / 'make'
        make.write_text('#!/bin/sh\n[ "$SETUP_ENV_READY" = yes ] || exit 8\necho "make $*" >> "$SETUP_LOG"\nexit "${MAKE_EXIT:-0}"\n')
        make.chmod(0o700)
        self.log = self.root / 'steps.log'
        self.env = {**os.environ, 'SETUP_LOG': str(self.log), 'PATH': str(binaries) + ':' + os.environ['PATH']}

    def run_setup(self, *args, **extra):
        return subprocess.run(['bash', str(self.root / 'run-setup.sh'), *args], env={**self.env, **extra},
                              text=True, capture_output=True, check=False, timeout=15)

    def steps(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_host_provisions_after_dependencies_and_before_build(self):
        result = self.run_setup('--role', 'host')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.steps()[:2], ['dependencies', 'host'])
        self.assertRegex(self.steps()[2], r'^make -j[0-9]+ all baseline$')

    def test_player_does_not_create_host_credentials(self):
        result = self.run_setup('--role', 'player')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.steps()[0], 'dependencies')
        self.assertEqual(len(self.steps()), 2)

    def test_failures_stop_setup_and_do_not_report_success(self):
        result = self.run_setup('--role', 'host', DEPENDENCY_EXIT='4')
        self.assertEqual(result.returncode, 4)
        self.assertEqual(self.steps(), ['dependencies'])
        self.assertNotIn('setup complete', result.stdout)
        result = self.run_setup('--role', 'player', '--skip-dependencies', MAKE_EXIT='5')
        self.assertEqual(result.returncode, 5)
        self.assertNotIn('setup complete', result.stdout)

    def test_invalid_options_cannot_install_anything(self):
        for args in [(), ('--role',), ('--role', 'unknown'), ('--unknown',)]:
            result = self.run_setup(*args)
            self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.steps(), [])


if __name__ == '__main__':
    unittest.main()
