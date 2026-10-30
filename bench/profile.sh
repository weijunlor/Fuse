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
#                  perf script dumps, perf trace / perf lock output, network sends
#
# Usage: bench/profile.sh <label> <bin_dir>
#   <bin_dir> contains the programs "server" and "client", e.g.
#     make baseline-O0 && bench/profile.sh baseline-O0 build/baseline-O0
#     make profile     && bench/profile.sh opt-01-unlock-before-publish build
#
# Settings (environment variables, defaults in brackets):
#   RUNS [10]  DURATION [30] seconds  CLIENTS [4]  WARMUP [5] seconds
#   RECORD_DURATION [10] seconds of perf record per process
#   STEPS ["stat lock trace record net"]  sections to run
#   SERVER_ARGS [""]  extra server options, e.g. "--snapshot-hz 10"
#   CLIENT_ARGS [""]  extra client options, e.g. "--input-send every-10ms"
#   FUSE_QOS [unset]  "default" = Cyclone's default QoS in server and clients
#                     (the before of #14); read by the game itself, recorded here
#   MOVE [0]  1 = every client moves on its own (FUSE_AUTO_MOVE=1, client/input.c:
#             holds right, down, left, up for 1 s each, like a held arrow key,
#             ~100 input messages/s per client); 0 = idle clients
#   REPORT_ONLY [0]  1 = only rebuild report.txt from the raw files already in
#             bench/results/<label>/ (no profiling, setup.txt left alone)
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
STEPS=${STEPS:-stat lock trace record net}
SERVER_ARGS=${SERVER_ARGS:-}
CLIENT_ARGS=${CLIENT_ARGS:-}
SUDO=${SUDO-sudo}
SERVER_CPUS=0-3
CLIENT_CPUS=4-15
EVENTS=task-clock,context-switches,cpu-migrations,cycles,instructions,branches,branch-misses
source bench/security_env.sh
dds_check_clients "$CLIENTS"

OUT=bench/results/$LABEL
RAW=$OUT/raw
CMDS=$RAW/commands.txt

# ---------------------------------------------------------------------------
# Preconditions (not needed when only rebuilding the report)
# ---------------------------------------------------------------------------
REPORT_ONLY=${REPORT_ONLY:-0}
if [ "$REPORT_ONLY" = 1 ]; then
    STEPS=""
    [ -f "$OUT/report.txt" ] || { echo "No $OUT/report.txt to rebuild" >&2; exit 1; }
else
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
fi
mkdir -p "$RAW"
[ "$REPORT_ONLY" = 1 ] || printf '# %s  STEPS="%s" RUNS=%s DURATION=%s CLIENTS=%s MOVE=%s SERVER_ARGS="%s" CLIENT_ARGS="%s" FUSE_QOS="%s"\n' \
    "$(date -Iseconds)" "$STEPS" "$RUNS" "$DURATION" "$CLIENTS" "${MOVE:-0}" "$SERVER_ARGS" "$CLIENT_ARGS" \
    "${FUSE_QOS:-}" >> "$CMDS"

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
    # SERVER_ARGS unquoted on purpose: it may hold several words
    # shellcheck disable=SC2086
    dds_run server taskset -c "$SERVER_CPUS" "$SERVER" $SERVER_ARGS > /dev/null &
    int_server_pid=$!
    arr_client_pids=()
    for int_client in $(seq "$CLIENTS"); do
        # shellcheck disable=SC2086
        dds_run "client-$int_client" taskset -c "$CLIENT_CPUS" "$CLIENT" $CLIENT_ARGS > /dev/null &
        arr_client_pids+=($!)
    done
    sleep "$WARMUP"
}

stop_scenario() {
    # Nothing started yet (pid 0 would make kill signal our own process group)
    [ "$int_server_pid" -ne 0 ] || return 0
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
if [ "$REPORT_ONLY" = 1 ]; then
    # The binaries may be gone: keep the level from the old report
    str_level=$(sed -n 's/.*Level: //p' "$OUT/report.txt" | head -1)
else
    str_level=$(opt_level "$SERVER")
fi
[ "$REPORT_ONLY" = 1 ] || write_setup "$LABEL" "$SERVER" "$CLIENT" > "$OUT/setup.txt"

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
# 5. Network sends: every send syscall of the server and of client #1, with
#    its size (the return value), counted at the same time. DDS sends one UDP
#    datagram per sample per remote reader (unicast on this machine), so this
#    shows how many messages and bytes each program puts on the network.
# ---------------------------------------------------------------------------
# File descriptors of process $1 that are UDP sockets (DDS traffic), one line.
# The client also sends on a Unix socket to the Wayland compositor (from the
# GL thread, ~120 messages/s); those are not network messages.
udp_fds() {
    local f l str_ino
    for f in /proc/"$1"/fd/*; do
        l=$(readlink "$f" 2> /dev/null) || continue
        [[ $l == socket:* ]] || continue
        str_ino=${l//[^0-9]/}
        if awk -v i="$str_ino" '$10 == i { found = 1 } END { exit !found }' /proc/net/udp /proc/net/udp6; then
            printf '%s ' "${f##*/}"
        fi
    done
    echo
}

