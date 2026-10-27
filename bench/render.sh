#!/usr/bin/env bash
# Measure client rendering cost (optimisation #16, batched drawing) and save
# everything for the final report, in the same layout as bench/profile.sh:
#
#   bench/results/<label>/
#     setup.txt    machine, OS, GL renderer, build flags and optimisation level, git commit
#     report.txt   how it was measured and the results table
#     raw/         client frame-time lines, perf stat text (as printed by perf), perf.data, perf script dumps
#
# Usage: bench/render.sh <label> [bin_dir]      (bin_dir default: build)
#   make profile && bench/render.sh opt-16b-short-vertices build
#
# The client runs alone (no server), with --stress N extra animated sprites and
# --frames F, then prints the CPU time spent in render_frame() per frame
# (mean, p50, p99, max). Every drawing mode comes from the same binary
# (--render <mode>, see client/render.h), so each step of #16 is measured
# against the step before it in the same session:
#   immediate  one glBegin/glEnd per sprite             (#16a before)
#   float      one glDrawArrays, 16-byte vertices        (#16a after, #16b before)
#   short      one glDrawArrays, 8-byte vertices         (#16b after)
#
# Settings (environment variables, defaults in brackets):
#   RUNS [5] runs per (mode, N)   FRAMES [1200]   STRESS ["0 1000 10000"]
#   CPUS [4-7] P-cores the client is pinned to
#   MODES ["immediate float short"] drawing modes to compare
#   SOURCE_REF [none] tag/commit the binary was built from, if not the checkout
#
# Binaries from before #16b (tag opt-16a-batched-sprites) only have
# --render-immediate; for those the modes are "immediate batched" (batched =
# what is now called float), detected automatically.
#   RECORD_STRESS [10000] sprite count used for perf record
#
# Needs perf_event_paranoid <= -1, kptr_restrict = 0, performance governor,
# and a desktop session (the client opens a window).

set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -lt 1 ]; then
    sed -n '/^# Usage/,/^$/p' "$0" >&2
    exit 1
fi
LABEL=$1
BIN_DIR=${2:-build}
CLIENT=$BIN_DIR/client

RUNS=${RUNS:-5}
FRAMES=${FRAMES:-1200}
STRESS=${STRESS:-0 1000 10000}
CPUS=${CPUS:-4-7}
RECORD_STRESS=${RECORD_STRESS:-10000}
# Same events as bench/profile.sh
EVENTS=task-clock,context-switches,cpu-migrations,cycles,instructions,branches,branch-misses

# Older client: one switch (--render-immediate) instead of --render <mode>
b_legacy=false
if grep -q -a -- '--render-immediate' "$CLIENT"; then
    b_legacy=true
    MODES=${MODES:-immediate batched}
fi
MODES=${MODES:-immediate float short}

OUT=bench/results/$LABEL
RAW=$OUT/raw
CMDS=$RAW/commands.txt

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
[ -x "$CLIENT" ] || { echo "Missing $CLIENT (run make profile)" >&2; exit 1; }
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
printf '# %s  RUNS=%s FRAMES=%s STRESS="%s" CPUS=%s MODES="%s"\n' "$(date -Iseconds)" \
    "$RUNS" "$FRAMES" "$STRESS" "$CPUS" "$MODES" >> "$CMDS"

# Client arguments for one drawing mode
mode_args() {
    if ! $b_legacy; then
        echo "--render $1"
    elif [ "$1" = immediate ]; then
        echo "--render-immediate"
    fi
}

# Print a command to raw/commands.txt, then run it
logged() {
    printf '%s\n' "$*" >> "$CMDS"
    "$@"
}

# ---------------------------------------------------------------------------
# 1. Frame times + perf stat. Runs are interleaved (run 1 of every mode and N,
#    then run 2, ...) so slow drift of the machine affects both modes equally.
# ---------------------------------------------------------------------------
for mode in $MODES; do
    for n in $STRESS; do
        rm -f "$RAW/frames_${mode}_$n.txt" "$RAW/stat_${mode}_$n.txt"
    done
done
for i in $(seq "$RUNS"); do
    for n in $STRESS; do
        for mode in $MODES; do
            echo "run $i/$RUNS: $mode, $n sprites"
            # -D 3000: start counting after 3 s, when start-up (zink/Vulkan
            # initialisation alone is ~2 billion instructions) is over
            # Normal (human-readable) perf stat output; stat_normalize reads it back
            # shellcheck disable=SC2046
            logged perf stat --append -D 3000 -o "$RAW/stat_${mode}_$n.txt" -e "$EVENTS" -- \
                taskset -c "$CPUS" "$CLIENT" --stress "$n" --frames "$FRAMES" $(mode_args "$mode") \
                | grep '^frames=' >> "$RAW/frames_${mode}_$n.txt"
        done
    done
done

