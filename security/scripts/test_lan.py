#!/usr/bin/env python3
"""LAN transfer/config regressions; --integration runs inside an isolated user/net namespace."""
from contextlib import redirect_stdout
import io
import gzip
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import lan
import lan_network
import security


class LanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="fuse-lan-tests-")
        cls.base = Path(cls.temporary.name)
        cls.source = cls.base / "host-security"
        cls.source.mkdir(mode=0o700)
        with redirect_stdout(io.StringIO()):
            security.make_certs(cls.source, ["server", "client-1", "client-2"], False)
        cls.auth = cls.base / "host-auth"
        subprocess.run([sys.executable, "security/scripts/accounts.py", "demo", "--directory", str(cls.auth)],
                       cwd=lan.REPO, check=True, stdout=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(dir=self.base)
        self.addCleanup(self.work.cleanup)
        self.directory = Path(self.work.name)
        self.environment = patch.dict(os.environ, {"FUSE_SECURITY_DIR": str(self.source),
            "FUSE_AUTH_DIR": str(self.auth), "FUSE_SERVER_IP": "", "FUSE_DDS_INTERFACE": ""})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        for name in ("FUSE_AUTH_USERS", "FUSE_PASSWORD_FILE", "FUSE_USERNAME"):
            os.environ.pop(name, None)

    def export(self):
        output = self.directory / "player.fuse-lan"
        with redirect_stdout(io.StringIO()):
            lan.export_bundle(1, output)
        return output

    def destination(self):
        root, auth = self.directory / "client-security", self.directory / "client-auth"
        os.environ.update(FUSE_SECURITY_DIR=str(root), FUSE_AUTH_DIR=str(auth))
        return root, auth

    def test_bundle_roundtrip_and_idempotence(self):
        bundle = self.export()
        player, payload = lan.read_bundle(bundle)
        self.assertEqual(player, 1)
        self.assertEqual(set(payload), set(lan.filenames(1)))
        self.assertEqual(bundle.stat().st_mode & 0o777, 0o600)
        root, auth = self.destination()
        for _ in range(2):
            with redirect_stdout(io.StringIO()):
                lan.import_bundle(bundle)
        self.assertEqual((auth / "client-1.pass").read_bytes(), payload["auth/client-1.pass"])
        self.assertFalse((auth / "users.db").exists())
        self.assertFalse((root / "ca/identity/key.pem").exists())
        self.assertFalse((root / "identities/server").exists())
        for path in (*root.rglob("*"), *auth.rglob("*")):
            self.assertEqual(path.stat().st_mode & 0o077, 0)
        self.assertIn(str(root), (root / "runtime/client-1.xml").read_text())

    def test_conflict_does_not_partially_install(self):
        bundle = self.export()
        root, auth = self.destination()
        auth.mkdir(mode=0o700)
        security.write(auth / "client-1.pass", "different\n")
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            lan.import_bundle(bundle)
        self.assertFalse(root.exists())
        self.assertEqual((auth / "client-1.pass").read_text(), "different\n")

    def test_wrong_host_password_not_exported(self):
        auth = self.directory / "wrong-auth"
        auth.mkdir(mode=0o700)
        security.write(auth / "client-1.pass", "different\n")
        security.write(auth / "users.db", (self.auth / "users.db").read_bytes())
        os.environ["FUSE_AUTH_DIR"] = str(auth)
        output = self.directory / "bad.fuse-lan"
        with self.assertRaisesRegex(ValueError, "does not match"):
            lan.export_bundle(1, output)
        self.assertFalse(output.exists())

    def test_symlink_destination_rejected(self):
        bundle = self.export()
        root, auth = self.destination()
        root.mkdir(mode=0o700)
        outside = self.directory / "outside"
        outside.mkdir()
        (root / "ca").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "Symlinked"):
            lan.import_bundle(bundle)
        self.assertFalse(auth.exists())
        self.assertEqual(list(outside.iterdir()), [])

    def test_runtime_symlink_rejected_before_install(self):
        bundle = self.export()
        root, auth = self.destination()
        root.mkdir(mode=0o700)
        (root / "runtime").symlink_to(self.directory, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "Symlinked"):
            lan.import_bundle(bundle)
        self.assertFalse(auth.exists())

    def test_encrypted_key_does_not_prompt(self):
        _, payload = lan.read_bundle(self.export())
        key = self.directory / "key.pem"
        security.write(key, payload["security/identities/client-1/key.pem"])
        encrypted = subprocess.check_output(["openssl", "pkey", "-in", str(key), "-aes-256-cbc",
                                             "-passout", "pass:fixture-only"])
        security.write(key, encrypted)
        with self.assertRaises(RuntimeError):
            security.check_pair(self.source / "identities/client-1/cert.pem", key)

    def test_malformed_archives_rejected(self):
        for name, contents, link in (("../escape", b"x", False),
                                      ("manifest.json", b"not json", False),
                                      ("manifest.json", b"", True)):
            output = self.directory / "malformed.fuse-lan"
            with tarfile.open(output, "w:gz") as archive:
                member = tarfile.TarInfo(name)
                member.size = len(contents)
                if link:
                    member.type, member.linkname = tarfile.SYMTYPE, "/tmp/escape"
                archive.addfile(member, io.BytesIO(contents))
            with self.assertRaises((ValueError, KeyError)):
                lan.read_bundle(output)
        self.assertFalse((self.directory.parent / "escape").exists())

    def test_compressed_archive_expansion_is_bounded(self):
        output = self.directory / 'oversized.fuse-lan'
        output.write_bytes(gzip.compress(b'\0' * (2 * 1024 * 1024 + 1)))
        with self.assertRaisesRegex(ValueError, 'Expanded credential bundle'):
            lan.read_bundle(output)

    def test_tampered_policy_rejected(self):
        _, payload = lan.read_bundle(self.export())
        payload["security/identities/client-1/permissions.p7s"] = b"not signed"
        with self.assertRaises(RuntimeError):
            lan.validate_payload(payload, 1)

    def test_export_never_overwrites(self):
        output = self.export()
        before = output.read_bytes()
        with self.assertRaises(FileExistsError):
            lan.export_bundle(1, output)
        self.assertEqual(output.read_bytes(), before)

    def test_direct_config_and_interface_selection(self):
        os.environ["FUSE_SERVER_IP"] = "127.0.0.1"
        with redirect_stdout(io.StringIO()):
            output = security.make_config(self.source, "client-1")
        document = ET.parse(output)
        ns = {"d": "https://cdds.io/config"}
        def value(path):
            return document.find("d:Domain/" + path, ns)
        self.assertEqual(value("d:General/d:AllowMulticast").text, "false")
        self.assertEqual(value("d:General/d:Transport").text, "udp")
        self.assertEqual(value("d:General/d:Interfaces/d:NetworkInterface").get("name"), "lo")
        self.assertEqual(value("d:Discovery/d:Peers/d:Peer").get("Address"), "127.0.0.1")
        self.assertEqual(value("d:Discovery/d:ParticipantIndex").text, "auto")
        self.assertEqual(value("d:Discovery/d:MaxAutoParticipantIndex").text, "9")
        self.assertIsNotNone(value("d:Security/d:Authentication"))

    def test_automatic_config(self):
        domain = ET.Element("Domain")
        lan_network.configure(domain, "server")
        self.assertEqual(domain.findtext("General/AllowMulticast"), "spdp")
        self.assertIsNone(domain.find("Discovery/Peers"))

    def test_invalid_network_options(self):
        for address in ("localhost", "::1", "0.0.0.0", "239.1.1.1", "255.255.255.255", "1.2.3.999"):
            with self.assertRaises(ValueError):
                lan_network.ipv4(address)
        with self.assertRaises(ValueError):
            lan_network.select_interface(None, "missing-fuse-interface")

    def test_launcher_arguments(self):
        for command in (("run-client.sh", "--player", "6"), ("run-client.sh", "--server"),
                        ("run-client.sh", "--player", "1", "--server", "::1", "--no-build"),
                        ("run-server.sh", "--interface"),
                        ("run-server.sh", "--interface", "missing-fuse-interface", "--no-build")):
            result = subprocess.run(["bash", *command], cwd=lan.REPO, capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, command)
        for script in ("run-client.sh", "run-server.sh"):
            result = subprocess.run(["bash", script, "--help"], cwd=lan.REPO, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertIn(b"--interface", result.stdout)


def integration():
    """Parent and child use distinct network stacks and a private veth connection.

    Run: unshare -Urn python3 security/scripts/test_lan.py --integration
    No host interface, firewall or administrator settings are changed.
    """
    if os.getuid() != 0:
        raise RuntimeError("Run this test inside unshare -Urn (private user/network namespaces)")
    # Guard against accidentally using the real root network namespace.
    if any(name != "lo" for name in lan_network.interfaces()):
        raise RuntimeError("Integration requires a fresh network namespace containing only lo")
    child = subprocess.Popen(["unshare", "-n", "sleep", "120"])
    server = None
    def run(*args, **kwargs):
        return subprocess.run(list(args), check=True, text=True, **kwargs)
    try:
        parent_ns = os.readlink("/proc/self/ns/net")
        for _ in range(100):
            if os.readlink(f"/proc/{child.pid}/ns/net") != parent_ns:
                break
            time.sleep(0.01)
        else:
            raise RuntimeError("Child network namespace did not start")
        enter = ["nsenter", f"--net=/proc/{child.pid}/ns/net", "--"]
        run("ip", "link", "add", "fuse-host", "type", "veth", "peer", "name", "fuse-client")
        run("ip", "link", "set", "fuse-client", "netns", str(child.pid))
        run("ip", "address", "add", "10.208.0.1/24", "dev", "fuse-host")
        run("ip", "link", "set", "fuse-host", "up")
        run("ip", "link", "set", "lo", "up")
        run(*enter, "ip", "address", "add", "10.208.0.2/24", "dev", "fuse-client")
        run(*enter, "ip", "link", "set", "fuse-client", "up")
        run(*enter, "ip", "link", "set", "lo", "up")
        with tempfile.TemporaryDirectory(prefix="fuse-lan-integration-") as directory:
            base = Path(directory)
            root, auth = base / "host/security", base / "host/auth"
            root.mkdir(parents=True, mode=0o700)
            with redirect_stdout(io.StringIO()):
                security.make_certs(root, ["server", "client-1", "client-2"], False)
            run(sys.executable, "security/scripts/accounts.py", "demo", "--directory", str(auth),
                stdout=subprocess.DEVNULL)
            env = dict(os.environ, FUSE_SECURITY_DIR=str(root), FUSE_AUTH_DIR=str(auth),
                       FUSE_DDS_MULTICAST="off", FUSE_DDS_INTERFACE="fuse-host", FUSE_AUTH="on")
            for name in ("FUSE_AUTH_USERS", "FUSE_PASSWORD_FILE", "FUSE_USERNAME", "FUSE_SERVER_IP"):
                env.pop(name, None)
            bundle = base / "player.fuse-lan"
            run(sys.executable, "security/scripts/lan.py", "export", "--player", "1", "--output", str(bundle), env=env)
            client_env = dict(env, FUSE_SECURITY_DIR=str(base / "client/security"),
                              FUSE_AUTH_DIR=str(base / "client/auth"), FUSE_DDS_INTERFACE="fuse-client")
            run(*enter, sys.executable, "security/scripts/lan.py", "import", str(bundle), env=client_env)
            second_bundle = base / "player-2.fuse-lan"
            run(sys.executable, "security/scripts/lan.py", "export", "--player", "2", "--output", str(second_bundle), env=env)
            run(*enter, sys.executable, "security/scripts/lan.py", "import", str(second_bundle), env=client_env)
            with (base / "server.log").open("w+") as log:
                server = subprocess.Popen(["bash", "security/scripts/run_secure.sh", "server", "./build/server"],
                                          env=env, stdout=log, stderr=subprocess.STDOUT)
                time.sleep(0.5)
                try:
                    for attempt in range(2):
                        run(*enter, sys.executable, "security/scripts/lan.py", "check", "--server", "10.208.0.1",
                            "--player", "1", env=client_env, timeout=25)
                        print(f"PASS isolated unicast gameplay {'and rejoin' if attempt else ''}", flush=True)
                    password_file = base / "client/auth/client-1.pass"
                    security.write(password_file, "incorrect-password\n")
                    rejected = subprocess.run([*enter, sys.executable, "security/scripts/lan.py", "check",
                        "--server", "10.208.0.1", "--player", "1"], env=client_env,
                        text=True, capture_output=True, timeout=25)
                    assert rejected.returncode == 1 and "login rejected" in rejected.stdout, rejected.stdout
                    print("PASS login rejection distinguished from connectivity failure", flush=True)
                    security.write(password_file, (auth / "client-1.pass").read_bytes())
                    # Two real authenticated clients must observe the same populated match.
                    clients = [subprocess.Popen([*enter, sys.executable, "security/scripts/lan.py", "check",
                        "--server", "10.208.0.1", "--player", str(player)], env=client_env,
                        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) for player in (1, 2)]
                    try:
                        ids = set()
                        for client in clients:
                            output, _ = client.communicate(timeout=25)
                            assert client.returncode == 0 and "max_players=2" in output, output
                            ids.add(re.search(r"joined=1 id=(\d+)", output)[1])
                        assert len(ids) == 2, "Concurrent clients were assigned the same player ID"
                        print("PASS two clients both observe the shared two-player match", flush=True)
                    finally:
                        for client in clients:
                            if client.poll() is None:
                                client.terminate()
                                client.wait(timeout=5)
                except Exception:
                    log.seek(0)
                    print(log.read())
                    raise
    finally:
        if server is not None:
            server.terminate()
            server.wait(timeout=5)
        child.terminate()
        child.wait(timeout=5)


if __name__ == "__main__":
    if "--integration" in sys.argv:
        integration()
    else:
        unittest.main()
