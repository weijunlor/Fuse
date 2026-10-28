#!/usr/bin/env bash
# Measure the particle system (optimisations #5, #6, #7) with bench_particles
# and save everything for the final report, in the same layout as
# bench/profile.sh and bench/render.sh:
#
#   bench/results/<label>/
#     setup.txt    machine, build flags and optimisation level, git commit
#     report.txt   how it was measured and the results tables
#     raw/         bench output lines, perf stat text (as printed by perf), extras
#
# Usage: bench/particles.sh <label> <step>
#   make bench && bench/particles.sh opt-06-particles-soa layout
#
# Steps (each compares one optimisation with the version before it, using the
# same source code; see bench/bench_particles.c and client/particles.h):
#   layout   #6  aos vs soa, both built without SIMD (bench_particles_scalar), 1 thread,
#                with the pool full (capacity = N) and 25% full (capacity = 4 N)
#   simd     #7  soa built three ways: scalar, SSE2 (4 floats), AVX2 (8 floats), 1 thread;
#                aos -O3 -march=native for reference (it does not vectorise);
#                soa-intrin: the loop hand-written with AVX2 intrinsics (vs the compiler's);
#                soa-intrin-scan: also the dead-particle scan (which GCC cannot vectorise)
#   threads  #5  soa AVX2 build with 1..8 threads (one per physical P-core)
#
# Settings (environment variables, defaults in brackets):
#   RUNS [10] runs per configuration   SIZES ["1000 10000 100000 1000000"] particle counts
#   WORK [200000000] particle updates per run (ticks = WORK / particles, at least 300)
#   CPU [4] P-core for the 1-thread steps
#   CPUS_MT [0,2,4,6,8,10,12,14] one hardware thread per physical P-core, for "threads"
#   THREADS ["1 2 4 6 8"] thread counts for "threads"
#
# Needs perf_event_paranoid <= -1, kptr_restrict = 0 and the performance governor.

set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -lt 2 ]; then
    sed -n '/^# Usage/,/^$/p' "$0" >&2
    exit 1
fi
LABEL=$1
STEP=$2

RUNS=${RUNS:-10}
SIZES=${SIZES:-1000 10000 100000 1000000}
WORK=${WORK:-200000000}
CPU=${CPU:-4}
CPUS_MT=${CPUS_MT:-0,2,4,6,8,10,12,14}
THREADS=${THREADS:-1 2 4 6 8}
# Same base events as bench/profile.sh, plus the cache events this step is about
EVENTS=task-clock,context-switches,cpu-migrations,cycles,instructions,branches,branch-misses,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses

BIN_SCALAR=build/bench_particles_scalar
BIN_SSE2=build/bench_particles_sse2
BIN_AVX2=build/bench_particles

# Configurations of this step, one per line:
#   name binary layout threads cpus capacity-factor (capacity = factor x N)
case $STEP in
    layout)
        CONFIGS="aos-full $BIN_SCALAR aos 1 $CPU 1
soa-full $BIN_SCALAR soa 1 $CPU 1
aos-25pct $BIN_SCALAR aos 1 $CPU 4
soa-25pct $BIN_SCALAR soa 1 $CPU 4"
        BINS="$BIN_SCALAR" ;;
    simd)
        CONFIGS="aos-avx2 $BIN_AVX2 aos 1 $CPU 1
soa-scalar $BIN_SCALAR soa 1 $CPU 1
soa-sse2 $BIN_SSE2 soa 1 $CPU 1
soa-avx2 $BIN_AVX2 soa 1 $CPU 1
soa-intrin $BIN_AVX2 soa-intrin 1 $CPU 1
soa-intrin-scan $BIN_AVX2 soa-intrin-scan 1 $CPU 1"
        BINS="$BIN_SCALAR $BIN_SSE2 $BIN_AVX2" ;;
    threads)
        CONFIGS=""
        for t in $THREADS; do
            CONFIGS+="soa-avx2-t$t $BIN_AVX2 soa $t $CPUS_MT 1"$'\n'
        done
        CONFIGS=${CONFIGS%$'\n'}
        BINS="$BIN_AVX2" ;;
    *)
        echo "Unknown step '$STEP' (layout, simd or threads)" >&2; exit 1 ;;
