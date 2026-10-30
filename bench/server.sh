#!/usr/bin/env bash
# Isolated #8/#9/#10 measurements. Defaults to hardware perf counters; the
# explicit METRICS=time mode is for smoke checks where counters are absent.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $# -ne 2 || ! $1 =~ ^[a-zA-Z0-9_-]+$ ]]; then
    echo "Usage: bash bench/server.sh <new-label> <collision|branches|reduce>" >&2
    exit 1
fi
label=$1
step=$2
runs=${RUNS:-10}
metrics=${METRICS:-perf}
[[ $runs =~ ^[1-9][0-9]*$ ]] || { echo "RUNS must be positive" >&2; exit 1; }
case "$step" in
    collision)
        binary=build/bench_collision
        modes=(brute grid)
        args=("${EMBERS:-2048}" "${QUERIES:-100000}" "${TICKS:-100}")
        events=cycles,instructions,L1-dcache-load-misses,LLC-load-misses
        ;;
    branches)
        binary=build/bench_branches
        modes=(branchy branchless)
        args=("${QUERIES:-10000000}")
        events=cycles,instructions,branches,branch-misses
        ;;
    reduce)
        binary=build/bench_reduce
        modes=(serial accum)
        args=("${VALUES:-1000000}" "${ROUNDS:-100}")
        events=cycles,instructions
        ;;
    *) echo "Unknown step: $step" >&2; exit 1 ;;
esac
[[ $metrics == perf || $metrics == time ]] || { echo "METRICS must be perf or time" >&2; exit 1; }
if [[ $metrics == perf ]]; then
    command -v perf >/dev/null || { echo "perf is required; use METRICS=time only for smoke checks" >&2; exit 1; }
    [[ -n ${CPUSET:-} ]] || { echo "Set CPUSET to fixed P-cores for perf measurements" >&2; exit 1; }
fi
make -B "$binary"
out="bench/results/$label"
# Refuse to overwrite previous evidence.
mkdir "$out"
mkdir "$out/raw"
{
    echo "Step: $step; metrics: $metrics; runs: $runs; CPUSET: ${CPUSET:-unrestricted}"
    echo "Recorded: $(date -Iseconds)"
    echo "Git: $(git rev-parse HEAD)"
    git status --short
    uname -a
    lscpu
    gcc --version | head -1
    echo "Build command (same Makefile and options used immediately above):"
    make -n -B "$binary"
    sha256sum "$binary"
    echo "Governor: $(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u || true)"
    echo "No governor, sysctl or privilege changes are made by this script."
    echo "METRICS=time is timing/checksum evidence only, without cache/branch/IPC claims."
} > "$out/setup.txt"
runner=()
if [[ -n ${CPUSET:-} ]]; then runner=(taskset -c "$CPUSET"); fi
for mode in "${modes[@]}"; do
    command=("${runner[@]}" "./$binary" "$mode" "${args[@]}")
    printf '%q ' "${command[@]}" >> "$out/raw/commands.txt"
    printf '\n' >> "$out/raw/commands.txt"
    if [[ $metrics == perf ]]; then
        # Failure leaves the actual error in raw/ and returns a failure;
        # unsupported counters are never silently replaced with timings.
        perf stat -r "$runs" -x, -e "$events" -o "$out/raw/stat_$mode.csv" \
            -- "${command[@]}" > "$out/raw/$mode.txt" 2> "$out/raw/$mode.stderr"
        if grep -Eq '<not (supported|counted)>' "$out/raw/stat_$mode.csv"; then
            echo "A required perf counter is unavailable; see $out/raw/stat_$mode.csv" >&2
            exit 1
        fi
    else
        for ((r=0; r<runs; r++)); do
            "${command[@]}" >> "$out/raw/$mode.txt"
        done
    fi
done
python3 - "$out" "$metrics" <<'PY'
import csv, pathlib, re, statistics, sys
out = pathlib.Path(sys.argv[1])
checksums = set()
rows = []
for path in sorted((out / "raw").glob("*.txt")):
    if path.name == "commands.txt":
        continue
    text = path.read_text()
    values = [float(x) for x in re.findall(r"ns_per_(?:query|value)=([\d.]+)", text)]
    checksums.update(re.findall(r"checksum=(\d+)", text))
    if not values:
        raise SystemExit(f"No benchmark readings in {path}")
    rows.append(f"{path.stem}: {len(values)} runs, mean={statistics.mean(values):.3f} ns, "
                f"stddev={statistics.stdev(values) if len(values) > 1 else 0:.3f} ns")
    counter_file = out / "raw" / f"stat_{path.stem}.csv"
    if counter_file.exists():
        counters = {}
        for fields in csv.reader(counter_file.read_text().splitlines()):
            if len(fields) < 3:
                continue
            try:
                value = float(fields[0])
            except ValueError:
                continue
            event = fields[2].split(":")[0]
            counters[event] = counters.get(event, 0) + value
        rows.append("  mean counters: " + ", ".join(f"{k}={v:.0f}" for k, v in counters.items()))
        if counters.get("cycles", 0):
            rows.append(f"  IPC={counters.get('instructions', 0) / counters['cycles']:.3f}")
        if counters.get("branches", 0):
            rows.append(f"  branch misses={100 * counters.get('branch-misses', 0) / counters['branches']:.3f}%")
if len(checksums) != 1:
    raise SystemExit(f"Benchmark checksums do not agree: {checksums}")
report = "\n".join([f"Metrics: {sys.argv[2]}", *rows,
                    f"All checksums match: {next(iter(checksums))}",
                    "Raw commands, timings and available perf counters are in raw/.",
                    "Compare only runs from the same machine; timing alone does not prove IPC/cache/branch changes."])
(out / "report.txt").write_text(report + "\n")
print(report)
PY
