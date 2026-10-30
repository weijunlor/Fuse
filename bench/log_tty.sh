#!/usr/bin/env bash
# Measure the cost of the game's printf logging when stdout is a terminal
# (optimisation #17). bench/profile.sh sends output to /dev/null, where stdio
# block-buffers it (one write per 4 KB). A terminal is line-buffered, so every
# printed line is one write() syscall: the worst case for logging.
#
# Usage: bench/log_tty.sh <label> <name>=<bin_dir> [<name>=<bin_dir> ...]
#   e.g. bench/log_tty.sh opt-17-log-macros before=/tmp/wt/build after=build
#
# Each run starts 1 server + CLIENTS clients, every one with stdout on its own
# pseudo-terminal (util-linux "script"; its copy of the output goes to
# /dev/null), waits WARMUP seconds, then counts for DURATION seconds with
#   perf stat -e task-clock,syscalls:sys_enter_write,... -p <pid>
# on the server and on client 1. Builds are interleaved run by run, so slow
# drift of the machine affects all of them equally.
# Results: bench/results/<label>/tty/raw/stat_<name>_<proc>_<run>.txt and
# tty/report.txt. Pinning as in profile.sh (server CPUs 0-3, clients 4-15).
# perf runs with sudo because syscall tracepoints are root-only.

set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -lt 2 ]; then
    sed -n '/^# Usage/,/^$/p' "$0" >&2
    exit 1
fi
LABEL=$1
shift

RUNS=${RUNS:-10}
DURATION=${DURATION:-30}
CLIENTS=${CLIENTS:-4}
WARMUP=${WARMUP:-5}
SUDO=${SUDO-sudo}
source bench/security_env.sh
dds_check_clients "$CLIENTS"
# MOVE=1: clients move on their own (FUSE_AUTO_MOVE=1, see bench/profile.sh),
# so the per-input log lines are printed too, not only the per-position ones
MOVE=${MOVE:-0}
[ "$MOVE" = 1 ] && export FUSE_AUTO_MOVE=1
EVENTS=task-clock,context-switches,syscalls:sys_enter_write,syscalls:sys_enter_futex

OUT=bench/results/$LABEL/tty
RAW=$OUT/raw
mkdir -p "$RAW"
arr_tty_pids=()
stop_ttys() {
    for int_pid in "${arr_tty_pids[@]}"; do
        # script's child execs the measured game; never select unrelated games.
        pkill -P "$int_pid" 2>/dev/null || true
        kill "$int_pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    arr_tty_pids=()
}
trap stop_ttys EXIT

# Start a program with stdout and stderr on a new pseudo-terminal
start_on_tty()
{
    local str_cpus=$1 str_bin=$2 str_identity=$3
    if [ "$FUSE_DDS_SECURITY" = on ]; then
        export FUSE_DDS_IDENTITY=$str_identity
        export CYCLONEDDS_URI
        CYCLONEDDS_URI=$(python3 security/scripts/security.py config "$str_identity")
    fi
    script -qfec "exec taskset -c $str_cpus $str_bin" /dev/null > /dev/null 2>&1 &
    arr_tty_pids+=($!)
}

for int_run in $(seq 1 "$RUNS"); do
    for str_pair in "$@"; do
        str_name=${str_pair%%=*}
        str_dir=${str_pair#*=}
        echo "run $int_run/$RUNS: $str_name ($str_dir)"

        start_on_tty 0-3 "$str_dir/server" server
        sleep 0.5
        for int_client in $(seq 1 "$CLIENTS"); do
            start_on_tty 4-15 "$str_dir/client" "client-$int_client"
        done
        sleep "$WARMUP"

        # The programs are children of "script"; find them by full path
        int_server_pid=$(pgrep -P "${arr_tty_pids[0]}")
        int_client_pid=$(pgrep -P "${arr_tty_pids[1]}")

        $SUDO perf stat -e "$EVENTS" -p "$int_server_pid" \
            -o "$RAW/stat_${str_name}_server_$int_run.txt" -- sleep "$DURATION" &
        $SUDO perf stat -e "$EVENTS" -p "$int_client_pid" \
            -o "$RAW/stat_${str_name}_client_$int_run.txt" -- sleep "$DURATION"
        wait "$!" 2> /dev/null || true
        sleep 1

        stop_ttys
        sleep 2
    done
done

# Summary table: mean and per-run values of each counter
python3 - "$RAW" "$DURATION" "$MOVE" "$@" > "$OUT/report.txt" <<'EOF'
import glob, re, statistics, sys
raw, duration, move, pairs = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4:]
names = [p.split('=', 1)[0] for p in pairs]
keys = [('task-clock', 'CPUs', lambda v: v / 1000 / duration),
        ('syscalls:sys_enter_write', 'writes/s', lambda v: v / duration),
        ('syscalls:sys_enter_futex', 'futex/s', lambda v: v / duration),
        ('context-switches', 'ctx-sw/s', lambda v: v / duration)]
def read(path):
    vals = {}
    for line in open(path):
        m = re.match(r'\s*([\d,.]+)\s+(?:msec\s+)?(\S+)', line)
        if m and any(m.group(2) == k for k, _, _ in keys):
            vals[m.group(2)] = float(m.group(1).replace(',', ''))
    return vals
print('#17 logging cost with stdout on a terminal (line-buffered)')
print(f'{duration:.0f} s per run; per-second rates; mean (sd) and per-run values')
print('Clients ' + ('moving (FUSE_AUTO_MOVE=1: ~100 input messages/s each)' if move == '1'
                    else 'idle (no input)') + '\n')
for proc in ('server', 'client'):
    print(f'-- {proc}')
    for key, label, conv in keys:
        for name in names:
            runs = []
            for f in sorted(glob.glob(f'{raw}/stat_{name}_{proc}_*.txt'),
                            key=lambda s: int(re.search(r'_(\d+)\.txt$', s).group(1))):
                v = read(f).get(key)
                if v is not None:
                    runs.append(conv(v))
            if not runs:
                continue
            sd = statistics.stdev(runs) if len(runs) > 1 else 0
            fmt = '.3f' if label == 'CPUs' else '.1f'
            per = ' '.join(f'{r:{fmt}}' for r in runs)
            print(f'{label:9} {name:8} mean {statistics.mean(runs):{fmt}} (sd {sd:{fmt}})  runs: {per}')
    print()
EOF
printf 'DDS security: %s\n' "$FUSE_DDS_SECURITY" >> "$OUT/report.txt"
cat "$OUT/report.txt"
