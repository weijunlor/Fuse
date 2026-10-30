#!/usr/bin/env python3
"""Production-thread TSan and real-renderer smoke checks for optimisations #3/#4.

Source security/scripts/env.sh, build the normal and concurrency-tsan targets,
then run with --tsan and/or --gui. Only child PIDs created here are stopped.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tsan", action="store_true")
    parser.add_argument("--tsan-dds-lib", type=Path,
                        help="Directory containing TSan-instrumented DDS and security plugins")
    parser.add_argument("--gui", action="store_true")
    args = parser.parse_args()
    if not (args.tsan or args.gui):
        parser.error("select --tsan and/or --gui")
    for name in ("server", "baseline_server"):
        if subprocess.run(["pgrep", "-x", name], capture_output=True).returncode == 0:
            parser.error(f"stop the existing {name} before isolated tests")
    output = ROOT / "bench/results/concurrency-tests"
    output.mkdir(parents=True, exist_ok=True)
    records = []
    report_name = "production" if args.tsan and args.gui else "tsan-production" if args.tsan else "gui"

    def record(name, good):
        records.append({"check": name, "passed": good})
        (output / f"{report_name}-report.json").write_text(json.dumps(records, indent=2) + "\n")
        print(f"{'PASS' if good else 'FAIL'} {name}", flush=True)
        if not good:
            raise RuntimeError(f"inspect {output}")

    def stop(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    # Per-process ASLR workaround for GCC 11 TSan's mapping conflict on WSL.
    tsan_prefix = ["setarch", os.uname().machine, "-R", "env", "TSAN_OPTIONS=halt_on_error=1"]
    with tempfile.TemporaryDirectory(prefix="fuse-concurrency-") as directory:
        env = dict(os.environ, FUSE_AUTH_DIR=directory, FUSE_AUTH="on", FUSE_DDS_SECURITY="on")
        for key in ("FUSE_AUTH_USERS", "FUSE_PASSWORD_FILE", "FUSE_USERNAME"):
            env.pop(key, None)
        invalid = subprocess.run(["build/server"], cwd=ROOT,
            env=dict(env, FUSE_HANDOFF="invalid"), text=True, capture_output=True, timeout=5)
        record("invalid-handoff-rejected", invalid.returncode != 0 and "FUSE_HANDOFF must be" in invalid.stderr)
        subprocess.run(["python3", "security/scripts/accounts.py", "demo"], cwd=ROOT,
                       env=env, check=True, stdout=subprocess.DEVNULL)
        if args.tsan:
            for mode in ("spsc", "mutex"):
                current = dict(env, FUSE_HANDOFF=mode)
                if args.tsan_dds_lib:
                    library = args.tsan_dds_lib.resolve()
                    current["LD_LIBRARY_PATH"] = str(library) + ":" + current.get("LD_LIBRARY_PATH", "")
                    current["FUSE_DDS_PLUGIN_DIR"] = str(library)
                server_log = output / f"tsan-server-{mode}.log"
                with server_log.open("w") as log:
                    server = subprocess.Popen(tsan_prefix + ["build/concurrency-tsan/server"],
                                              cwd=ROOT, env=current, stdout=log, stderr=subprocess.STDOUT)
                    try:
                        time.sleep(1)
                        record(f"tsan-server-{mode}-started", server.poll() is None)
                        worker = subprocess.run(tsan_prefix + ["build/concurrency-tsan/auth_worker"],
                            cwd=ROOT, env=dict(current, FUSE_DDS_IDENTITY="client-1"),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=25)
                        (output / f"tsan-worker-{mode}.log").write_text(worker.stdout)
                        record(f"tsan-worker-{mode}-movement-leave", worker.returncode == 0 and "moved=1" in worker.stdout)
                        time.sleep(0.2)
                        record(f"tsan-server-{mode}-clean", server.poll() is None and
                               "ThreadSanitizer" not in server_log.read_text())
                    finally:
                        stop(server)
        if args.gui:
            current = dict(env, FUSE_HANDOFF="spsc")
            server_log = output / "gui-server.log"
            clients = []
            with server_log.open("w") as log:
                server = subprocess.Popen(["stdbuf", "-oL", "build/server"], cwd=ROOT,
                                          env=current, stdout=log, stderr=subprocess.STDOUT)
                try:
                    time.sleep(0.7)
                    record("gui-server-started", server.poll() is None)
                    for index in (1, 2):
                        screenshot = output / f"gui-client-{index}.png"
                        screenshot.unlink(missing_ok=True)
                        client_log = (output / f"gui-client-{index}.log").open("w")
                        process = subprocess.Popen(["stdbuf", "-oL", "build/client", "--frames", "6000"], cwd=ROOT,
                            env=dict(current, FUSE_DDS_IDENTITY=f"client-{index}", FUSE_AUTO_MOVE="1",
                                     FUSE_SCREENSHOT=str(screenshot), FUSE_SCREENSHOT_DELAY="5"),
                            stdout=client_log, stderr=subprocess.STDOUT)
                        clients.append((index, process, client_log, screenshot))
                    deadline = time.monotonic() + 20
                    while not all(shot.is_file() for _, _, _, shot in clients):
                        if time.monotonic() > deadline or any(proc.poll() is not None for _, proc, _, _ in clients):
                            record("gui-screenshots", False)
                        time.sleep(0.1)
                    record("gui-two-clients-joined", server_log.read_text().count("Join response sent") >= 2)
                    for index, process, client_log, screenshot in clients:
                        # Normal frame-limited exit exercises the production
                        # stop/join/Leave path. Destroying an X drawable from
                        # outside GLFW can abort Mesa before that cleanup.
                        process.wait(timeout=60)
                        client_log.flush()
                        text = (output / f"gui-client-{index}.log").read_text()
                        record(f"gui-client-{index}-render-close-leave", process.returncode == 0 and
                               "Left the game" in text and screenshot.stat().st_size > 0)
                    time.sleep(0.5)
                    record("gui-server-freed-both-slots", server_log.read_text().count(" left") >= 2)
                finally:
                    for _, process, client_log, _ in clients:
                        stop(process)
                        client_log.close()
                    stop(server)
    sources = [*ROOT.glob("common/*.[ch]"), *ROOT.glob("client/*.[ch]"), *ROOT.glob("server/*.[ch]")]
    (output / "source-sha256.json").write_text(json.dumps({
        str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sources}, indent=2) + "\n")


if __name__ == "__main__":
    main()
