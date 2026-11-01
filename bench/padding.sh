#!/usr/bin/env bash
# #11: interleaved compact/padded runs on two explicitly selected CPUs.
# PERF=0 permits a timing-only smoke check where hardware counters are unavailable.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$#" -ne 3 ]; then
    echo 'Usage: bash bench/padding.sh <label> <producer_cpu> <consumer_cpu>' >&2
    exit 1
fi
str_label=$1
if [[ ! "$str_label" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]]; then
    echo 'Label must contain only letters, digits, underscores or hyphens' >&2
    exit 1
fi
RUNS=${RUNS:-10}
ITERATIONS=${ITERATIONS:-10000000}
PERF=${PERF:-1}
for str_number in "$RUNS" "$ITERATIONS" "$2" "$3"; do
    [[ "$str_number" =~ ^[0-9]+$ ]] || { echo 'Expected numeric settings/CPUs' >&2; exit 1; }
done
[[ "$RUNS" -gt 0 && "$ITERATIONS" -gt 0 && "$2" -ne "$3" ]] || exit 1
[[ "$PERF" == 0 || "$PERF" == 1 ]] || { echo 'PERF must be 0 or 1' >&2; exit 1; }
for int_padding in 0 1; do
    str_binary=build/bench_false_share_$int_padding
    [[ -x "$str_binary" ]] || { echo 'Run make bench-padding first' >&2; exit 1; }
    strings "$str_binary" | grep -x "FUSE_CACHE_PADDING=$int_padding" >/dev/null
done
str_output=bench/results/$str_label
mkdir -p "$str_output/raw"
source bench/setup_info.sh
write_setup "$str_label" build/bench_false_share_0 build/bench_false_share_1 > "$str_output/setup.txt"
printf 'CPUs: %s %s\nRUNS=%s ITERATIONS=%s PERF=%s\n' "$2" "$3" "$RUNS" "$ITERATIONS" "$PERF" >> "$str_output/setup.txt"
printf 'mode,padding,run,elapsed_ns,ns_per_iteration,checksum\n' > "$str_output/results.csv"
: > "$str_output/raw/commands.txt"
for str_mode in counters ring; do
    # Warm both layouts before the measured interleaved runs.
    for int_padding in 0 1; do
        "build/bench_false_share_$int_padding" "$str_mode" "$ITERATIONS" "$2" "$3" >/dev/null
    done
    for ((int_run=1; int_run<=RUNS; int_run++)); do
        # Alternate which layout runs first to reduce ordering bias.
        arr_order=(0 1)
        if ((int_run % 2 == 0)); then arr_order=(1 0); fi
        for int_padding in "${arr_order[@]}"; do
            str_stem=$str_output/raw/$str_mode-padding$int_padding-run$int_run
            arr_command=("build/bench_false_share_$int_padding" "$str_mode" "$ITERATIONS" "$2" "$3")
            if [[ "$PERF" == 1 ]]; then
                arr_command=(perf stat '-x,' -o "$str_stem.stat.csv" -e 'task-clock,context-switches,cpu-migrations,cycles,instructions,cache-misses' -- "${arr_command[@]}")
            fi
            printf '%q ' "${arr_command[@]}" >> "$str_output/raw/commands.txt"
            printf '\n' >> "$str_output/raw/commands.txt"
            "${arr_command[@]}" | tee "$str_stem.txt"
            awk -v run="$int_run" '{
                for (i=1; i<=NF; i++) { split($i, item, "="); value[item[1]]=item[2] }
                printf "%s,%s,%d,%s,%s,%s\n", value["mode"], value["padding"], run,
                    value["elapsed_ns"], value["ns_per_iteration"], value["checksum"]
            }' "$str_stem.txt" >> "$str_output/results.csv"
        done
    done
done
echo "Saved setup, commands, per-run results and checksums to $str_output"
