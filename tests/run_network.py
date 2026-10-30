"""Check merged wait/publish modes against real DDS traffic, in isolation."""
import os
from pathlib import Path
import subprocess
import uuid

root = Path(__file__).resolve().parents[1]
out = root / "build" / "network-tests"
out.mkdir(exist_ok=True)
for args in (["--bots", "5"], ["--bots", "junk"], ["--snapshot-hz", "7"],
             ["--player-timeout", "-1"], ["--wait", "wrong"], ["--branches"]):
    result = subprocess.run(["./build/server", *args], cwd=root, capture_output=True, timeout=3)
    assert result.returncode == 1, (args, result.returncode)
print("PASS invalid options", flush=True)

cases = [
    ("default", [], {}, 0, "sleep and waitset", "after unlocking"),
    ("sleep-locked", ["--publish", "locked"], {}, 0, "sleep and waitset", "under the mutex"),
    ("busy-unlocked", ["--wait", "busy"], {}, 0, "spin", "after unlocking"),
    ("env-busy-locked", [], {"FUSE_SERVER_WAIT": "spin", "FUSE_WRITE_UNDER_LOCK": "on"},
     0, "spin", "under the mutex"),
    ("cli-overrides-env", ["--wait", "sleep", "--publish", "unlocked"],
     {"FUSE_SERVER_WAIT": "spin", "FUSE_WRITE_UNDER_LOCK": "on"},
     0, "sleep and waitset", "after unlocking"),
    ("bot", ["--bots", "1"], {}, 1, "sleep and waitset", "after unlocking"),
]
for name, args, overrides, bots, wait_mode, publish_mode in cases:
    env = os.environ.copy()
    for key in ("FUSE_JOIN_NONCE", "FUSE_LEAVE", "FUSE_QOS", "FUSE_SERVER_WAIT", "FUSE_WRITE_UNDER_LOCK"):
        env.pop(key, None)
    env.update(overrides)
    env["CYCLONEDDS_URI"] = (
        '<CycloneDDS><Domain id="any"><Discovery><Tag>'
        f'fuse-test-{uuid.uuid4().hex}</Tag></Discovery></Domain></CycloneDDS>'
    )
    log_path = out / f"{name}-server.log"
    with log_path.open("w") as log:
        # Line buffering preserves startup-mode evidence when terminating the
        # test process. stdbuf ships with the Ubuntu coreutils package.
        server = subprocess.Popen(["stdbuf", "-oL", "./build/server", *args], cwd=root,
                                  env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            result = subprocess.run(["./build/test_dds", str(bots)], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=25)
            (out / f"{name}-client.log").write_text(result.stdout + result.stderr)
            assert server.poll() is None, f"{name}: server exited early"
            assert result.returncode == 0, f"{name}: {result.stdout}{result.stderr}"
            startup = log_path.read_text()
            assert f"Server wait: {wait_mode}" in startup, startup
            assert f"snapshot write: {publish_mode}" in startup, startup
            print(f"PASS network scenario: {name}", flush=True)
        finally:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)
