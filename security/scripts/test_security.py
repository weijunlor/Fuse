#!/usr/bin/env python3
"""Repeatable DDS acceptance tests. Run after sourcing env.sh and building probes.

The harness owns every child PID and temporary credential tree it creates.
Packet files use Cyclone's built-in UDP packet logging (synthetic IP headers).
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
import security

REPO = Path(__file__).resolve().parents[2]
RESULTS = []
OUT = None


def record(name, passed, detail=""):
    RESULTS.append({"test": name, "passed": bool(passed), "detail": detail})
    print(f"{'PASS' if passed else 'FAIL'} {name}: {detail}", flush=True)
    if not passed:
        raise AssertionError(name)


def environment(identity="client-1", mode="on", root=None):
    env = dict(os.environ, FUSE_DDS_IDENTITY=identity, FUSE_DDS_SECURITY=mode)
    env.pop("CYCLONEDDS_URI", None)
    if root is not None:
        env["FUSE_SECURITY_DIR"] = str(root)
    return env


def probe(name, arguments, env, expected=0, baseline=False):
    executable = "build/dds_security_probe" + ("_baseline" if baseline else "")
    result = subprocess.run([executable, *arguments], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    (OUT / "raw" / f"{name}.txt").write_text(result.stdout)
    passed = result.returncode == expected if expected is not None else result.returncode != 0
    record(name, passed, f"exit={result.returncode}")
    return result.stdout


@contextmanager
def server(name, mode="on", baseline=False, capture=False):
    env = environment("server", mode)
    generated = None
    if baseline and mode == "on":
        generated = subprocess.check_output([sys.executable, "security/scripts/security.py", "config", "server"], env=env, text=True).strip()
        env["CYCLONEDDS_URI"] = generated
    if capture:
        if generated:
            document = ET.parse(generated).getroot()
            # Cyclone 0.10 parses literal element names, not namespace prefixes.
            for node in document.iter():
                node.tag = node.tag.split("}")[-1]
            document.set("xmlns", "https://cdds.io/config")
            domain = document.find("Domain")
            old_tracing = domain.find("Tracing")
            if old_tracing is not None:
                domain.remove(old_tracing)
        else:
            document = ET.Element("CycloneDDS", xmlns="https://cdds.io/config")
            domain = ET.SubElement(document, "Domain", id="0")
        tracing = ET.SubElement(domain, "Tracing")
        ET.SubElement(tracing, "PacketCaptureFile").text = str(OUT / "packets" / f"{name}.pcap")
        # In insecure FUSE mode the application deliberately clears external XML;
        # baseline binaries are always configuration-driven and retain tracing.
        if mode == "on" or baseline:
            config = OUT / "raw" / f"{name}-config.xml"
            config.write_bytes(ET.tostring(document, encoding="utf-8", xml_declaration=True))
            env["CYCLONEDDS_URI"] = str(config)
    executable = "build/baseline_server" if baseline else "build/server"
    with (OUT / "raw" / f"{name}-server.txt").open("w") as log:
        process = subprocess.Popen(["stdbuf", "-oL", executable], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(0.8)
            record(f"{name}-server-start", process.poll() is None)
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            time.sleep(0.3)


def copy_credentials(source, destination):
    shutil.copytree(source, destination)
    return destination


def mutate_permissions(root, identity, transform):
    xml = root / "identities" / identity / "permissions.xml"
    document = ET.parse(xml)
    transform(document.getroot())
    xml.write_bytes(ET.tostring(document.getroot(), encoding="utf-8", xml_declaration=True))
    security.sign(root, xml, xml.with_suffix(".p7s"))


def run_tests(root):
    defaults = environment()
    defaults.pop("FUSE_DDS_SECURITY")
    probe("participant-default-secure", ["participant"], defaults)
    missing_identity = environment()
    missing_identity.pop("FUSE_DDS_IDENTITY")
    probe("missing-client-identity", ["participant"], missing_identity, expected=None)
    probe("invalid-mode", ["participant"], environment(mode="invalid"), expected=None)
    probe("wrong-role", ["participant"], environment(identity="server"), expected=None)
    for mode in ("on", "off"):
        result = subprocess.run(["bash", "-c", "source bench/security_env.sh && dds_run client-1 build/dds_security_probe participant"],
                                env=environment(mode=mode), capture_output=True, text=True, timeout=15)
        (OUT / "raw" / f"benchmark-launcher-{mode}.txt").write_text(result.stdout + result.stderr)
        record(f"benchmark-launcher-{mode}", result.returncode == 0 and "participant=ok" in result.stdout)
    for topic in ("GameSnapshot", "JoinResponse", "UnlistedTopic"):
        text = probe(f"deny-writer-{topic}", ["deny-writer", topic], environment())
        record(f"{topic}-security-denial", "denied=1" in text)
    probe("deny-read-input", ["deny-reader", "Input"], environment())
    with tempfile.TemporaryDirectory(prefix="fuse-security-tests-") as temporary:
        stage = Path(temporary)
        missing = copy_credentials(root, stage / "missing")
        (missing / "identities/client-1/key.pem").unlink()
        probe("missing-key", ["participant"], environment(root=missing), expected=None)
        mismatch = copy_credentials(root, stage / "mismatch")
        shutil.copyfile(mismatch / "identities/client-2/key.pem", mismatch / "identities/client-1/key.pem")
        probe("mismatched-key", ["participant"], environment(root=mismatch), expected=None)
        tampered = copy_credentials(root, stage / "tampered")
        permissions = tampered / "identities/client-1/permissions.p7s"
        permissions.write_bytes(permissions.read_bytes().replace(b"<default>DENY", b"<default>ALLOW", 1))
        probe("tampered-permissions", ["participant"], environment(root=tampered), expected=None)
        expired = copy_credentials(root, stage / "expired")
        def expire(document):
            document.find("permissions/grant/validity/not_before").text = "2019-01-01T00:00:00Z"
            document.find("permissions/grant/validity/not_after").text = "2020-01-01T00:00:00Z"
        mutate_permissions(expired, "client-1", expire)
        probe("expired-permissions", ["participant"], environment(root=expired), expected=None)
        untrusted = stage / "untrusted"
        # A valid, internally consistent identity from an entirely different CA.
        subprocess.run([sys.executable, "security/scripts/security.py", "certs", "client-3"],
                       env=environment(root=untrusted), check=True, stdout=subprocess.DEVNULL)
        with server("fuse-secure", capture=True) as process:
            text = probe("secure-play", ["play", "4"], environment())
            record("secure-movement-and-leave", "moved=1" in text and "leave_confirmed=1" in text)
            # Keep a known-good peer active throughout both rejection attempts.
            with (OUT / "raw/authorized-during-rejection.txt").open("w") as log:
                good = subprocess.Popen(["build/dds_security_probe", "play", "9"],
                                        env=environment("client-2"), stdout=log, stderr=subprocess.STDOUT)
                try:
                    text = probe("reject-plaintext-peer", ["play", "3"], environment("client-3", "off"), expected=1)
                    record("plaintext-no-game-data", "joined=0" in text and "states=0" in text)
                    text = probe("reject-untrusted-peer", ["play", "3"], environment("client-3", root=untrusted), expected=1)
                    record("untrusted-no-game-data", "joined=0" in text and "states=0" in text)
                    good.wait(timeout=15)
                    record("authorized-peer-survives", good.returncode == 0 and process.poll() is None)
                finally:
                    if good.poll() is None:
                        good.terminate()
                        good.wait(timeout=5)
        # External XML without security must not downgrade participant QoS.
        no_security = OUT / "raw/attempted-downgrade.xml"
        no_security.write_text('<CycloneDDS><Domain id="0"/></CycloneDDS>')
        env = environment()
        env["CYCLONEDDS_URI"] = str(no_security)
        with server("downgrade-secure"):
            probe("external-xml-stays-secure", ["play", "3"], env)
        with server("fuse-insecure", mode="off"):
            probe("plaintext-regression", ["play", "3"], environment(mode="off"))
        # The baseline receives security entirely from XML, without source edits.
        with server("baseline-secure", baseline=True, capture=True):
            probe("baseline-secure-play", ["play", "4"], environment(), baseline=True)
        with server("baseline-plaintext", mode="off", baseline=True, capture=True):
            probe("baseline-plaintext-play", ["play", "4"], environment(mode="off"), baseline=True)


def gui_tests():
    """Real WSLg clients; --frames gives FUSE a normal close and reliable Leave."""
    with server("fuse-gui"), tempfile.TemporaryDirectory(prefix="fuse-gui-") as temporary:
        clients = []
        try:
            for index in (1, 2):
                env = environment(f"client-{index}")
                env["FUSE_AUTO_MOVE"] = "1"
                env["FUSE_SCREENSHOT"] = str(OUT / "raw" / f"client-{index}.png")
                env["FUSE_SCREENSHOT_DELAY"] = "8"
                # Isolate assets/output from the user's working-directory capture.
                working = Path(temporary) / f"client-{index}"
                working.mkdir()
                (working / "assets").symlink_to(REPO / "assets", target_is_directory=True)
                log = (OUT / "raw" / f"gui-client-{index}.txt").open("w")
                process = subprocess.Popen(["stdbuf", "-oL", str(REPO / "build/client"), "--frames", "6000"], env=env,
                                           cwd=working, stdout=log, stderr=subprocess.STDOUT)
                clients.append((index, process, log, working))
            deadline = time.monotonic() + 8
            while (OUT / "raw/fuse-gui-server.txt").read_text().count("Join response sent") < 2:
                if time.monotonic() > deadline:
                    record("gui-both-clients-joined", False)
                time.sleep(0.1)
            record("gui-both-clients-joined", True)
            time.sleep(0.3)
            for index, process, _, _ in clients:
                windows = subprocess.run(["xdotool", "search", "--onlyvisible", "--pid", str(process.pid)],
                                         capture_output=True, text=True, timeout=5)
                record(f"gui-client-{index}-window", windows.returncode == 0)
            for index, process, log, _ in clients:
                process.wait(timeout=60)
                log.close()
                text = (OUT / "raw" / f"gui-client-{index}.txt").read_text()
                record(f"gui-client-{index}", process.returncode == 0 and
                       "Join response received" in text and "Left the game" in text and
                       (OUT / "raw" / f"client-{index}.png").is_file())
            text = (OUT / "raw/fuse-gui-server.txt").read_text()
            record("gui-both-clients-left", text.count(" left") >= 2)
        finally:
            for _, process, log, _ in clients:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                log.close()
    with server("baseline-gui", baseline=True):
        env = environment()
        env["CYCLONEDDS_URI"] = subprocess.check_output(
            [sys.executable, "security/scripts/security.py", "config", "client-1"], env=env, text=True).strip()
        with (OUT / "raw/baseline-gui-client.txt").open("w") as log:
            process = subprocess.Popen(["stdbuf", "-oL", "build/baseline_client"], env=env,
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                time.sleep(4)
                windows = subprocess.run(["xdotool", "search", "--onlyvisible", "--pid", str(process.pid)],
                                         capture_output=True, text=True, timeout=5)
                record("baseline-real-window", process.poll() is None and windows.returncode == 0)
                text = (OUT / "raw/baseline-gui-client.txt").read_text()
                record("baseline-real-client-traffic", "Join response received" in text and
                       "Position received" in text)
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)


def packet_tests():
    counts = {}
    for name in ("baseline-plaintext", "baseline-secure", "fuse-secure"):
        path = OUT / "packets" / f"{name}.pcap"
        # SIGTERM does not flush the server's last stdio buffer. Preserve every
        # complete packet and remove only an incomplete trailing pcap record.
        data = path.read_bytes()
        endian = "<" if data[:4] in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1") else ">"
        offset = 24
        while offset + 16 <= len(data):
            length = struct.unpack_from(endian + "I", data, offset + 8)[0]
            if offset + 16 + length > len(data):
                break
            offset += 16 + length
        if offset != len(data):
            path.write_bytes(data[:offset])
            (OUT / "raw" / f"{name}-capture-tail.txt").write_text(
                f"Removed {len(data) - offset} bytes of an incomplete final pcap record after server shutdown.\n")
        result = subprocess.run(["tshark", "-r", str(path), "-Y",
                                 "rtps.sm.id == 0x33 || rtps.sm.id == 0x30", "-T", "fields",
                                 "-e", "frame.number", "-e", "_ws.col.Info"],
                                capture_output=True, text=True, timeout=15)
        record(f"{name}-pcap-decodes", result.returncode == 0)
        counts[name] = len(result.stdout.splitlines())
        (OUT / "packets" / f"{name}-encrypted-frames.txt").write_text(result.stdout)
        (OUT / "raw" / f"{name}-tshark.txt").write_text(result.stderr)
        record(f"{name}-capture", path.stat().st_size > 24)
    record("plaintext-has-no-encrypted-frames", counts["baseline-plaintext"] == 0)
    record("baseline-rtps-encrypted", counts["baseline-secure"] > 0, str(counts["baseline-secure"]))
    record("fuse-rtps-encrypted", counts["fuse-secure"] > 0, str(counts["fuse-secure"]))


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="bench/results/dds-security-wsl")
    parser.add_argument("--gui", action="store_true", help="Also exercise real WSLg clients")
    args = parser.parse_args()
    os.chdir(REPO)
    OUT = Path(args.out).resolve()
    (OUT / "raw").mkdir(parents=True, exist_ok=True)
    (OUT / "packets").mkdir(exist_ok=True)
    root = Path(os.environ["FUSE_SECURITY_DIR"])
    metadata = {"recorded_utc": datetime.now(timezone.utc).isoformat(),
                "cyclonedds": subprocess.check_output(["pkg-config", "--modversion", "CycloneDDS"], text=True).strip(),
                "kernel": os.uname().release,
                "packet_source": "Cyclone DDS built-in UDP capture; synthetic IP/UDP headers",
                "binaries": {name: hashlib.sha256((REPO / "build" / name).read_bytes()).hexdigest()
                             for name in ("server", "client", "baseline_server", "baseline_client", "dds_security_probe")}}
    (OUT / "setup.json").write_text(json.dumps(metadata, indent=2) + "\n")
    # Refuse to interfere with an already running game in the fixed domain.
    for name in ("server", "baseline_server"):
        running = subprocess.run(["pgrep", "-x", name], capture_output=True)
        if running.returncode == 0:
            sys.exit(f"Stop the existing {name} before running isolated tests")
    try:
        run_tests(root)
        if args.gui:
            gui_tests()
        packet_tests()
    finally:
        (OUT / "results.json").write_text(json.dumps(RESULTS, indent=2) + "\n")
        (OUT / "report.txt").write_text("DDS security acceptance tests\n" + "\n".join(
            f"{'PASS' if row['passed'] else 'FAIL'} {row['test']}: {row['detail']}" for row in RESULTS) + "\n")
    print(f"All {len(RESULTS)} checks passed. Evidence: {OUT}")


if __name__ == "__main__":
    main()
