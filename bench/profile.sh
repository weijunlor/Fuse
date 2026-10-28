#!/usr/bin/env bash
# Profile one build of the game (server + clients) with perf and save
# everything needed to write the final report later:
#
#   bench/results/<label>/
#     setup.txt    machine, OS, tool versions, build flags and optimisation
#                  level (read from the binary's debug info), git commit
#     report.txt   how the profiling was done (scenario, exact perf commands)
#                  and a summary of the results
#     raw/         untouched perf output: perf stat text (as printed by perf), perf.data files,
#                  perf script dumps, perf trace / perf lock output
#
# Usage: bench/profile.sh <label> <bin_dir>
#   <bin_dir> contains the programs "server" and "client", e.g.
#     make baseline-O0 && bench/profile.sh baseline-O0 build/baseline-O0
#     make profile     && bench/profile.sh opt-01-unlock-before-publish build
#
# Settings (environment variables, defaults in brackets):
#   RUNS [10]  DURATION [30] seconds  CLIENTS [4]  WARMUP [5] seconds
#   RECORD_DURATION [10] seconds of perf record per process
#   STEPS ["stat lock trace record"]  sections to run
#   MOVE [0]  1 = every client moves on its own (FUSE_AUTO_MOVE=1, client/input.c:
#             holds right, down, left, up for 1 s each, like a held arrow key,
#             ~100 input messages/s per client); 0 = idle clients
#   SUDO ["sudo"]  perf lock/trace read root-only tracepoint files, so they
#                  run as root; the game itself always runs as you
#
# Scenario: 1 server + CLIENTS clients on this machine, idle (clients join on
# their own and send no input) or moving (MOVE=1). Every measurement starts fresh processes,
# waits WARMUP seconds, then attaches perf by PID, so only the steady state is
# measured. perf stat -r is not used: a client that already joined never
# rejoins a new server, so the server cannot be restarted on its own.
#
# Pinning (Intel hybrid i7-14650HX: P-cores are CPUs 0-15, E-cores 16-23):
#   server -> CPUs 0-3, clients -> CPUs 4-15
#
# Needs kernel.perf_event_paranoid <= -1, kernel.kptr_restrict = 0 and the
# performance governor (docs/PLAN.md section 7); checked below.

set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -ne 2 ]; then
    sed -n '/^# Usage/,/^$/p' "$0" >&2
    exit 1
fi
LABEL=$1
BIN_DIR=$2
SERVER=$BIN_DIR/server
CLIENT=$BIN_DIR/client

RUNS=${RUNS:-10}
DURATION=${DURATION:-30}
CLIENTS=${CLIENTS:-4}
WARMUP=${WARMUP:-5}
RECORD_DURATION=${RECORD_DURATION:-10}
RECORD_FREQ=499
STEPS=${STEPS:-stat lock trace record}
SUDO=${SUDO-sudo}
SERVER_CPUS=0-3
CLIENT_CPUS=4-15
EVENTS=task-clock,context-switches,cpu-migrations,cycles,instructions,branches,branch-misses

OUT=bench/results/$LABEL
RAW=$OUT/raw
CMDS=$RAW/commands.txt

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
for bin in "$SERVER" "$CLIENT"; do
    [ -x "$bin" ] || { echo "Missing $bin (build it first)" >&2; exit 1; }
done
if [ "$(cat /proc/sys/kernel/perf_event_paranoid)" -gt -1 ]; then
    echo "Run first: sudo sysctl kernel.perf_event_paranoid=-1" >&2; exit 1
fi
if [ "$(cat /proc/sys/kernel/kptr_restrict)" -ne 0 ]; then
    echo "Run first: sudo sysctl kernel.kptr_restrict=0" >&2; exit 1
fi
if grep -qv performance /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; then
    echo "Run first: sudo cpupower frequency-set -g performance" >&2; exit 1
