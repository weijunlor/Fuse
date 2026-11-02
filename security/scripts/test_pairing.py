#!/usr/bin/env python3
"""Pair real TLS peers with temporary credentials; never record temporary codes."""
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import threading
import unittest
from unittest.mock import patch

import security

ROOT = Path(__file__).resolve().parents[2]


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Host:
    def __init__(self, env, binary='build/pairing', mode='host', address='127.0.0.1', port=None):
        self.port = port or free_port()
        self.buffer = b''
        code_read, code_write = os.pipe()
        self.codes = os.fdopen(code_read,'rb',buffering=0)
        self.process = subprocess.Popen([binary, mode, address, str(self.port)], cwd=ROOT,
            env=dict(env,FUSE_PAIRING_CODE_FD=str(code_write)), pass_fds=(code_write,),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        os.close(code_write)
    def code(self):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            match = re.search(rb'Pairing code: (FUSE-[A-Za-z0-9_-]{43})', self.buffer)
            if match:
                self.buffer = self.buffer[match.end():]
                return match[1].decode()
            ready, _, _ = select.select([self.codes], [], [], .1)
            if ready:
                data = os.read(self.codes.fileno(), 4096)
                if not data:
                    raise RuntimeError('Pairing host closed its private code channel')
                self.buffer += data
        raise RuntimeError('Pairing host did not display a code')
    def command(self, value):
        self.process.stdin.write((value + '\n').encode()); self.process.stdin.flush()
    def stop(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
        self.process.wait(timeout=10)
        self.process.stdin.close(); self.process.stdout.close(); self.process.stderr.close()
        self.codes.close()


class PairingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = tempfile.TemporaryDirectory(prefix='fuse-pair-tests-')
        cls.source = Path(cls.base.name) / 'source'
        cls.source.mkdir(mode=0o700)
        with redirect_stdout(io.StringIO()):
            security.make_certs(cls.source, security.IDENTITIES, False)
        cls.auth = Path(cls.base.name) / 'auth'
        subprocess.run([sys.executable, 'security/scripts/accounts.py', 'demo', '--directory', str(cls.auth)],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.base.cleanup()

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='case-', dir=self.base.name)
        self.addCleanup(self.work.cleanup)
        self.directory = Path(self.work.name)
        shutil.copytree(self.source, self.directory / 'security')
        self.env = dict(os.environ, FUSE_SECURITY_DIR=str(self.directory / 'security'),
                        FUSE_AUTH_DIR=str(self.auth))
        for key in ('FUSE_SERVER_IP', 'FUSE_DDS_INTERFACE', 'FUSE_AUTH_USERS', 'FUSE_PASSWORD_FILE', 'FUSE_USERNAME'):
            self.env.pop(key, None)
        self.host = Host(self.env)
        self.addCleanup(self.host.stop)

    def client_env(self, name='client'):
        return dict(self.env, FUSE_PAIRING_DIR=str(self.directory / name))

    def pair(self, code, name='client', host=None, expected=0, binary='build/pairing', mode='client'):
        target = host or self.host
        result = subprocess.run([binary, mode, '127.0.0.1', str(target.port)], input=code+'\n',
            cwd=ROOT, env=self.client_env(name), capture_output=True, text=True, timeout=40)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def assignments(self):
        path = self.directory / 'security/pairing/devices.json'
        return json.loads(path.read_text()) if path.exists() else {}

    def test_fresh_restart_and_same_device_retry(self):
        code = self.host.code()
        self.pair(code)
        base = self.directory / 'client'
        host_id = json.loads((base / 'active').read_text())['host_id']
        profile = json.loads((base / host_id / 'profile.json').read_text())
        self.assertEqual(profile['player'], 1)
        self.assertFalse((base / host_id / 'security/ca/identity/key.pem').exists())
        self.assertFalse((base / host_id / 'auth/users.db').exists())
        for path in base.rglob('*'):
            self.assertEqual(path.stat().st_mode & 0o077, 0)
        self.pair(self.host.code())
        self.assertEqual(len(self.assignments()), 1)
        self.host.stop()
        self.host = Host(self.env)
        self.addCleanup(self.host.stop)
        self.pair(self.host.code())
        self.assertEqual(len(self.assignments()), 1)
        activated = subprocess.run([sys.executable, 'security/scripts/pairing_store.py', 'activate'],
            cwd=ROOT, env=self.client_env(), capture_output=True, text=True, timeout=10)
        self.assertEqual(activated.returncode, 0, activated.stderr)
        self.assertIn('FUSE_DDS_IDENTITY=client-1', activated.stdout)
        self.assertIn(str(base), activated.stdout)

    def test_all_five_and_capacity(self):
        for player in range(1, 6):
            self.pair(self.host.code(), name=f'client-{player}')
        self.assertEqual(set(self.assignments().values()), {1,2,3,4,5})
        self.host.command('pair')
        result = self.pair(self.host.code(), name='sixth', expected=1)
        self.assertIn('All five', result.stdout)
        self.assertFalse((self.directory / 'sixth/active').exists())

    def test_wrong_code_does_not_consume_correct_code(self):
        code = self.host.code()
        wrong = code[:5] + ('A' if code[5] != 'A' else 'B') + code[6:]
        self.pair(wrong, expected=1)
        self.assertEqual(self.assignments(), {})
        self.pair(code)

    def test_noncanonical_code_is_rejected(self):
        code=self.host.code()
        alphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
        # Flip an unused Base64 padding bit: lenient decoders produce the same
        # 32-byte PSK, but the displayed one-time code must match exactly.
        wrong=code[:-1]+alphabet[alphabet.index(code[-1])+1]
        self.pair(wrong,expected=1)
        self.assertEqual(self.assignments(),{})
        self.pair(code)

    def test_cancelled_and_used_codes(self):
        cancelled = self.host.code()
        self.host.command('cancel'); time.sleep(.2)
        self.pair(cancelled, expected=1)
        self.host.command('pair'); code = self.host.code()
        self.pair(code)
        self.host.code()  # Wait for the replacement code.
        self.pair(code, name='other', expected=1)
        self.assertEqual(len(self.assignments()), 1)

    def test_concurrent_reuse(self):
        code = self.host.code()
        clients = [subprocess.Popen(['build/pairing', 'client', '127.0.0.1', str(self.host.port)], cwd=ROOT,
            env=self.client_env(f'concurrent-{i}'), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True) for i in range(2)]
        try:
            for client in clients:
                client.stdin.write(code+'\n'); client.stdin.flush(); client.stdin.close(); client.stdin = None
            for client in clients:
                client.communicate(timeout=40)
            self.assertEqual(sorted(client.returncode for client in clients), [0,1])
            self.assertEqual(len(self.assignments()), 1)
        finally:
            for client in clients:
                if client.poll() is None: client.terminate(); client.wait(timeout=10)

    def test_device_proof_is_required(self):
        code = self.host.code()
        self.pair(code, binary='build/test_pairing', mode='bad-proof')
        self.assertEqual(self.assignments(), {})
        self.pair(code)

    def test_device_signature_cannot_cross_tls_connections(self):
        code = self.host.code()
        self.pair(code, binary='build/test_pairing', mode='replay-proof')
        self.assertEqual(self.assignments(), {})
        self.pair(code)

    def test_interrupted_delivery_keeps_the_device_assignment(self):
        code = self.host.code()
        env = self.client_env()
        process = subprocess.Popen(['build/pairing','client','127.0.0.1',str(self.host.port)], cwd=ROOT,
            env=env, stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
        process.stdin.write((code+'\n').encode()); process.stdin.flush()
        try:
            deadline = time.monotonic()+10
            while not self.assignments() and time.monotonic()<deadline:
                time.sleep(.001)
            self.assertEqual(len(self.assignments()),1)
            if process.poll() is None: os.killpg(process.pid,signal.SIGTERM)
            process.communicate(timeout=5)
            self.pair(self.host.code())
            self.assertEqual(len(self.assignments()),1)
        finally:
            if process.poll() is None: os.killpg(process.pid,signal.SIGTERM); process.wait(timeout=5)

    def test_cancel_before_and_after_atomic_activation(self):
        import lan
        import pairing_store
        bundle=self.directory/'fixture.fuse-lan'
        original_handler=signal.getsignal(signal.SIGTERM)
        try:
            with patch.dict(os.environ,self.client_env('commit-boundary')):
                with redirect_stdout(io.StringIO()):
                    lan.export_bundle(1,bundle)
                    pairing_store.install(bundle,'127.0.0.1')
                    pairing_store.activate(emit=False)
                base=self.directory/'commit-boundary'
                previous=(base/'active').read_bytes()
                host_id=json.loads(previous)['host_id']
                runtime=base/host_id/'security/runtime/client-1.xml'
                previous_runtime=runtime.read_bytes()
                write=pairing_store.durable_write
                change_mask=signal.pthread_sigmask
                def before_commit(how,mask):
                    if how==signal.SIG_BLOCK and signal.SIGTERM in mask:
                        signal.raise_signal(signal.SIGTERM)
                    return change_mask(how,mask)
                with patch.object(pairing_store.signal,'pthread_sigmask',side_effect=before_commit):
                    with self.assertRaises(InterruptedError):
                        pairing_store.install(bundle,'127.0.0.2')
                self.assertEqual((base/'active').read_bytes(),previous)
                self.assertEqual(runtime.read_bytes(),previous_runtime)
                def after_commit(path,data):
                    write(path,data)
                    if path.name=='active': signal.raise_signal(signal.SIGTERM)
                with patch.object(pairing_store,'durable_write',side_effect=after_commit):
                    with redirect_stdout(io.StringIO()):
                        with self.assertRaises(SystemExit) as stopped:
                            pairing_store.install(bundle,'127.0.0.2')
                    self.assertEqual(stopped.exception.code,0)
                self.assertEqual(json.loads((base/'active').read_text())['server'],'127.0.0.2')
        finally:
            signal.signal(signal.SIGTERM,original_handler)

    def test_four_connection_limit(self):
        code = self.host.code()
        sockets = [socket.create_connection(('127.0.0.1',self.host.port)) for _ in range(4)]
        try:
            time.sleep(.2)
            self.pair(code,expected=1)
            self.assertEqual(self.assignments(),{})
        finally:
            for connection in sockets: connection.close()
        time.sleep(.2)
        self.pair(code)

    def test_bad_bundle_and_oversize_preserve_profile(self):
        self.pair(self.host.code())
        active = (self.directory / 'client/active').read_bytes()
        for mode in ('invalid-bundle', 'oversize'):
            adversary = Host(self.env, binary='build/test_pairing', mode=mode)
            try:
                self.pair(adversary.code(), host=adversary, expected=1)
                self.assertEqual((self.directory / 'client/active').read_bytes(), active)
            finally:
                adversary.stop()

    def test_shutdown_during_transfer(self):
        adversary = Host(self.env, binary='build/test_pairing', mode='stall')
        try:
            code = adversary.code()
            process = subprocess.Popen(['build/pairing','client','127.0.0.1',str(adversary.port)], cwd=ROOT,
                env=self.client_env(), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                start_new_session=True, text=True)
            process.stdin.write(code+'\n'); process.stdin.flush()
            time.sleep(.5)
            os.killpg(process.pid, signal.SIGTERM)
            process.communicate(timeout=5)
            self.assertFalse((self.directory / 'client/active').exists())
        finally:
            adversary.stop()

    def test_certificate_fallback_is_rejected(self):
        port = free_port()
        server = subprocess.Popen(['openssl','s_server','-accept',f'127.0.0.1:{port}', '-quiet', '-ign_eof',
            '-tls1_3','-cert',str(self.directory/'security/identities/server/cert.pem'),
            '-key',str(self.directory/'security/identities/server/key.pem')], stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            time.sleep(.2)
            target = type('Target', (), {'port': port})()
            self.pair(self.host.code(), host=target, expected=1)
            self.assertEqual(self.assignments(), {})
        finally:
            server.terminate(); server.wait(timeout=5)

    def test_handshake_rate_limit(self):
        code = self.host.code()
        wrong = code[:5] + ('A' if code[5] != 'A' else 'B') + code[6:]
        for _ in range(5): self.pair(wrong, expected=1)
        self.pair(code, expected=1)
        self.assertEqual(self.assignments(), {})

    def test_temporary_code_is_not_in_game_logs(self):
        code=self.host.code()
        ready,_,_=select.select([self.host.process.stdout],[],[],2)
        self.assertTrue(ready)
        output=os.read(self.host.process.stdout.fileno(),4096)
        self.assertNotIn(code.encode(),output)
        self.assertNotRegex(output,rb'FUSE-[A-Za-z0-9_-]{43}')

    @unittest.skipUnless(os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'), 'Requires GLFW display')
    def test_pairing_ui_and_saved_startup(self):
        target = Host(self.env,port=7440)
        try:
            code=target.code()
            result=subprocess.run(['build/test_pairing_ui','127.0.0.1','build/pairing-validation/pairing-screen.png'],
                cwd=ROOT,env=self.client_env('ui'),input=code+'\n',capture_output=True,text=True,timeout=40)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            restarted=subprocess.run(['build/test_pairing_ui','reload'],cwd=ROOT,env=self.client_env('ui'),
                capture_output=True,text=True,timeout=15)
            self.assertEqual(restarted.returncode,0,restarted.stdout+restarted.stderr)
        finally:
            target.stop()

    @unittest.skipUnless(os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'), 'Requires GLFW display')
    def test_pairing_ui_cancel(self):
        target=Host(self.env,binary='build/test_pairing',mode='stall',port=7440)
        try:
            code=target.code()
            result=subprocess.run(['build/test_pairing_ui','127.0.0.1','build/pairing-validation/pairing-cancel.png','cancel'],
                cwd=ROOT,env=self.client_env('ui-cancel'),input=code+'\n',capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertFalse((self.directory/'ui-cancel/active').exists())
        finally:
            target.stop()


def integration():
    """Private veth LAN: pair two PCs, then use only their saved profiles to play."""
    import lan_network
    if os.getuid() != 0 or any(name != 'lo' for name in lan_network.interfaces()):
        raise RuntimeError('Run inside a fresh user/network namespace: unshare -Urn python3 security/scripts/test_pairing.py --integration')
    child = subprocess.Popen(['unshare','-n','sleep','180'])
    game = host = capture = None
    stop_capture = threading.Event()
    def run(*args, **kwargs):
        return subprocess.run(list(args),check=True,text=True,cwd=ROOT,**kwargs)
    try:
        parent = os.readlink('/proc/self/ns/net')
        for _ in range(100):
            if os.readlink(f'/proc/{child.pid}/ns/net') != parent: break
            time.sleep(.01)
        else: raise RuntimeError('Child namespace did not start')
        enter = ['nsenter',f'--net=/proc/{child.pid}/ns/net','--']
        run('ip','link','add','fuse-host','type','veth','peer','name','fuse-client')
        run('ip','link','set','fuse-client','netns',str(child.pid))
        run('ip','address','add','10.208.0.1/24','dev','fuse-host')
        run('ip','link','set','fuse-host','up'); run('ip','link','set','lo','up')
        run(*enter,'ip','address','add','10.208.0.2/24','dev','fuse-client')
        run(*enter,'ip','link','set','fuse-client','up'); run(*enter,'ip','link','set','lo','up')
        PairingTests.setUpClass()
        source, auth = PairingTests.source, PairingTests.auth
        env = dict(os.environ,FUSE_SECURITY_DIR=str(source),FUSE_AUTH_DIR=str(auth),
                   FUSE_DDS_INTERFACE='fuse-host',FUSE_DDS_MULTICAST='off')
        for key in ('FUSE_SERVER_IP','FUSE_DDS_IDENTITY','FUSE_USERNAME','FUSE_PASSWORD_FILE','FUSE_AUTH_USERS'):
            env.pop(key,None)
        out = ROOT/'build/pairing-validation'; out.mkdir(exist_ok=True)
        capture = socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(3))
        capture.bind(('fuse-host',0)); capture.settimeout(.1)
        pcap = out/'enrollment.pcap'
        def record_packets():
            with pcap.open('wb') as stream:
                stream.write(struct.pack('<IHHIIII',0xa1b2c3d4,2,4,0,0,65535,1))
                while not stop_capture.is_set():
                    try: packet = capture.recv(65535)
                    except socket.timeout: continue
                    if len(packet)<54 or packet[12:14]!=b'\x08\x00' or packet[23]!=6: continue
                    tcp = 14+(packet[14]&15)*4
                    if len(packet)<tcp+4 or 7440 not in struct.unpack('!HH',packet[tcp:tcp+4]): continue
                    current=time.time(); seconds=int(current)
                    stream.write(struct.pack('<IIII',seconds,int((current-seconds)*1e6),len(packet),len(packet)))
                    stream.write(packet)
        recorder = threading.Thread(target=record_packets); recorder.start()
        host = Host(env,address='10.208.0.1',port=7440)
        clients, codes = [], []
        for index in (1,2):
            client_env = dict(env,FUSE_PAIRING_DIR=str(Path(PairingTests.base.name)/f'pc-{index}'))
            client_env.pop('FUSE_DDS_INTERFACE')  # Exercise automatic remote route selection.
            code=host.code(); codes.append(code)
            paired=run(*enter,'./build/pairing','client','10.208.0.1',env=client_env,input=code+'\n',capture_output=True,timeout=40)
            assert 'connection is saved' in paired.stdout
            clients.append(client_env)
        stop_capture.set(); recorder.join(timeout=2); capture.close(); capture=None
        packets=pcap.read_bytes()
        assert all(code.encode() not in packets for code in codes)
        assert all((auth/f'client-{i}.pass').read_bytes().strip() not in packets for i in (1,2))
        decoded=run('tshark','-r',str(pcap),'-d','tcp.port==7440,tls','-Y','tls.app_data',
                    '-T','fields','-e','frame.number',capture_output=True)
        assert decoded.stdout.strip(), 'Capture must contain encrypted TLS application records'
        print('PASS two PCs enrolled over TLS; captures contain encrypted records and no pairing codes/passwords',flush=True)
        host.stop(); host=Host(env,address='10.208.0.1',port=7440)
        # A service restart retains the public device-key assignments.
        fresh=host.code()
        run(*enter,'./build/pairing','client','10.208.0.1',env=clients[0],input=fresh+'\n',capture_output=True,timeout=40)
        assert len(json.loads((source/'pairing/devices.json').read_text()))==2
        # A client on the host computer uses the same advertised LAN address.
        # Its kernel route points at lo, but DDS must select the owning adapter.
        local_env = dict(env,FUSE_PAIRING_DIR=str(Path(PairingTests.base.name)/'pc-local'))
        local_env.pop('FUSE_DDS_INTERFACE')
        run('./build/pairing','client','10.208.0.1',env=local_env,input=host.code()+'\n',capture_output=True,timeout=40)
        with (out/'game-server.log').open('w') as log:
            game=subprocess.Popen(['bash','security/scripts/run_secure.sh','server','./build/server'],cwd=ROOT,
                                  env=env,stdout=log,stderr=subprocess.STDOUT)
            checks=[subprocess.Popen([*enter,sys.executable,'security/scripts/lan.py','check'],cwd=ROOT,
                    env=item,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True) for item in clients]
            checks.append(subprocess.Popen([sys.executable,'security/scripts/lan.py','check'],cwd=ROOT,
                    env=local_env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True))
            try:
                ids=set()
                for index,process in enumerate(checks):
                    output,_=process.communicate(timeout=25)
                    (out/f'paired-player-{index+1}.log').write_text(output)
                    assert process.returncode==0 and 'max_players=3' in output,output
                    if index == 2:
                        assert 'interface: fuse-host;' in output, output
                    ids.add(re.search(r'joined=1 id=(\d+)',output)[1])
                assert len(ids)==3
            finally:
                for process in checks:
                    if process.poll() is None: process.terminate();process.wait(timeout=5)
        print('PASS two remote clients and a same-computer client automatically select their LAN adapters and join using saved profiles',flush=True)
        print('PASS distinct IDs, movement, shared snapshots and Leave, without IP/code/player flags',flush=True)
    finally:
        stop_capture.set()
        if 'recorder' in locals(): recorder.join(timeout=2)
        if capture is not None: capture.close()
        if host is not None: host.stop()
        if game is not None: game.terminate();game.wait(timeout=5)
        child.terminate();child.wait(timeout=5)
        if hasattr(PairingTests,'base'): PairingTests.tearDownClass()


if __name__ == '__main__':
    os.chdir(ROOT)
    if '--integration' in sys.argv: integration()
    else: unittest.main()
