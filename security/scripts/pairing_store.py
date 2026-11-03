#!/usr/bin/env python3
"""Private storage behind the native TLS pairing helper; never prints credentials."""
from contextlib import redirect_stdout
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

import lan
import lan_network
import security


def private_dir(path):
    path = Path(path)
    if (not path.is_absolute() or str(path.resolve()).startswith('/mnt/') or
            path.resolve().is_relative_to(lan.REPO.resolve()) or any(c in str(path) for c in '\r\n')):
        raise ValueError('Use an absolute private Linux storage directory outside /mnt')
    for parent in (path, *path.parents):
        if parent.is_symlink():
            raise ValueError('Symlinked private directory refused')
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    if path.stat().st_uid != os.getuid():
        raise ValueError('Private storage must belong to you')
    path.chmod(0o700)
    return path


def durable_write(path, contents):
    """Commit claims before delivery, including across a power loss/restart."""
    fd, temporary = tempfile.mkstemp(dir=path.parent, prefix='.commit-')
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(contents.encode() if isinstance(contents, str) else contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def client_root():
    return private_dir(os.environ.get('FUSE_PAIRING_DIR', str(Path.home() / '.local/share/fuse/pairings')))


def host_root():
    root, _ = lan.roots()
    return private_dir(root / 'pairing')


def device():
    root = client_root()
    path = root / 'device.pem'
    # Serialise initial creation so two windows cannot silently replace a device key.
    with lock(root):
        if path.is_symlink():
            raise ValueError('Symlinked device key refused')
        if not path.exists():
            durable_write(path, security.run('openssl', 'genpkey', '-algorithm', 'ED25519'))
        lan.private_read(path)
    print(path)


def lock(root):
    class Locked:
        def __enter__(self):
            fd = os.open(root / 'store.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
            self.stream = os.fdopen(fd, 'r+b')
            info = os.fstat(fd)
            if info.st_uid != os.getuid() or info.st_mode & 0o077:
                self.stream.close()
                raise ValueError('Private lock file required')
            fcntl.flock(fd, fcntl.LOCK_EX)
            return self
        def __exit__(self, *unused):
            self.stream.close()
    return Locked()


def registry(root):
    path = root / 'devices.json'
    if path.is_symlink():
        raise ValueError('Symlinked device registry refused')
    data = json.loads(lan.private_read(path)) if path.exists() else {}
    if not isinstance(data, dict) or len(data) > 5:
        raise ValueError('Invalid device registry')
    if any(not re.fullmatch('[0-9a-f]{64}', key) or type(slot) is not int or slot not in range(1, 6)
           for key, slot in data.items()) or len(set(data.values())) != len(data):
        raise ValueError('Invalid device registry')
    return data


def issue(public_key):
    if not re.fullmatch('[0-9a-f]{64}', public_key):
        raise ValueError('Invalid device key')
    root = host_root()
    with lock(root):
        data = registry(root)
        slot = data.get(public_key)
        if slot is None:
            slot = next((i for i in range(1, 6) if i not in data.values()), None)
        if slot is None:
            return 3
        # Validation/export happens before reserving a slot. The reservation is
        # durable before a single credential byte reaches the network helper.
        with tempfile.TemporaryDirectory(prefix='fuse-pair-export-') as directory:
            bundle = Path(directory) / 'client.fuse-lan'
            with redirect_stdout(io.StringIO()):
                lan.export_bundle(slot, bundle)
            if public_key not in data:
                data[public_key] = slot
                durable_write(root / 'devices.json', json.dumps(data, sort_keys=True))
            sys.stdout.buffer.write(bundle.read_bytes())
    return 0


def install(bundle, address):
    # Cancellation wins before activation; once the atomic commit starts,
    # finish it and acknowledge completion even if cancellation arrives late.
    completed = False
    def interrupted(unused_signal, unused_frame):
        if completed:
            raise SystemExit(0)
        raise InterruptedError('Pairing cancelled before activation')
    signal.signal(signal.SIGTERM, interrupted)
    address = lan_network.ipv4(address)
    player, payload = lan.read_bundle(Path(bundle))
    # DER avoids giving formatting changes a different authority identifier.
    with tempfile.TemporaryDirectory(prefix='fuse-pair-ca-') as directory:
        cert = Path(directory) / 'cert.pem'
        security.write(cert, payload['security/ca/identity/cert.pem'])
        host_id = hashlib.sha256(security.run('openssl', 'x509', '-in', cert, '-outform', 'DER')).hexdigest()
    root = client_root()
    with lock(root):
        destination = root / host_id
        if destination.is_symlink():
            raise ValueError('Symlinked saved host refused')
        with tempfile.TemporaryDirectory(prefix='.incoming-', dir=root) as directory:
            stage = Path(directory)
            previous = {key: os.environ.get(key) for key in ('FUSE_SECURITY_DIR', 'FUSE_AUTH_DIR', 'FUSE_SERVER_IP')}
            try:
                os.environ.update(FUSE_SECURITY_DIR=str(stage / 'security'), FUSE_AUTH_DIR=str(stage / 'auth'),
                                  FUSE_SERVER_IP=address)
                with redirect_stdout(io.StringIO()):
                    lan.import_bundle(Path(bundle))
                security.write(stage / 'profile.json', json.dumps({'version': 1, 'server': address, 'player': player}))
                # A known authority must retain the same assigned identity and key.
                if destination.exists():
                    old = load_profile(destination)
                    if old['player'] != player:
                        raise ValueError('This host assigned a different identity; existing profile preserved')
                    for name in lan.filenames(player):
                        if lan.private_read(destination / name) != payload[name]:
                            raise ValueError('Conflicting host credentials; existing profile preserved')
                else:
                    os.rename(stage, destination)
                    # TemporaryDirectory can safely clean its now-absent old path.
                # Existing profiles are immutable until activation. In particular,
                # do not replace their DDS XML when an IP-change pairing is cancelled.
                # The native client/check command generates final runtime paths
                # from the committed active record before creating DDS entities.
                for path in destination.rglob('*'):
                    if path.is_file():
                        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
                        try:
                            os.fsync(fd)
                        finally:
                            os.close(fd)
                for path in sorted((p for p in destination.rglob('*') if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
                    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                    try:
                        os.fsync(fd)
                    finally:
                        os.close(fd)
                old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM})
                try:
                    durable_write(root / 'active', json.dumps({'version': 1, 'host_id': host_id,
                                                              'server': address, 'player': player}))
                    completed = True
                    print('PAIRING-COMMITTED', flush=True)
                finally:
                    signal.pthread_sigmask(signal.SIG_SETMASK, old_mask)
            finally:
                for key, value in previous.items():
                    if value is None:
                        os.environ.pop(key, None)
                    else:
                        os.environ[key] = value


def load_profile(directory):
    data = json.loads(lan.private_read(directory / 'profile.json'))
    if data.get('version') != 1 or type(data.get('player')) is not int or data['player'] not in range(1, 6):
        raise ValueError('Invalid saved profile')
    data['server'] = lan_network.ipv4(data['server'])
    return data


def activate(emit=True):
    root = client_root()
    active = lan.private_read(root / 'active').decode('ascii').strip()
    if re.fullmatch('[0-9a-f]{64}', active):
        host_id, saved = active, None  # Early/manual profile compatibility.
    else:
        saved = json.loads(active)
        if saved.get('version') != 1:
            raise ValueError('Invalid active profile version')
        host_id = saved['host_id']
    if not re.fullmatch('[0-9a-f]{64}', host_id):
        raise ValueError('Invalid active profile')
    directory = private_dir(root / host_id)
    profile = load_profile(directory)
    if saved is not None and (type(saved.get('player')) is not int or saved['player'] != profile['player']):
        raise ValueError('Active identity does not match the cached credentials')
    server = lan_network.ipv4(os.environ.get('FUSE_SERVER_IP') or (saved or profile)['server'])
    identity = f"client-{profile['player']}"
    env = {'FUSE_SERVER_IP': server, 'FUSE_DDS_IDENTITY': identity, 'FUSE_USERNAME': identity,
           'FUSE_SECURITY_DIR': str(directory / 'security'), 'FUSE_AUTH_DIR': str(directory / 'auth')}
    os.environ.update(env)
    with redirect_stdout(io.StringIO()):
        config = security.make_config(directory / 'security', identity)
    env.update(CYCLONEDDS_URI=str(config), FUSE_PASSWORD_FILE=str(directory / 'auth' / f'{identity}.pass'))
    os.environ.update(env)
    if emit:
        for key, value in env.items():
            print(f'{key}={value}')
    return server, profile['player']


def bind_address():
    name = lan_network.select_interface(None, os.environ.get('FUSE_DDS_INTERFACE'))
    if not name:
        raise ValueError('Cannot determine a LAN interface; select --interface NAME')
    addresses = lan_network.interfaces()[name]
    print(next(address for address in addresses if not address.startswith('169.254.')))


def main():
    os.umask(0o077)
    command = sys.argv[1]
    if command == 'device':
        device()
    elif command == 'issue':
        return issue(sys.argv[2])
    elif command == 'install':
        install(sys.argv[2], sys.argv[3])
    elif command in ('activate', 'activate-saved'):
        if command == 'activate-saved':
            os.environ.pop('FUSE_SERVER_IP', None)
        activate()
    elif command == 'bind':
        bind_address()
    elif command in ('devices', 'count'):
        root = host_root()
        with lock(root):
            data = registry(root)
        if command == 'count':
            print(len(data))
        else:
            for key, slot in sorted(data.items(), key=lambda item: item[1]):
                print(f'Player identity {slot}: device {hashlib.sha256(bytes.fromhex(key)).hexdigest()[:12]}')
            print(f'{len(data)}/5 identities assigned.')
    else:
        raise ValueError('Unknown storage operation')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, AttributeError, RuntimeError, StopIteration) as error:
        sys.exit(f'Pairing storage: {error}')
