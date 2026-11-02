#!/usr/bin/env python3
"""Private credential transfer and secure, direct LAN connectivity diagnostics."""
import argparse
from contextlib import redirect_stdout
from datetime import datetime, timezone
import hashlib
import gzip
import hmac
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET

import lan_network
import security

REPO = Path(__file__).resolve().parents[2]
MAX_FILE = 128 * 1024


def roots():
    result = [Path(os.environ.get(name, str(Path.home() / ".local/share/fuse" / leaf)))
              for name, leaf in (("FUSE_SECURITY_DIR", "security"), ("FUSE_AUTH_DIR", "auth"))]
    for path in result:
        if not path.is_absolute() or str(path.resolve()).startswith("/mnt/"):
            raise ValueError("Credential directories must be absolute Linux paths outside /mnt")
        # Reject redirected destinations, including symlinked parent directories.
        if any(parent.is_symlink() for parent in (path, *path.parents)):
            raise ValueError(f"Symlinked credential directory: {path}")
    return result


def private_read(path):
    metadata = path.lstat()
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
        raise ValueError(f"Expected an owner-only regular file (chmod 600): {path}")
    if metadata.st_size > MAX_FILE:
        raise ValueError(f"Credential file is too large: {path}")
    return path.read_bytes()


def filenames(player):
    identity = f"client-{player}"
    return ("security/ca/identity/cert.pem", "security/ca/permissions/cert.pem",
            "security/governance.p7s",
            *(f"security/identities/{identity}/{name}" for name in
              ("cert.pem", "key.pem", "permissions.p7s")), f"auth/{identity}.pass")


def validate_payload(payload, player):
    """Validate everything in a private staging tree before changing destinations."""
    identity = f"client-{player}"
    password = payload[f"auth/{identity}.pass"].decode("utf-8").removesuffix("\n")
    if not 1 <= len(password.encode("utf-8")) <= 256 or any(c in password for c in "\r\n\0"):
        raise ValueError("Invalid client password file")
    with tempfile.TemporaryDirectory(prefix="fuse-lan-validate-") as directory:
        stage = Path(directory)
        for name, contents in payload.items():
            security.write(stage / name, contents)
        root = stage / "security"
        own = root / "identities" / identity
        security.check_pair(own / "cert.pem", own / "key.pem", root / "ca/identity/cert.pem")
        for ca in ("identity", "permissions"):
            cert = root / "ca" / ca / "cert.pem"
            security.run("openssl", "verify", "-CAfile", cert, cert)
        governance = ET.fromstring(security.verify_policy(root, root / "governance.p7s").strip())
        if governance.find("domain_access_rules/domain_rule/domains/id").text != "0":
            raise ValueError("Governance does not cover domain 0")
        policy = ET.fromstring(security.verify_policy(root, own / "permissions.p7s").strip())
        grant = policy.find("permissions/grant")
        subject = security.run("openssl", "x509", "-in", own / "cert.pem", "-noout", "-subject",
                               "-nameopt", "RFC2253").decode().strip().removeprefix("subject=")
        if grant is None or grant.findtext("subject_name") != subject or grant.get("name") != identity:
            raise ValueError("Permissions do not match the client identity")
        now = datetime.now(timezone.utc)
        start = datetime.fromisoformat(grant.findtext("validity/not_before").replace("Z", "+00:00"))
        end = datetime.fromisoformat(grant.findtext("validity/not_after").replace("Z", "+00:00"))
        if not start <= now < end:
            raise ValueError("Client permissions are not currently valid")


