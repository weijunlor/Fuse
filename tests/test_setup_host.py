"""Real credential provisioning in temporary Linux directories only."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'security/scripts'))
import setup_host


class HostSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='fuse-full-setup-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.auth = self.root / 'auth'
        self.security = self.root / 'security'
        self.env = patch.dict(os.environ, {'FUSE_SECURITY_DIR': str(self.security),
                                          'FUSE_AUTH_DIR': str(self.auth)})
        self.env.start()
        self.addCleanup(self.env.stop)
        os.environ.pop('FUSE_AUTH_USERS', None)

    def snapshot(self):
        return {str(p.relative_to(self.root)): p.read_bytes() for p in self.root.rglob('*')
                if p.is_file() and p.name != '.setup.lock'}

    def test_full_host_rerun_preserves_keys_accounts_and_assignments(self):
        setup_host.main()
        registry = self.security / 'pairing/devices.json'
        registry.parent.mkdir(mode=0o700)
        registry.write_text('{"saved": "assignment"}')
        before = self.snapshot()
        setup_host.main()
        self.assertEqual(before, self.snapshot())
        for number in range(1, 6):
            path = self.auth / f'client-{number}.pass'
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_password_only_interruption_is_recovered_without_reset(self):
        self.auth.mkdir(mode=0o700)
        password = self.auth / 'client-1.pass'
        password.write_bytes(b'preserve-this-password\n')
        password.chmod(0o600)
        setup_host.ensure_accounts(self.auth)
        self.assertEqual(password.read_bytes(), b'preserve-this-password\n')
        expected = hashlib.pbkdf2_hmac('sha256', b'preserve-this-password', b'client-1', 600000).hex()
        self.assertIn('client-1:' + expected, (self.auth / 'users.db').read_text())

    def test_missing_password_never_resets_existing_account(self):
        setup_host.ensure_accounts(self.auth)
        (self.auth / 'client-3.pass').unlink()
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'Missing password'):
            setup_host.ensure_accounts(self.auth)
        self.assertEqual(before, self.snapshot())

    def test_mismatch_and_insecure_files_are_rejected_without_overwrite(self):
        setup_host.ensure_accounts(self.auth)
        password = self.auth / 'client-5.pass'
        password.write_text('wrong-password\n')
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            setup_host.ensure_accounts(self.auth)
        self.assertEqual(before, self.snapshot())
        password.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'owner-only'):
            setup_host.ensure_accounts(self.auth)

    def test_symlinked_credential_root_is_rejected(self):
        target = self.root / 'elsewhere'
        target.mkdir()
        self.security.symlink_to(target)
        with self.assertRaisesRegex(ValueError, 'Symlinked'):
            setup_host.main()
        self.assertEqual(list(target.iterdir()), [])

    def test_existing_named_accounts_are_preserved(self):
        self.auth.mkdir(mode=0o700)
        database = self.auth / 'users.db'
        existing = 'named-user:' + 'ab' * 32 + '\n'
        database.write_text(existing)
        database.chmod(0o600)
        setup_host.ensure_accounts(self.auth)
        self.assertIn(existing, database.read_text())

    def test_account_limit_fails_before_creating_passwords(self):
        self.auth.mkdir(mode=0o700)
        database = self.auth / 'users.db'
        database.write_text(''.join(f'user-{number}:' + 'ab' * 32 + '\n' for number in range(28)))
        database.chmod(0o600)
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'at most 32'):
            setup_host.ensure_accounts(self.auth)
        self.assertEqual(before, self.snapshot())


if __name__ == '__main__':
    unittest.main()