esac

OUT=bench/results/$LABEL
RAW=$OUT/raw
CMDS=$RAW/commands.txt

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
for b in $BINS; do
    [ -x "$b" ] || { echo "Missing $b (run make bench)" >&2; exit 1; }
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
printf '# %s  STEP=%s RUNS=%s SIZES="%s" WORK=%s CPU=%s CPUS_MT=%s THREADS="%s"\n' \
    "$(date -Iseconds)" "$STEP" "$RUNS" "$SIZES" "$WORK" "$CPU" "$CPUS_MT" "$THREADS" >> "$CMDS"

# Print a command to raw/commands.txt, then run it
logged() {
    printf '%s\n' "$*" >> "$CMDS"
    "$@"
}

ticks_for() {
    local t=$((WORK / $1))
    echo $((t < 300 ? 300 : t))
}

# ---------------------------------------------------------------------------
# 1. perf stat, one process per run. Runs are interleaved (run 1 of every
#    configuration, then run 2, ...) so slow drift of the machine (heat,
#    background work) affects every configuration equally.
# ---------------------------------------------------------------------------
while read -r name bin layout threads cpus cap; do
    for n in $SIZES; do
        rm -f "$RAW/out_${name}_$n.txt" "$RAW/stat_${name}_$n.txt"
    done
done <<< "$CONFIGS"
for i in $(seq "$RUNS"); do
    for n in $SIZES; do
        while read -r name bin layout threads cpus cap; do
            echo "run $i/$RUNS: $name, $n particles"
            logged perf stat --append -o "$RAW/stat_${name}_$n.txt" -e "$EVENTS" -- \
                taskset -c "$cpus" "$bin" "$layout" "$threads" "$n" "$(ticks_for "$n")" $((cap * n)) \
                >> "$RAW/out_${name}_$n.txt"
        done <<< "$CONFIGS"
    done
done

# ---------------------------------------------------------------------------
# 2. Step extras: evidence of what the compiler or the threads did
# ---------------------------------------------------------------------------
if [ "$STEP" = simd ]; then
    # Vectoriser decisions for the particle loops (written by make bench)
    for b in $BIN_SSE2 $BIN_AVX2; do
        grep -E 'client/particles\.c:.*(optimized|missed: not vectorized)' "$b.vec.txt" \
            > "$RAW/$(basename "$b").vec.txt" || true
    done
    # The hot loop's machine code: soa_move in each build, and the
    # hand-written soa_move_avx2 (both are noinline, so each is one function)
    for b in $BINS; do
        logged objdump -d --no-show-raw-insn -C "$b" \
            | awk '/<soa_move>:/,/^$/' > "$RAW/disasm_$(basename "$b").txt"
    done
    logged objdump -d --no-show-raw-insn -C "$BIN_AVX2" \
        | awk '/<soa_move_avx2>:/,/^$/' > "$RAW/disasm_intrinsics.txt"
    # Where the time goes at 100000 particles, per build
    while read -r name bin layout threads cpus cap; do
        data=$RAW/record_$name.data
        logged perf record -q -e cpu_core/cycles/ -F 4999 -o "$data" -- \
            taskset -c "$cpus" "$bin" "$layout" "$threads" 100000 "$(ticks_for 100000)" > /dev/null
        logged perf report -i "$data" --stdio --no-children --percent-limit 1 \
            > "$RAW/report_$name.txt" 2> /dev/null
        sym=soa_move
        [ "${layout#soa-intrin}" != "$layout" ] && sym=soa_move_avx2
        [ "$layout" = aos ] && sym=particles_update
        logged perf annotate -i "$data" --stdio -s "$sym" \
            > "$RAW/annotate_$name.txt" 2> /dev/null || true
    done <<< "$CONFIGS"