def export_bundle(player, output):
    root, auth = roots()
    output = output.expanduser().resolve()
    if output.is_relative_to(REPO):
        raise ValueError("Save credential bundles outside the source checkout")
    payload = {name: private_read((root if name.startswith("security/") else auth) /
                                 name.split("/", 1)[1]) for name in filenames(player)}
    validate_payload(payload, player)
    # Catch independently provisioned account/password files before distributing them.
    database = private_read(Path(os.environ.get("FUSE_AUTH_USERS", str(auth / "users.db"))))
    name = f"client-{player}"
    password = payload[f"auth/{name}.pass"].decode("utf-8").removesuffix("\n")
    derived = hashlib.pbkdf2_hmac("sha256", password.encode(), name.encode(), 600_000).hex()
    entries = dict(line.split(":", 1) for line in database.decode("ascii").splitlines()
                   if line and not line.startswith("#"))
    if not hmac.compare_digest(entries.get(name, "").lower(), derived):
        raise ValueError("Client password does not match the host account database")
    payload["manifest.json"] = json.dumps({"version": 1, "player": player}).encode()
    # Exclusive creation prevents overwriting a previous bundle or following a link.
    fd = os.open(output, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream, tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for name, data in payload.items():
                member = tarfile.TarInfo(name)
                member.size, member.mode = len(data), 0o600
                archive.addfile(member, io.BytesIO(data))
    except Exception:
        output.unlink(missing_ok=True)
        raise
    print(f"Exported client-{player}: {output}\nTransfer privately to that player, then delete the transfer copy.")


def read_bundle(bundle):
    if bundle.stat().st_size > 2 * 1024 * 1024:
        raise ValueError("Bundle is too large")
    payload = {}
    # Bound decompression before tarfile processes extended headers. A small
    # network response must not expand into an unbounded credential archive.
    with gzip.open(bundle, 'rb') as compressed:
        expanded = compressed.read(2 * 1024 * 1024 + 1)
    if len(expanded) > 2 * 1024 * 1024:
        raise ValueError('Expanded credential bundle is too large')
    # Never extract archive paths. Accept only the exact allowlist below.
    with tarfile.open(fileobj=io.BytesIO(expanded), mode="r:") as archive:
        for member in archive:
            if len(payload) >= 8 or not member.isfile() or member.name in payload or not 0 <= member.size <= MAX_FILE:
                raise ValueError("Bundle has duplicate, oversized or non-regular entries")
            with archive.extractfile(member) as source:
                payload[member.name] = source.read(MAX_FILE + 1)
    manifest = json.loads(payload.pop("manifest.json"))
    player = manifest.get("player")
    if manifest.get("version") != 1 or type(player) is not int or player not in range(1, 6):
        raise ValueError("Unsupported bundle manifest")
    if set(payload) != set(filenames(player)):
        raise ValueError("Bundle contains missing or unexpected credential files")
    validate_payload(payload, player)
    return player, payload


def import_bundle(bundle):
    player, payload = read_bundle(bundle)
    root, auth = roots()
    destinations = {(root if name.startswith("security/") else auth) / name.split("/", 1)[1]: data
                    for name, data in payload.items()}
    # Preflight the entire import: a different CA/password must not partially replace a setup.
    for path, data in destinations.items():
        for parent in (path.parent, *path.parent.parents):
            if parent.is_symlink():
                raise ValueError(f"Symlinked destination: {parent}")
        if path.exists() or path.is_symlink():
            if private_read(path) != data:
                raise ValueError(f"Conflicting existing credential: {path}. Use fresh FUSE_SECURITY_DIR/FUSE_AUTH_DIR directories.")
    if (root / "runtime").is_symlink():
        raise ValueError(f"Symlinked destination: {root / 'runtime'}")
    for base in (root, auth):
        base.mkdir(parents=True, exist_ok=True, mode=0o700)
        if base.stat().st_uid != os.getuid():
            raise ValueError(f"Credential directory must belong to you: {base}")
        base.chmod(0o700)
    for path, data in destinations.items():
        base = root if path.is_relative_to(root) else auth
        directory = base
        for component in path.relative_to(base).parts[:-1]:
            directory /= component
            directory.mkdir(exist_ok=True, mode=0o700)
            if directory.stat().st_uid != os.getuid():
                raise ValueError(f"Credential directory must belong to you: {directory}")
            directory.chmod(0o700)
        if not path.exists():
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            with os.fdopen(fd, "wb") as stream:
                stream.write(data)
    with redirect_stdout(io.StringIO()):
        security.make_config(root, f"client-{player}")
    print(f"Imported client-{player}. Launch with --player {player}; no CA private keys are needed.")


def network_options(player=None):
    server = os.environ.get("FUSE_SERVER_IP") if player else None
    server = lan_network.ipv4(server) if server else None
    interface = lan_network.select_interface(server, os.environ.get("FUSE_DDS_INTERFACE"))
    print(f"Network: {server or 'automatic discovery'}; interface: {interface or 'automatic'}; "
          "UDP 7400-7401,7410-7429", flush=True)
    return server, interface


def host():
    _, selected = network_options()
    mode = lan_network.networking_mode()
    print(f"WSL/network mode: {mode}")
    for name, addresses in lan_network.interfaces().items():
        if name == "lo":
            continue
        if selected and name != selected:
            continue
        for address in addresses:
            if not address.startswith(("127.", "169.254.")):
                print(f"Host address ({name}): {address}\n"
                      f"  Windows: .\\run-client.bat --player 2 --server {address}\n"
                      f"  Linux: bash run-client.sh --player 2 --server {address}")
    if mode == "nat":
        print("WSL uses NAT: the addresses above are internal to WSL and normally unreachable "
              "from other PCs. Run setup_lan.ps1 to inspect before changing networking.")


def check(server, player):
    os.environ["FUSE_SERVER_IP"] = lan_network.ipv4(server)
    _, interface = network_options(player)
    print(f"Route addresses: {', '.join(lan_network.interfaces()[interface])}")
    mode = lan_network.networking_mode()
    print(f"WSL/network mode: {mode}", flush=True)
    root, auth = roots()
    payload = {name: private_read((root if name.startswith("security/") else auth) /
                                 name.split("/", 1)[1]) for name in filenames(player)}
    validate_payload(payload, player)
    print("Credentials: certificate, signed policies and private password file valid", flush=True)
    probe = REPO / "build/dds_security_probe"
    if not probe.is_file():
        raise RuntimeError("Build the diagnostic first: make build/dds_security_probe")
    env = dict(os.environ, FUSE_DDS_IDENTITY=f"client-{player}", FUSE_DDS_SECURITY="on",
               FUSE_AUTH="on", FUSE_USERNAME=f"client-{player}")
    env["FUSE_PASSWORD_FILE"] = str(auth / f"client-{player}.pass")
    with redirect_stdout(io.StringIO()):
        env["CYCLONEDDS_URI"] = str(security.make_config(root, f"client-{player}"))
    result = subprocess.run([str(probe), "play", "12"], cwd=REPO, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    # Print only diagnostic output; the probe never logs credential material.
    print(result.stdout, end="", flush=True)
    if result.returncode == 0 and "leave_confirmed=1" in result.stdout:
        print("PASS: secure DDS discovery, authenticated join, movement, snapshots and Leave")
        return 0
    status = re.search(r"join_status=(\d+)", result.stdout)
    if "Participant creation failed" in result.stdout or result.returncode == 2:
        print("FAIL: local DDS/client startup failed. Check the configuration, credentials and DDS installation above.")
    elif status and status[1] == "2":
        print("FAIL: server reached; login rejected. Import the host's matching player bundle.")
    elif status and status[1] in ("1", "3", "4"):
        reason = {"1": "lobby full", "3": "match in progress", "4": "join rate-limited"}[status[1]]
        print(f"FAIL: server reached; {reason}. Retry in the lobby.")
    elif "discovered=1" in result.stdout:
        print("FAIL: secure DDS endpoints matched; authenticated gameplay did not complete.")
    else:
        print("FAIL: no secure server match. Check host IP, running server, matching CAs and routing. "
              "Then inspect UDP firewall rules and Wi-Fi client isolation; this alone does not prove a firewall block.")
    if mode == "nat":
        print("WSL NAT detected. Inspect setup_lan.ps1; mirrored networking may be needed on Windows PCs.")
    return 1


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    export = commands.add_parser("export", help="export one client's private credential bundle")
    export.add_argument("--player", type=int, choices=range(1, 6), required=True)
    export.add_argument("--output", type=Path, required=True)
    install = commands.add_parser("import", help="validate and install a transferred bundle")
    install.add_argument("bundle", type=Path)
    diagnostic = commands.add_parser("check", help="exercise a running host using secure DDS")
    diagnostic.add_argument("--server")
    diagnostic.add_argument("--player", type=int, choices=range(1, 6))
    commands.add_parser("host", help="show host addresses and launch commands")
    network = commands.add_parser("network", help="validate launcher network options")
    network.add_argument("--player", type=int, choices=range(1, 6))
    args = parser.parse_args()
    if args.command == "export":
        export_bundle(args.player, args.output)
    elif args.command == "import":
        import_bundle(args.bundle)
    elif args.command == "check":
        if args.player is None:
            import pairing_store
            if args.server:
                os.environ['FUSE_SERVER_IP'] = args.server
            saved_server, args.player = pairing_store.activate(emit=False)
            args.server = args.server or saved_server
        if not args.server:
            parser.error('--server is required with an explicit --player')
        return check(args.server, args.player)
    elif args.command == "host":
        host()
    else:
        network_options(args.player)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, ValueError, KeyError, AttributeError, TypeError,
            ET.ParseError, tarfile.TarError, subprocess.TimeoutExpired) as error:
        sys.exit(f"LAN: {error}")
