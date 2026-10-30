#!/usr/bin/env python3
"""Generate local DDS credentials and signed policies; never overwrite keys.

OpenSSL owns all cryptographic operations. Python only handles paths, XML,
validity dates and atomic output, keeping shell quoting out of the policy data.
"""
import argparse
from datetime import datetime, timedelta, timezone
import os
from pathlib import Path
import secrets
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

CONFIG = Path(__file__).resolve().parents[1] / "config"
IDENTITIES = ("server", *(f"client-{i}" for i in range(1, 6)))


def run(*args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(f"{args[0]} {args[1]} failed: {result.stderr.decode().strip()}")
    return result.stdout


def root_dir():
    root = Path(os.environ.get("FUSE_SECURITY_DIR", str(Path.home() / ".local/share/fuse/security")))
    if not root.is_absolute() or str(root.resolve()).startswith("/mnt/"):
        raise RuntimeError("FUSE_SECURITY_DIR must be an absolute Linux filesystem path, outside /mnt")
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    if root.stat().st_uid != os.getuid():
        raise RuntimeError("Credential directory must belong to the current user")
    root.chmod(0o700)
    return root


def write(path, data):
    """An interrupted generation must not leave a half-written policy/config."""
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        output.write(data.encode() if isinstance(data, str) else data)
        temporary = Path(output.name)
    os.replace(temporary, path)


def check_pair(cert, key, ca=None):
    run("openssl", "x509", "-checkend", "0", "-noout", "-in", cert)
    public_key = run("openssl", "pkey", "-in", key, "-pubout")
    if public_key != run("openssl", "x509", "-in", cert, "-pubkey", "-noout"):
        raise RuntimeError(f"Certificate and private key do not match: {cert}")
    run("openssl", "verify", "-CAfile", ca or cert, cert)
    if key.stat().st_mode & 0o077:
        raise RuntimeError(f"Private key must have owner-only permissions: {key}")


def make_ca(root):
    for name in ("identity", "permissions"):
        directory = root / "ca" / name
        cert, key = directory / "cert.pem", directory / "key.pem"
        if cert.exists() or key.exists():
            check_pair(cert, key)
            if name == "permissions":
                run("openssl", "verify", "-purpose", "smimesign", "-CAfile", cert, cert)
            continue
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            stage = Path(temporary)
            run("openssl", "req", "-new", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-sha256", "-days", "3650", "-subj", f"/O=FUSE/CN=FUSE {name} CA",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical," + ("digitalSignature," if name == "permissions" else "") + "keyCertSign,cRLSign",
                "-keyout", stage / "key.pem", "-out", stage / "cert.pem")
            write(cert, (stage / "cert.pem").read_bytes())
            write(key, (stage / "key.pem").read_bytes())
        check_pair(cert, key)


def sign(root, xml, signed):
    run("openssl", "smime", "-sign", "-md", "sha256", "-text", "-in", xml,
        "-signer", root / "ca/permissions/cert.pem", "-inkey", root / "ca/permissions/key.pem",
        "-out", signed)
    signed.chmod(0o600)


def verify_policy(root, signed):
    return run("openssl", "smime", "-verify", "-text", "-in", signed,
               "-CAfile", root / "ca/permissions/cert.pem")


def make_certs(root, identities, refresh):
    make_ca(root)
    governance = root / "governance.xml"
    governance_signed = root / "governance.p7s"
    contents = (CONFIG / "governance.xml").read_bytes()
    if not governance.exists() or governance.read_bytes() != contents or refresh:
        write(governance, contents)
        sign(root, governance, governance_signed)
    verify_policy(root, governance_signed)
    for identity in identities:
        if identity not in IDENTITIES:
            raise RuntimeError(f"Unknown identity: {identity}")
        directory = root / "identities" / identity
        cert, key = directory / "cert.pem", directory / "key.pem"
        if not cert.exists() and not key.exists():
            with tempfile.TemporaryDirectory(dir=root) as temporary:
                stage = Path(temporary)
                run("openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-sha256",
                    "-subj", f"/O=FUSE/CN={identity}", "-keyout", stage / "key.pem",
                    "-out", stage / "request.pem")
                write(stage / "extensions", "basicConstraints=critical,CA:FALSE\n"
                      "keyUsage=critical,digitalSignature,keyEncipherment\n")
                run("openssl", "x509", "-req", "-in", stage / "request.pem", "-sha256",
                    "-CA", root / "ca/identity/cert.pem", "-CAkey", root / "ca/identity/key.pem",
                    "-set_serial", str(secrets.randbits(128) or 1), "-days", "365",
                    "-extfile", stage / "extensions", "-out", stage / "cert.pem")
                write(cert, (stage / "cert.pem").read_bytes())
                write(key, (stage / "key.pem").read_bytes())
        check_pair(cert, key, root / "ca/identity/cert.pem")
        permissions = directory / "permissions.xml"
        signed = directory / "permissions.p7s"
        subject = run("openssl", "x509", "-in", cert, "-noout", "-subject",
                      "-nameopt", "RFC2253").decode().strip().removeprefix("subject=")
        role = "server" if identity == "server" else "client"
        document = ET.parse(CONFIG / f"permissions_{role}.xml")
        grant = document.getroot().find("permissions/grant")
        grant.set("name", identity)
        grant.find("subject_name").text = subject
        now = datetime.now(timezone.utc)
        if permissions.exists() and not refresh:
            # Preserve the existing validity period and signed file on repeated setup.
            existing = ET.fromstring(verify_policy(root, signed).strip())
            old_grant = existing.find("permissions/grant")
            if old_grant.findtext("subject_name") != subject:
                raise RuntimeError(f"Permission subject mismatch: {signed}")
            for field in ("not_before", "not_after"):
                value = old_grant.findtext(f"validity/{field}")
                grant.find(f"validity/{field}").text = value
            end = datetime.fromisoformat(grant.findtext("validity/not_after").replace("Z", "+00:00"))
            if end <= now:
                raise RuntimeError("Permissions expired; rerun make_certs.sh --refresh-policies")
        else:
            end_text = run("openssl", "x509", "-in", cert, "-noout", "-enddate").decode().strip()[9:]
            end = datetime.strptime(end_text, "%b %d %H:%M:%S %Y %Z").replace(tzinfo=timezone.utc)
            grant.find("validity/not_before").text = (now - timedelta(minutes=5)).strftime("%Y-%m-%dT%H:%M:%SZ")
            grant.find("validity/not_after").text = end.strftime("%Y-%m-%dT%H:%M:%SZ")
        rendered = ET.tostring(document.getroot(), encoding="utf-8", xml_declaration=True)
        if not permissions.exists() or permissions.read_bytes() != rendered or refresh:
            write(permissions, rendered)
            sign(root, permissions, signed)
        verify_policy(root, signed)
        print(f"Ready: {identity}")


def make_config(root, identity):
    if identity not in IDENTITIES:
        raise RuntimeError(f"Unknown identity: {identity}")
    directory = root / "identities" / identity
    check_pair(directory / "cert.pem", directory / "key.pem", root / "ca/identity/cert.pem")
    verify_policy(root, root / "governance.p7s")
    verify_policy(root, directory / "permissions.p7s")
    plugin_dir = os.environ.get("FUSE_DDS_PLUGIN_DIR")
    if not plugin_dir:
        raise RuntimeError("Security plugins not found; source security/scripts/env.sh")
    document = ET.Element("CycloneDDS", xmlns="https://cdds.io/config")
    domain = ET.SubElement(document, "Domain", id="0")
    security = ET.SubElement(domain, "Security")
    for section, plugin, init, finalize in (
            ("Authentication", "auth", "authentication", "authentication"),
            ("Cryptographic", "crypto", "crypto", "crypto"),
            ("AccessControl", "ac", "access_control", "access_control")):
        group = ET.SubElement(security, section)
        path = Path(plugin_dir) / f"libdds_security_{plugin}.so"
        if not path.is_file():
            raise RuntimeError(f"Missing security plugin: {path}")
        ET.SubElement(group, "Library", path=str(path), initFunction=f"init_{init}",
                      finalizeFunction=f"finalize_{finalize}")
        files = {"Authentication": {"IdentityCA": root / "ca/identity/cert.pem",
                                   "IdentityCertificate": directory / "cert.pem",
                                   "PrivateKey": directory / "key.pem"},
                 "AccessControl": {"PermissionsCA": root / "ca/permissions/cert.pem",
                                   "Governance": root / "governance.p7s",
                                   "Permissions": directory / "permissions.p7s"}}.get(section, {})
        for name, file in files.items():
            ET.SubElement(group, name).text = f"file:{file}"
    tracing = ET.SubElement(domain, "Tracing")
    ET.SubElement(tracing, "Verbosity").text = "warning"
    ET.SubElement(tracing, "OutputFile").text = "stderr"
    output = root / "runtime" / f"{identity}.xml"
    write(output, ET.tostring(document, encoding="utf-8", xml_declaration=True))
    print(output)


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("ca")
    certs = commands.add_parser("certs")
    certs.add_argument("identities", nargs="*", default=[])
    certs.add_argument("--refresh-policies", action="store_true")
    config = commands.add_parser("config")
    config.add_argument("identity", choices=IDENTITIES)
    args = parser.parse_args()
    root = root_dir()
    if args.command == "ca":
        make_ca(root)
        print(f"CAs ready: {root}")
    elif args.command == "certs":
        make_certs(root, args.identities or IDENTITIES, args.refresh_policies)
    else:
        make_config(root, args.identity)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ET.ParseError, ValueError) as error:
        sys.exit(f"DDS security: {error}")