fi

# ---------------------------------------------------------------------------
# setup.txt and report.txt
# ---------------------------------------------------------------------------
source bench/setup_info.sh
# shellcheck disable=SC2086
write_setup "$LABEL" $BINS > "$OUT/setup.txt"

# Normalised perf stat ("value,,event" lines) next to each raw file, for the parser
while read -r name bin layout threads cpus cap; do
    for n in $SIZES; do
        stat_normalize "$RAW/stat_${name}_$n.txt" > "$RAW/.norm_${name}_$n.txt"
    done
done <<< "$CONFIGS"

{
    echo "Particle report: $LABEL (step: $STEP)"
    echo "Generated by bench/particles.sh on $(date -Iseconds)"
    echo "Machine, build flags: setup.txt. Raw output: raw/ (commands in raw/commands.txt)"
    echo
    echo "== Method"
    echo "bench_particles runs the client's particle code (client/particles.c) without a window."
    echo "Every tick (dt = 1/60 s) it updates every particle, then respawns bursts until"
    echo "the count is back to N (steady state: ~1 in 75 particles dies and is replaced"
    echo "per tick); 120 untimed warm-up ticks first. Each run does about $WORK particle"
    echo "updates (ticks = $WORK / N, at least 300). Every configuration gets the same"
    echo "random numbers, and the checksum column shows they end with the same particles."
    echo "$RUNS runs per configuration and N, interleaved; one process per run under"
    echo "perf stat, pinned with taskset (CPUs column). Hardware counters count the whole"
    echo "process (set-up, warm-up and the timed ticks)."
    case $STEP in
        layout)
            echo "Step #6 (cache, branches): aos = array of 32-byte structs with an alive flag,"
            echo "holes left by dead particles, free-slot search on spawn; soa = separate arrays,"
            echo "live particles packed, swap-remove. Both from bench_particles_scalar"
            echo "(-O2 -fno-tree-vectorize), so the layout is the only difference."
            echo "full: pool capacity = N (always full). 25pct: capacity = 4 N, i.e. the pool is"
            echo "sized for 4x the usual load (several explosions at once) and is 25% full;"
            echo "aos walks all 4 N slots every tick, soa only the N live particles." ;;
        simd)
            echo "Step #7 (DLP): the same soa code built three ways: scalar (-O2"
            echo "-fno-tree-vectorize), sse2 (-O3 -fno-trapping-math, 128-bit, 4 floats),"
            echo "avx2 (-O3 -march=native -fno-trapping-math, 256-bit, 8 floats). aos-avx2 is"
            echo "the aos code in the AVX2 build: its loop has the alive branch, so it is not"
            echo "vectorised (see raw/bench_particles.vec.txt)."
            echo "Without -fno-trapping-math GCC does not vectorise the soa loop either"
            echo "(\"unsupported control flow\": the clamps stay branches)."
            echo "soa-intrin = the same loop written by hand with AVX2/FMA intrinsics"
            echo "(soa_move_avx2), in the AVX2 build, to compare with the compiler's own AVX2"
            echo "code (soa-avx2). soa-intrin-scan also replaces the dead-particle scan"
            echo "(soa_remove_chunk, which GCC cannot vectorise) with an AVX2 version that tests"
            echo "8 lives per compare + movemask (soa_remove_chunk_avx2). All three give"
            echo "bit-identical particles." ;;
        threads)
            echo "Step #5 (TLP): soa AVX2 build, update split over a worker pool (common/"
            echo "worker_pool.c): cache-line-aligned chunks, two barrier waits per tick, dead"
            echo "particles removed inside each chunk in parallel, then a short serial step"
            echo "closes the gaps between chunks. Respawning is serial. One thread per physical"
            echo "P-core (CPUs $CPUS_MT; no hyper-threading siblings)." ;;
    esac
    echo
    python3 - "$RAW" "$STEP" "$SIZES" "$CONFIGS" <<'PYEOF'
import re, statistics, sys