fi
mkdir -p "$RAW"
printf '# %s  STEPS="%s" RUNS=%s DURATION=%s CLIENTS=%s MOVE=%s\n' "$(date -Iseconds)" \
    "$STEPS" "$RUNS" "$DURATION" "$CLIENTS" "${MOVE:-0}" >> "$CMDS"

# True if section $1 was requested in STEPS
want() { [[ " $STEPS " == *" $1 "* ]]; }

# Print a perf command to raw/commands.txt, then run it, so the report shows
# exactly what was executed
logged() {
    printf '%s\n' "$*" >> "$CMDS"
    "$@"
}

# ---------------------------------------------------------------------------
# Scenario helpers
# ---------------------------------------------------------------------------
arr_client_pids=()
int_server_pid=0
MOVE=${MOVE:-0}
if [ "$MOVE" = 1 ]; then
    export FUSE_AUTO_MOVE=1     # inherited by every client started below
    str_scenario="moving: every client holds right, down, left, up for 1 s each (FUSE_AUTO_MOVE=1),
sending ~100 input messages/s like a held arrow key"
else
    str_scenario="idle: clients join on their own
and send no input"
fi

start_scenario() {
    # stdout goes to /dev/null so terminal printing is not part of the cost
    taskset -c "$SERVER_CPUS" "$SERVER" > /dev/null &
    int_server_pid=$!
    arr_client_pids=()
    for _ in $(seq "$CLIENTS"); do
        taskset -c "$CLIENT_CPUS" "$CLIENT" > /dev/null &
        arr_client_pids+=($!)
    done
    sleep "$WARMUP"
}

stop_scenario() {
    kill "$int_server_pid" "${arr_client_pids[@]}" 2> /dev/null || true
    wait 2> /dev/null || true
    sleep 1
}
trap stop_scenario EXIT

# Files written by perf running as root are handed back to the user
own_files() {
    [ -z "$SUDO" ] || $SUDO chown "$(id -u):$(id -g)" "$@"
}

# ---------------------------------------------------------------------------
# setup.txt: machine and build (needed below every results table)
# ---------------------------------------------------------------------------
source bench/setup_info.sh
str_level=$(opt_level "$SERVER")
write_setup "$LABEL" "$SERVER" "$CLIENT" > "$OUT/setup.txt"

# ---------------------------------------------------------------------------
# 1. perf stat: server and client #1, RUNS fresh runs
# ---------------------------------------------------------------------------
if want stat; then
    rm -f "$RAW/stat_server.txt" "$RAW/stat_client.txt"
    for i in $(seq "$RUNS"); do
        echo "perf stat run $i/$RUNS"
        start_scenario
        # Normal (human-readable) perf stat output; stat_normalize reads it back
        logged perf stat --append -o "$RAW/stat_server.txt" -e "$EVENTS" \
            -p "$int_server_pid" -- sleep "$DURATION" &
        int_perf_pid=$!
        logged perf stat --append -o "$RAW/stat_client.txt" -e "$EVENTS" \
            -p "${arr_client_pids[0]}" -- sleep "$DURATION"
        wait "$int_perf_pid"
        stop_scenario
    done
fi

# ---------------------------------------------------------------------------
# 2. Lock contention (kernel lock tracepoints; Ubuntu's perf has no BPF, so
#    record first and analyse the recording afterwards)
# ---------------------------------------------------------------------------
if want lock; then
    echo "perf lock contention"
    start_scenario
    rm -f "$RAW/lock_server.data"
    logged $SUDO perf lock record -o "$RAW/lock_server.data" \
        -p "$int_server_pid" -- sleep "$DURATION" > /dev/null 2>&1 || true
    own_files "$RAW/lock_server.data"
    stop_scenario
    logged perf lock contention -i "$RAW/lock_server.data" -t \
        > "$RAW/lock_server.txt" 2>&1 || true
fi

