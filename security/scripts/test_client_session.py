#!/usr/bin/env python3
"""Concurrent launcher leases, persistent profiles and process cleanup."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import unittest

import security

ROOT = Path(__file__).resolve().parents[2]
WORKER = '''
import json, os, signal, time
signal.signal(signal.SIGTERM, lambda *_: exit(0))
print(json.dumps({'root': os.environ['FUSE_PAIRING_DIR'],
                  'host': os.environ.get('FUSE_SERVER_IP'), 'pid': os.getpid()}), flush=True)
while True: time.sleep(.1)
'''


class ClientSessionTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='fuse-window-tests-')
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name) / 'profiles'
        self.root.mkdir(mode=0o700)
        self.env = dict(os.environ, FUSE_PAIRING_DIR=str(self.root))
        self.env.pop('FUSE_SERVER_IP', None)
        self.children = []
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for process in self.children:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            process.stdout.close()
            process.stderr.close()

    def start(self):
        process = subprocess.Popen([sys.executable, 'security/scripts/client_session.py',
                                    sys.executable, '-c', WORKER], cwd=ROOT, env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(process)
        return process

    def ready(self, process):
        readable, _, _ = select.select([process.stdout], [], [], 5)
        self.assertTrue(readable, 'Client did not start')
        line = process.stdout.readline()
        if not line:
            self.fail(process.stderr.read())
        return json.loads(line)

    def test_five_distinct_profiles_and_capacity(self):
        roots = [self.ready(self.start())['root'] for _ in range(5)]
        self.assertEqual(len(set(roots)), 5)
        self.assertEqual(roots[0], str(self.root))
        sixth = self.start()
        _, error = sixth.communicate(timeout=5)
        self.assertNotEqual(sixth.returncode, 0)
        self.assertIn('Five client windows', error)
        for path in self.root.rglob('*'):
            self.assertEqual(path.stat().st_mode & 0o077, 0, path)

    def test_reuses_freed_profile_without_copying_credentials(self):
        security.write(self.root / 'device.pem', b'first-player-device')
        security.write(self.root / 'active', json.dumps({'version': 1, 'server': '192.168.1.50'}))
        first = self.start()
        self.assertEqual(self.ready(first)['root'], str(self.root))
        second = self.start()
        second_info = self.ready(second)
        self.assertNotEqual(second_info['root'], str(self.root))
        self.assertEqual(second_info['host'], '192.168.1.50')
        extra = Path(second_info['root'])
        self.assertFalse((extra / 'device.pem').exists())
        self.assertFalse((extra / 'active').exists())
        security.write(extra / 'active', 'saved-second-player')
        second.terminate()
        self.assertEqual(second.wait(timeout=5), 0)
        again = self.ready(self.start())
        self.assertEqual(again['root'], str(extra))
        self.assertIsNone(again['host'])  # Its own saved host takes precedence.
        self.assertEqual((extra / 'active').read_text(), 'saved-second-player')
        self.assertEqual((self.root / 'device.pem').read_bytes(), b'first-player-device')

    def test_game_crash_releases_profile(self):
        first = self.start()
        info = self.ready(first)
        os.kill(info['pid'], signal.SIGKILL)
        self.assertEqual(first.wait(timeout=5), 128 + signal.SIGKILL)
        self.assertEqual(self.ready(self.start())['root'], info['root'])

    def test_interrupt_releases_profile(self):
        first = self.start()
        info = self.ready(first)
        first.send_signal(signal.SIGINT)
        self.assertEqual(first.wait(timeout=5), 128 + signal.SIGINT)
        self.assertEqual(self.ready(self.start())['root'], info['root'])

    def test_symlink_and_insecure_locks_rejected(self):
        target = self.root / 'untouched'
        security.write(target, 'original')
        lock = self.root / 'window-1.lock'
        lock.symlink_to(target)
        failed = self.start()
        _, error = failed.communicate(timeout=5)
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(target.read_text(), 'original')
        lock.unlink()
        lock.write_text('')
        lock.chmod(0o644)
        failed = self.start()
        _, error = failed.communicate(timeout=5)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn('Private window lock', error)


if __name__ == '__main__':
    unittest.main()