raw, step, sizes, configs = sys.argv[1], sys.argv[2], sys.argv[3].split(), sys.argv[4]
configs = [line.split() for line in configs.strip().splitlines()]

def mean_sd(xs):
    return (statistics.mean(xs), statistics.stdev(xs) if len(xs) > 1 else 0.0)

def load(name, n):
    """Per-run bench lines and perf counters for one configuration and size"""
    outs = [dict(re.findall(r'(\w+)=(\S+)', l)) for l in open(f'{raw}/out_{name}_{n}.txt')]
    runs, cur = [], None
    for line in open(f'{raw}/.norm_{name}_{n}.txt'):
        if line.startswith('# started'):
            cur = {}; runs.append(cur); continue
        v, _, ev = line.strip().split(',')[:3]
        ev = ev.replace('cpu_core/', '').rstrip('/')
        if cur is not None and not ev.startswith('cpu_atom'):
            cur[ev] = float(v)
    return outs, runs

rows = {}
for name, *_ in configs:
    for n in sizes:
        outs, runs = load(name, n)
        work = [int(o['ticks']) * int(o['particles']) for o in outs]
        r = {
            'ns': mean_sd([float(o['ns_per_particle']) for o in outs]),
            'ns_runs': [float(o['ns_per_particle']) for o in outs],
            'checksum': sorted({o['checksum'] for o in outs}),
            'threads': outs[0]['threads'],
        }
        def per(ev, scale=1.0):
            vals = [run[ev] / w * scale for run, w in zip(runs, work) if ev in run]
            return mean_sd(vals) if vals else None
        def ratio(a, b, scale=1.0):
            vals = [run[a] / run[b] * scale for run in runs if a in run and b in run and run[b] > 0]
            return mean_sd(vals) if vals else None
        r['instr'] = per('instructions')
        r['ipc'] = ratio('instructions', 'cycles')
        r['brmiss'] = ratio('branch-misses', 'branches', 100)
        r['brmiss_pp'] = per('branch-misses')
        r['l1'] = per('L1-dcache-load-misses')
        r['l1pct'] = ratio('L1-dcache-load-misses', 'L1-dcache-loads', 100)
        r['llc'] = per('LLC-load-misses', 1000)
        rows[(name, n)] = r

def fmt(ms, d=2):
    return '-' if ms is None else f'{ms[0]:.{d}f} ± {ms[1]:.{d}f}'

print('== Time per particle update (ns, mean ± sd over runs; lower is better)')
print('   Timed ticks only (bench\'s own clock): update + respawn.')
hdr = f"{'config':<16} {'thr':>3}" + ''.join(f'{"N=" + n:>18}' for n in sizes)
print(hdr)
for name, *_ in configs:
    line = f"{name:<16} {rows[(name, sizes[0])]['threads']:>3}"
    line += ''.join(f"{fmt(rows[(name, n)]['ns'], 3):>18}" for n in sizes)
    print(line)
print()

base = configs[0][0]
print(f'== Speed-up vs {base} (ratio of mean time per particle)')
print(hdr)
for name, *_ in configs:
    line = f"{name:<16} {rows[(name, sizes[0])]['threads']:>3}"
    line += ''.join(f"{rows[(base, n)]['ns'][0] / rows[(name, n)]['ns'][0]:>17.2f}x" for n in sizes)
    print(line)
print()

print('== Hardware counters (whole process, per particle update)')
print(f"{'config':<16} {'N':>8} {'instr':>14} {'IPC':>12} {'br-miss %':>12} {'br-miss':>14}"
      f" {'L1d miss':>14} {'L1d miss %':>12} {'LLC miss/1000':>16}")
for n in sizes:
    for name, *_ in configs:
        r = rows[(name, n)]
        print(f"{name:<16} {n:>8} {fmt(r['instr']):>14} {fmt(r['ipc']):>12} {fmt(r['brmiss']):>12}"
              f" {fmt(r['brmiss_pp'], 4):>14} {fmt(r['l1'], 3):>14} {fmt(r['l1pct']):>12} {fmt(r['llc'], 3):>16}")
    print()