# ---------------------------------------------------------------------------
# 3. Syscall summary per thread (futex calls show user-space mutex contention)
# ---------------------------------------------------------------------------
if want trace; then
    echo "perf trace -s"
    start_scenario
    for name in server client; do
        if [ "$name" = server ]; then pid=$int_server_pid; else pid=${arr_client_pids[0]}; fi
        logged $SUDO perf trace -s -o "$RAW/trace_$name.txt" \
            -p "$pid" -- sleep "$DURATION" > /dev/null 2>&1 || true
        own_files "$RAW/trace_$name.txt"
    done
    stop_scenario
fi

# ---------------------------------------------------------------------------
# 4. Sampled call stacks: hotspots and flame graphs. Frame-pointer unwinding
#    needs -fno-omit-frame-pointer (make baseline-O<n>, make profile).
# ---------------------------------------------------------------------------
if want record; then
    echo "perf record"
    start_scenario
    for name in server client; do
        if [ "$name" = server ]; then pid=$int_server_pid; else pid=${arr_client_pids[0]}; fi
        rm -f "$RAW/record_$name.data"
        # cpu_core/cycles/ explicitly: on this hybrid CPU a plain "cycles"
        # can make perf report show the E-core event (processes are on P-cores)
        logged perf record -q -e cpu_core/cycles/ -F "$RECORD_FREQ" --call-graph fp \
            -o "$RAW/record_$name.data" -p "$pid" -- sleep "$RECORD_DURATION"
    done
    stop_scenario
    for name in server client; do
        # Text dump of every sample with symbols, readable on any machine
        logged perf script -i "$RAW/record_$name.data" 2> /dev/null \
            | gzip > "$RAW/record_$name.script.gz"
        logged perf report -i "$RAW/record_$name.data" --stdio --no-children \
            --percent-limit 0.5 > "$RAW/report_$name.txt" 2> /dev/null
        if command -v stackcollapse-perf.pl > /dev/null; then
            zcat "$RAW/record_$name.script.gz" | stackcollapse-perf.pl \
                | flamegraph.pl --title "$LABEL $name" > "$OUT/flame_$name.svg"
        fi
    done
fi

