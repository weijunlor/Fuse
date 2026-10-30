#!/usr/bin/env python3
"""Authentication acceptance checks. Run after sourcing security/scripts/env.sh."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import signal
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "bench/results/app-auth-tests")
    args = parser.parse_args()
    for name in ("server", "baseline_server"):
        if subprocess.run(["pgrep", "-x", name], capture_output=True).returncode == 0:
            parser.error(f"stop the existing {name} before running isolated tests")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    metadata = {"kernel": os.uname().release,
                "openssl": subprocess.check_output(["openssl", "version"], text=True).strip(),
                "cyclonedds": subprocess.check_output(["pkg-config", "--modversion", "CycloneDDS"], text=True).strip(),
                "binaries": {name: hashlib.sha256((ROOT / "build" / name).read_bytes()).hexdigest()
                             for name in ("server", "client", "test_auth", "auth_probe", "auth_worker")}}
    (output / "setup.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    results = []

    def run(name, command, env, expected=0, timeout=20):
        proc = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, timeout=timeout)
        (output / f"{name}.log").write_text(proc.stdout, encoding="utf-8")
        good = proc.returncode == expected if expected is not None else proc.returncode != 0
        results.append((name, good))
        print(f"{'PASS' if good else 'FAIL'} {name}", flush=True)
        if not good:
            raise RuntimeError(f"{name}: inspect {output / (name + '.log')}")
        return proc

    env = os.environ.copy()
    env["FUSE_AUTH"] = "on"
    with tempfile.TemporaryDirectory(prefix="fuse-auth-accept-") as directory:
        env["FUSE_AUTH_DIR"] = directory
        for key in ("FUSE_AUTH_USERS", "FUSE_PASSWORD_FILE", "FUSE_USERNAME", "FUSE_JOIN_NONCE"):
            env.pop(key, None)
        run("provision", ["python3", "security/scripts/accounts.py", "demo"], env)
        before = hashlib.sha256((Path(directory) / "users.db").read_bytes()).digest()
        run("provision-no-overwrite", ["python3", "security/scripts/accounts.py", "demo"], env, expected=None)
        assert before == hashlib.sha256((Path(directory) / "users.db").read_bytes()).digest()
        run("unit", ["build/test_auth"], env)
        missing = dict(env, FUSE_AUTH_USERS=directory + "/missing")
        run("server-missing-database", ["build/server"], missing, expected=None)
        missing = dict(env, FUSE_DDS_IDENTITY="client-1", FUSE_PASSWORD_FILE=directory + "/missing")
        run("client-missing-password", ["build/auth_worker"], missing, expected=None)
        bad_config = dict(env, FUSE_AUTH="off", FUSE_DDS_SECURITY="on")
        run("reject-auth-downgrade", ["build/server"], bad_config, expected=None)
        run("reject-invalid-mode", ["build/server"], dict(env, FUSE_AUTH="typo"), expected=None)
        for mode in ("on", "off"):
            server_env = dict(env, FUSE_DDS_SECURITY=mode)
            with (output / f"server-{mode}.log").open("w") as log:
                server = subprocess.Popen(["build/server"], cwd=ROOT, env=server_env,
                                          stdout=log, stderr=subprocess.STDOUT)
                try:
                    time.sleep(0.5)
                    if server.poll() is not None:
                        raise RuntimeError(f"server failed: {output / ('server-' + mode + '.log')}")
                    client_env = dict(server_env, FUSE_DDS_IDENTITY="client-1")
                    run(f"wire-{mode}", ["build/auth_probe"], client_env)
                    run(f"worker-{mode}", ["build/auth_worker"], client_env)
                    if mode == "on":
                        rejoin_log = output / "worker-rejoin.log"
                        with rejoin_log.open("w") as worker_log:
                            worker = subprocess.Popen(["build/auth_worker"], cwd=ROOT,
                                env=dict(client_env, FUSE_TEST_REJOIN="1"),
                                stdout=worker_log, stderr=subprocess.STDOUT)
                            try:
                                deadline = time.monotonic() + 10
                                while "worker_ready_to_pause=1" not in rejoin_log.read_text():
                                    if worker.poll() is not None or time.monotonic() > deadline:
                                        raise RuntimeError("rejoin worker did not join/pause")
                                    time.sleep(0.05)
                                time.sleep(6)  # default server timeout is 5 s
                                worker.send_signal(signal.SIGCONT)
                                worker.wait(timeout=10)
                                text = rejoin_log.read_text()
                                good = worker.returncode == 0 and "joining again" in text and text.count("Join response received") >= 2
                                results.append(("worker-rejoin", good))
                                if not good:
                                    raise RuntimeError("production worker failed to reauthenticate after timeout")
                                print("PASS worker-rejoin", flush=True)
                            finally:
                                if worker.poll() is None:
                                    worker.send_signal(signal.SIGCONT); worker.terminate(); worker.wait(timeout=5)
                    # The second account validates default identity -> username selection too.
                    run(f"probe-{mode}", ["build/dds_security_probe", "play", "3"],
                        dict(server_env, FUSE_DDS_IDENTITY="client-2"))
                    run(f"joiner-{mode}", ["build/dds_joiner", "2"],
                        dict(server_env, FUSE_DDS_IDENTITY="client-3"))
                    wrong = Path(directory) / "wrong.pass"
                    wrong.write_text("wrong-password\n", encoding="ascii"); wrong.chmod(0o600)
                    run(f"wrong-password-{mode}", ["build/auth_worker"],
                        dict(client_env, FUSE_PASSWORD_FILE=str(wrong)), expected=None)
                finally:
                    server.terminate()
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill(); server.wait()
        # Preserve the original unauthenticated path only in explicit plaintext comparisons.
        comparison = dict(env, FUSE_AUTH="off", FUSE_DDS_SECURITY="off")
        with (output / "server-comparison.log").open("w") as log:
            server = subprocess.Popen(["build/server"], cwd=ROOT, env=comparison,
                                      stdout=log, stderr=subprocess.STDOUT)
            try:
                time.sleep(0.5)
                run("plaintext-comparison", ["build/auth_worker"], comparison)
            finally:
                server.terminate(); server.wait(timeout=5)
    report = "\n".join(f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in results) + "\n"
    (output / "report.txt").write_text(report, encoding="utf-8")
    print(f"PASS acceptance checks={len(results)}; logs: {output}")


if __name__ == "__main__":
    main()
