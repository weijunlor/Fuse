#!/usr/bin/env python3
"""Keep simultaneous game windows on distinct, persistent pairing profiles.

The first window retains the existing PC profile. Additional windows have their
own device keys and pair once, so they never log in using the first player's
credentials. Kernel locks reserve windows only while their game process lives;
releasing a window does not recycle its durable host identity assignment.
"""
import fcntl
import json
import os
import re
import signal
import stat
import subprocess
import sys

import lan
import lan_network
import pairing_store


def reserve(root):
    for slot in range(1, 6):
        fd = os.open(root / f'window-{slot}.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        try:
            info = os.fstat(fd)
            if info.st_uid != os.getuid() or info.st_mode & 0o077 or not stat.S_ISREG(info.st_mode):
                raise ValueError('Private window lock required')
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                os.close(fd)
                continue
            profile = root
            if slot != 1:
                profile = pairing_store.private_dir(pairing_store.private_dir(root / 'players') / str(slot))
            return fd, profile
        except BaseException:
            os.close(fd)
            raise
    raise ValueError('Five client windows are already open. Close a window before starting another.')


def saved_host(root):
    """Prefill additional players without copying any credentials/device key."""
    try:
        active = lan.private_read(root / 'active').decode('ascii').strip()
        if re.fullmatch('[0-9a-f]{64}', active):
            active = pairing_store.load_profile(root / active)
        else:
            active = json.loads(active)
        return lan_network.ipv4(active['server'])
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return None


def run(command):
    root = pairing_store.client_root()
    fd, profile = reserve(root)
    child = None
    pending_signal = None
    def forward(number, unused_frame):
        nonlocal pending_signal
        pending_signal = number
        if child is not None and child.poll() is None:
            try:
                child.send_signal(number)
            except ProcessLookupError:
                pass
    old_handlers = {number: signal.signal(number, forward) for number in (signal.SIGINT, signal.SIGTERM)}
    try:
        env = dict(os.environ, FUSE_PAIRING_DIR=str(profile))
        if profile != root and not (profile / 'active').exists() and not env.get('FUSE_SERVER_IP'):
            host = saved_host(root)
            if host:
                env['FUSE_SERVER_IP'] = host
        # The supervisor alone owns the lease; enrollment helpers cannot inherit
        # it and keep a closed game window's profile unnecessarily reserved.
        child = subprocess.Popen(command, env=env, close_fds=True, start_new_session=True)
        if pending_signal is not None:
            forward(pending_signal, None)
        result = child.wait()
        return result if result >= 0 else 128 - result
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        os.close(fd)
        for number, handler in old_handlers.items():
            signal.signal(number, handler)


if __name__ == '__main__':
    try:
        if len(sys.argv) < 2:
            raise ValueError('A client executable is required')
        sys.exit(run(sys.argv[1:]))
    except (OSError, ValueError, RuntimeError) as error:
        sys.exit(f'Client startup: {error}')