if want net; then
    echo "perf trace: network sends"
    start_scenario
    udp_fds "$int_server_pid" > "$RAW/net_server_udpfds.txt"
    udp_fds "${arr_client_pids[0]}" > "$RAW/net_client_udpfds.txt"
    logged $SUDO perf trace -e sendmsg,sendto -o "$RAW/net_server.txt" \
        -p "$int_server_pid" -- sleep "$DURATION" > /dev/null 2>&1 &
    int_perf_pid=$!
    logged $SUDO perf trace -e sendmsg,sendto -o "$RAW/net_client.txt" \
        -p "${arr_client_pids[0]}" -- sleep "$DURATION" > /dev/null 2>&1 || true
    wait "$int_perf_pid" || true
    own_files "$RAW/net_server.txt" "$RAW/net_client.txt"
    stop_scenario
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

# Sends per second and bytes per second from a perf trace -e sendmsg,sendto
# log, per thread and in total. A line looks like
#   16.666 ( 0.002 ms): server/161310 sendmsg(fd: 6, msg: 0x...)   = 224
# (thread name/TID, syscall, fd, and the bytes sent after "=").
# Only UDP sockets count: their fds are in raw/net_<name>_udpfds.txt (saved
# while tracing). Runs from before that file existed use the fds that DDS's
# "tev" thread sends on (it only sends DDS traffic). Sends on other sockets
# (the client's Wayland connection) are shown on a separate line.
net_table() {
    local str_fds=""
    [ -f "$2" ] && str_fds=$(cat "$2")
    awk -v dur="$DURATION" -v fds="$str_fds" '
        BEGIN { nf = split(fds, a, " "); for (k = 1; k <= nf; k++) udp[a[k]] = 1 }
        / = [0-9]+$/ {
            for (i = 1; i <= NF; i++) if ($i ~ /^send(msg|to)\(/) break
            if (i > NF) next
            t = $(i - 1); sub(/\/[0-9]+$/, "", t)
            fd = $(i + 1); sub(/,$/, "", fd)
            c++; T[c] = t; F[c] = fd; S[c] = $NF
            if (nf == 0 && t == "tev") udp[fd] = 1
        }
        END {
            for (k = 1; k <= c; k++) {
                if (F[k] in udp) { n[T[k]]++; b[T[k]] += S[k]; N++; B += S[k] }
                else { on++; ob += S[k] }
            }
            printf "%-16s %10s %12s %10s\n", "thread", "sends/s", "bytes/s", "bytes/send"
            for (t in n)
                printf "%-16s %10.1f %12.0f %10.1f\n", t, n[t] / dur, b[t] / dur, b[t] / n[t]
            if (N) printf "%-16s %10.1f %12.0f %10.1f\n", "total UDP", N / dur, B / dur, B / N
            if (on) printf "(not counted: %.1f sends/s on other sockets, e.g. Wayland)\n", on / dur
            if (nf == 0) print "(UDP sockets = the fds DDS thread tev sends on; no udpfds file)"
        }' "$1"
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
    [ -z "$SERVER_ARGS" ] || echo "Server options: $SERVER_ARGS"
    [ -z "$CLIENT_ARGS" ] || echo "Client options: $CLIENT_ARGS"
    [ -z "${FUSE_QOS:-}" ] || echo "Environment: FUSE_QOS=$FUSE_QOS"
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
    echo "e) perf trace -e sendmsg,sendto: 1 run, ${DURATION} s, server and client #1"
    echo "   at the same time; every UDP send and its size. Raw: raw/net_*.txt"
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
        if [ -s "$RAW/net_$name.txt" ]; then
            echo "-- network sends, $name (${DURATION} s; UDP datagrams and bytes per second)"
            net_table "$RAW/net_$name.txt" "$RAW/net_${name}_udpfds.txt"
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
