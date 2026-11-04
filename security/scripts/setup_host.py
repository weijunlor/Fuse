#!/usr/bin/env python3
"""Idempotent host provisioning; never replace existing passwords or keys."""
import fcntl
import hashlib
import hmac
import os
from pathlib import Path
import re
import secrets

import accounts
import lan
import security


def ensure_accounts(auth):
    auth.mkdir(parents=True, exist_ok=True, mode=0o700)
    if auth.stat().st_uid != os.getuid():
        raise ValueError('The account directory must belong to the current Linux user')
    auth.chmod(0o700)
    database = Path(os.environ.get('FUSE_AUTH_USERS', str(auth / 'users.db')))
    if not database.is_absolute() or str(database.resolve()).startswith('/mnt/') or any(
            parent.is_symlink() for parent in (database, *database.parents)):
        raise ValueError('Use a non-symlinked private Linux database path outside /mnt')
    entries = {}
    if database.exists():
        for line in lan.private_read(database).decode('ascii').splitlines():
            if not line or line.startswith('#'):
                continue
            fields = line.split(':')
            if len(fields) != 2 or not re.fullmatch(r'[A-Za-z0-9_-]{1,15}', fields[0]) or not re.fullmatch(
                    r'[0-9a-fA-F]{64}', fields[1]) or fields[0] in entries:
                raise ValueError('Invalid existing account database; no credentials were replaced')
            entries[fields[0]] = fields[1]

    updated = dict(entries)
    new_passwords = {}
    # Validate the entire existing set before writing anything. Password-only
    # leftovers from an interrupted first setup can reconstruct their DB entry.
    for number in range(1, 6):
        name = f'client-{number}'
        password_path = auth / f'{name}.pass'
        if password_path.exists() or password_path.is_symlink():
            password = lan.private_read(password_path).decode('utf-8').removesuffix('\n').removesuffix('\r')
            if not password or len(password.encode('utf-8')) > 256 or any(c in password for c in '\r\n\0'):
                raise ValueError(f'Invalid existing password for {name}; no credentials were replaced')
        elif name in entries:
            raise ValueError(f'Missing password for existing {name}; restore its private .pass file. Setup will not reset accounts.')
        else:
            password = secrets.token_urlsafe(24)
            new_passwords[name] = password
        key = hashlib.pbkdf2_hmac('sha256', password.encode('utf-8'), name.encode('ascii'), accounts.ROUNDS).hex()
        if name in entries and not hmac.compare_digest(entries[name].lower(), key):
            raise ValueError(f'Existing password/database mismatch for {name}; no credentials were replaced')
        updated[name] = key

    if len(updated) > 32:
        raise ValueError('The server supports at most 32 login accounts; no credentials were replaced')
    for name, password in new_passwords.items():
        fd = os.open(auth / f'{name}.pass', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as stream:
            stream.write(password + '\n')
    if updated != entries:
        database.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        accounts.private_write(database, '# FUSE PBKDF2-HMAC-SHA256, 600000 rounds, account-name salt\n' +
                               ''.join(f'{name}:{key}\n' for name, key in updated.items()))
    print('Ready: five host login accounts (existing credentials preserved)')


def main():
    root, auth = lan.roots()  # Reject Windows-mounted or symlinked credential roots.
    root = security.root_dir()
    fd = os.open(root / '.setup.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        security.make_certs(root, security.IDENTITIES, False)
        ensure_accounts(auth)


if __name__ == '__main__':
    main()