# ---------------------------------------------------------------------------
# report.txt: method + summary, built from the raw files above
# ---------------------------------------------------------------------------
# Per-run perf stat table and mean/stddev. A new run starts at each
# "# started on" line; hybrid CPUs prefix events with cpu_core/ (counted,
# because we pin to P-cores) and cpu_atom/ (<not counted>, skipped).
stat_table() {
    awk -F, -v dur="$DURATION" '
        /^# started/ { run++ }
        $1 ~ /^[0-9.]+$/ {
            e = $3; sub(/^cpu_core\//, "", e); sub(/\/$/, "", e)
            v[run, e] = $1
        }
        # ratios = 0 for the sd row: a ratio of two stddevs means nothing
        function row(name, x, ratios) {
            printf "%-4s %10.0f %6s %9.0f %7.0f %8.2f %8.2f %5s %7s\n", name,
                x["task-clock"],
                ratios ? sprintf("%.3f", x["task-clock"] / (dur * 1000)) : "-",
                x["context-switches"], x["cpu-migrations"],
                x["cycles"] / 1e9, x["instructions"] / 1e9,
                ratios && x["cycles"] ? sprintf("%.2f", x["instructions"] / x["cycles"]) : "-",
                ratios && x["branches"] ? sprintf("%.3f", 100 * x["branch-misses"] / x["branches"]) : "-"
        }
        END {
            n = split("task-clock context-switches cpu-migrations cycles instructions branches branch-misses", ev, " ")
            printf "%-4s %10s %6s %9s %7s %8s %8s %5s %7s\n", "run", "task-ms",
                "CPUs", "ctx-sw", "migr", "Gcycles", "Ginstr", "IPC", "brmiss%"
            for (r = 1; r <= run; r++) {
                for (k = 1; k <= n; k++) x[ev[k]] = v[r, ev[k]]
                row(r, x, 1)
            }
            for (k = 1; k <= n; k++) {
                s = 0; q = 0
                for (r = 1; r <= run; r++) { s += v[r, ev[k]]; q += v[r, ev[k]] ^ 2 }
                m[ev[k]] = s / run; d = q / run - m[ev[k]] ^ 2
                sd[ev[k]] = sqrt(d > 0 ? d : 0)
            }
            row("mean", m, 1)
            row("sd", sd, 0)
        }' <(stat_normalize "$1")
}

# Per-thread syscall totals from perf trace -s, largest first
trace_table() {
    awk '
        /^ [^ ].* \([0-9]+\), [0-9]+ events/ { thread = $1 " " $2 }
        $1 ~ /^[a-z_0-9]+$/ && $2 ~ /^[0-9]+$/ && NF >= 8 && thread != "" {
            printf "%-28s %-18s %10d calls %12.1f ms\n", thread, $1, $2, $4
        }' "$1" | sort -k4 -n -r | head -15
}

{
    echo "Profiling report: $LABEL"
    echo "Generated by bench/profile.sh on $(date -Iseconds)"
    echo "Machine, build flags and optimisation level: setup.txt"
    echo "Raw perf output: raw/ (exact commands in raw/commands.txt)"
    echo
    echo "== 1. What was profiled"
    echo "Server: $SERVER   Client: $CLIENT   Level: ${str_level:-unknown}"
    echo
    echo "== 2. Scenario"
    echo "1 server + $CLIENTS clients on one machine, $str_scenario."
    echo "Server pinned to CPUs $SERVER_CPUS, clients to CPUs"
    echo "$CLIENT_CPUS (P-cores only). Program output goes to /dev/null."
    echo "Every measurement starts fresh processes, waits ${WARMUP} s (joining finishes),"
    echo "attaches perf to the process by PID, then kills everything."
    echo
    echo "== 3. Measurements"
    echo "a) perf stat: $RUNS runs x ${DURATION} s, server and client #1 counted at the"
    echo "   same time. Events: $EVENTS"
    echo "   Raw: raw/stat_server.txt, raw/stat_client.txt (perf stat's normal output,"
    echo "   one block per run)"
    echo "b) perf lock record + perf lock contention: 1 run, ${DURATION} s, server."
    echo "   Kernel lock tracepoints only: pthread mutexes in user space do not"
    echo "   appear here; their contention shows as futex calls in (c)."
    echo "   Raw: raw/lock_server.data, raw/lock_server.txt"
    echo "c) perf trace -s: 1 run, ${DURATION} s per process (server, then client #1"
    echo "   in the same scenario). Per-thread syscall counts and time."
    echo "   Raw: raw/trace_server.txt, raw/trace_client.txt"
    echo "d) perf record: 1 run, ${RECORD_DURATION} s per process at ${RECORD_FREQ} Hz,"
    echo "   frame-pointer call stacks. Raw: raw/record_*.data (open with"
    echo "   perf report -i on this machine), raw/record_*.script.gz (perf script"
    echo "   text, portable; feed to stackcollapse-perf.pl for flame graphs),"
    echo "   raw/report_*.txt (perf report --no-children)."
    echo
    echo "== 4. Results"
    for name in server client; do
        if [ -s "$RAW/stat_$name.txt" ]; then
            echo "-- perf stat, $name (per ${DURATION} s run; CPUs = task-clock / wall time)"
            stat_table "$RAW/stat_$name.txt"
            echo
        fi
    done
    if [ -s "$RAW/lock_server.txt" ]; then
        echo "-- perf lock contention, server"
        cat "$RAW/lock_server.txt"
        echo
    fi
    for name in server client; do
        if [ -s "$RAW/trace_$name.txt" ]; then
            echo "-- perf trace -s, $name: top syscalls by call count (${DURATION} s)"
            trace_table "$RAW/trace_$name.txt"
            echo
        fi
    done
    for name in server client; do
        if [ -s "$RAW/report_$name.txt" ]; then
            echo "-- perf record, $name: top functions by self time"
            grep -E '^ +[0-9]+\.[0-9]+%' "$RAW/report_$name.txt" | head -15
            echo
        fi
    done
} > "$OUT/report.txt"

echo "Done: $OUT/report.txt"