print('== Per-run time per particle (ns), to show outliers or bimodal runs')
for n in sizes:
    for name, *_ in configs:
        print(f"{name:<16} {n:>8}  " + ' '.join(f'{x:.3f}' for x in rows[(name, n)]['ns_runs']))
print()

print('== Checksums (must be equal within each N, up to the last digits)')
for n in sizes:
    print(f'N={n}: ' + '  '.join(f"{name}={'/'.join(rows[(name, n)]['checksum'])}" for name, *_ in configs))
PYEOF
    if [ "$STEP" = simd ]; then
        echo
        echo "== Vectoriser report for the two update loops (-fopt-info-vec; all loops in raw/*.vec.txt)"
        # Source lines of the loops: aos_update's walk and soa_move's loop
        line_aos=$(awk '/^static void aos_update/ { f = 1 } f && /for \(/ { print NR; exit }' client/particles.c)
        line_soa=$(awk '/^static void soa_move/ { f = 1 } f && /for \(/ { print NR; exit }' client/particles.c)
        for b in $BIN_SSE2 $BIN_AVX2; do
            echo "-- $(basename "$b")"
            grep -E "particles\.c:($line_aos|$line_soa):" "$RAW/$(basename "$b").vec.txt" \
                | sed -e "s/:$line_aos:[0-9]*:/ (aos_update loop):/" -e "s/:$line_soa:[0-9]*:/ (soa_move loop):/" \
                      -e 's/^/   /' | sort -u
        done
        echo
        echo "== Instructions in the update loop function (static count, whole function)"
        printf "%-36s %8s %8s %8s %8s\n" function total ymm xmm "vmulps+vfmadd"
        for f in "$RAW"/disasm_*.txt; do
            tag=$(basename "$f" .txt | sed 's/^disasm_//')
            case $tag in intrinsics) tag="soa_move_avx2 (bench_particles)" ;; *) tag="soa_move ($tag)" ;; esac
            printf "%-36s %8s %8s %8s %8s\n" "$tag" "$(grep -c '^ ' "$f")" \
                "$(grep -c ymm "$f")" "$(grep -c xmm "$f")" "$(grep -cE 'vmulps|vfmadd...ps|mulps' "$f")"
        done
        echo
        echo "== Main 8-wide loop body: compiler (soa_move, AVX2 build) vs hand-written (soa_move_avx2)"
        echo "   (instructions between the loop label and its backward jump; full listings in raw/disasm_*.txt)"
        for f in "$RAW/disasm_bench_particles.txt" "$RAW/disasm_intrinsics.txt"; do
            echo "-- $(basename "$f")"
            # The 8-wide loop is the one containing vfmadd on ymm registers:
            # print from the jump target of the first backward jump around it
            python3 - "$f" <<'PY2'
import re, sys
lines = [l.rstrip() for l in open(sys.argv[1]) if re.match(r'\s+[0-9a-f]+:', l)]
addr = [int(l.split(':')[0], 16) for l in lines]
for k, l in enumerate(lines):
    m = re.search(r'\sj\w+\s+([0-9a-f]+)', l)
    if m:
        tgt = int(m.group(1), 16)
        body = [x for x, a in zip(lines, addr) if tgt <= a <= addr[k]]
        if tgt < addr[k] and any('ymm' in x and 'vfmadd' in x for x in body):
            print('\n'.join('   ' + x.strip() for x in body))
            print(f'   ({len(body)} instructions per 8 particles)')
            break
PY2
        done
        echo
        echo "== perf record at 100000 particles: top functions per configuration"
        for f in "$RAW"/report_*.txt; do
            echo "-- $(basename "$f" .txt | sed 's/^report_//')"
            grep -E '^ +[0-9]' "$f" | head -6
        done
    fi
} > "$OUT/report.txt"
rm -f "$RAW"/.norm_*.txt

echo "Done: $OUT/report.txt"