# ---------------------------------------------------------------------------
# 2. Where the time goes: sampled call stacks at RECORD_STRESS sprites
# ---------------------------------------------------------------------------
for mode in $MODES; do
    echo "perf record: $mode"
    data=$RAW/record_$mode.data
    rm -f "$data"
    # shellcheck disable=SC2046
    # cpu_core/cycles/: on this hybrid CPU a plain "cycles" records P-core and
    # E-core events separately, and perf report shows the (empty) E-core one
    # -D 3000: skip start-up, as for perf stat
    logged perf record -q -e cpu_core/cycles/ -F 999 -D 3000 --call-graph fp -o "$data" -- \
        taskset -c "$CPUS" "$CLIENT" --stress "$RECORD_STRESS" --frames "$FRAMES" $(mode_args "$mode") \
        > /dev/null
    logged perf script -i "$data" 2> /dev/null | gzip > "$RAW/record_$mode.script.gz"
    logged perf report -i "$data" --stdio --no-children --percent-limit 0.5 \
        > "$RAW/report_$mode.txt" 2> /dev/null
    # Per thread and library: Mesa's libraries have no symbols on Ubuntu, so
    # "how much time is in the GL driver" is clearest at this level
    logged perf report -i "$data" --stdio --no-children --sort comm,dso -g none \
        --percent-limit 1 > "$RAW/report_${mode}_dso.txt" 2> /dev/null
done

# ---------------------------------------------------------------------------
# setup.txt and report.txt
# ---------------------------------------------------------------------------
source bench/setup_info.sh
STR_GL_RENDERER=$(grep -m1 -o 'gl_renderer="[^"]*"' "$RAW"/frames_*.txt | head -1 | cut -d'"' -f2)
export STR_GL_RENDERER
write_setup "$LABEL" "$CLIENT" > "$OUT/setup.txt"

# Value of key=... in a frames line
field() { grep -o "$1=[0-9.]*" | cut -d= -f2; }

# mean and stddev of the numbers on stdin
mean_sd() {
    awk '{ s += $1; q += $1 * $1; n++ }
         END { m = s / n; v = q / n - m * m; printf "%.1f ± %.1f", m, sqrt(v > 0 ? v : 0) }'
}

{
    echo "Rendering report: $LABEL"
    echo "Generated by bench/render.sh on $(date -Iseconds)"
    echo "Machine, GL renderer, build flags: setup.txt. Raw output: raw/ (commands in raw/commands.txt)"
    echo
    echo "== Method"
    echo "Client alone (no server), pinned to CPUs $CPUS, vsync on (glfwSwapInterval(1))."
    echo "Each run draws the floor (130 tiles) + N animated stress sprites for $FRAMES frames,"
    echo "then prints the CPU time of render_frame() per frame (clock_gettime around the"
    echo "call; GPU work and buffer swaps are not included)."
    echo "Modes (--render <mode>): immediate = one glBegin/glEnd per sprite;"
    echo "float = one vertex array and one glDrawArrays per frame, 16-byte vertices;"
    echo "short = as float with 8-byte vertices (shorts)."
    echo "(Binaries before #16b call the float mode \"batched\" and select immediate"
    echo "with --render-immediate.) Modes in this run: $MODES"
    echo "$RUNS runs per mode and N, interleaved. perf stat counts the whole process"
    echo "(all threads, including the GL driver's) from 3 s after start, so start-up is excluded."
    echo
    echo "== Frame time (render_frame CPU time, microseconds; mean ± sd over $RUNS runs)"
    printf "%-10s %7s %18s %18s %18s %10s %16s\n" mode sprites mean p50 p99 fps "instr/frame"
    for n in $STRESS; do
        for mode in $MODES; do
            f=$RAW/frames_${mode}_$n.txt
            fps=$(field fps < "$f" | mean_sd | cut -d' ' -f1)
            # Instructions per frame, per run, then averaged. perf counts from
            # 3 s after start, so the frames it saw = fps x (elapsed_s - 3).
            # Run i of the perf stat file (blocks start with "# started") pairs with line i of $f.
            instr=$(awk -F, '
                NR == FNR {
                    match($0, /fps=[0-9.]+/);       fr[NR] = substr($0, RSTART + 4, RLENGTH - 4) + 0
                    match($0, /elapsed_s=[0-9.]+/); el[NR] = substr($0, RSTART + 10, RLENGTH - 10) + 0
                    next
                }
                /^# started/ { run++ }
                $3 ~ /instructions/ && $1 ~ /^[0-9]+$/ && el[run] > 3 {
                    s += $1 / (fr[run] * (el[run] - 3)); n++
                }
                END { if (n) printf "%.0f", s / n }' "$f" <(stat_normalize "$RAW/stat_${mode}_$n.txt"))
            printf "%-10s %7s %18s %18s %18s %10s %16s\n" "$mode" "$n" \
                "$(field render_us_mean < "$f" | mean_sd)" \
                "$(field render_us_p50 < "$f" | mean_sd)" \
                "$(field render_us_p99 < "$f" | mean_sd)" \
                "$fps" \
                "${instr:-n/a}"
        done
    done
    echo
    echo "instr/frame = instructions counted after the first 3 s / (fps x (elapsed_s - 3)),"
    echo "per run, averaged; all threads of the process (application + GL driver)."
    echo
    for mode in $MODES; do
        echo "== perf record, $mode, $RECORD_STRESS sprites: samples by thread and library"
        grep -m1 '^# Samples' "$RAW/report_${mode}_dso.txt"
        grep -E '^ +[0-9]+\.[0-9]+%' "$RAW/report_${mode}_dso.txt" | head -12
        echo
        echo "-- top functions by self time (Mesa has no symbols: addresses only)"
        grep -E '^ +[0-9]+\.[0-9]+%' "$RAW/report_$mode.txt" | head -10
        echo
    done
} > "$OUT/report.txt"

echo "Done: $OUT/report.txt"
cat "$OUT/report.txt" | sed -n '/== Frame time/,/^instr/p'
