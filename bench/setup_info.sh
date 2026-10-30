#!/usr/bin/env bash
# Shared by bench/profile.sh and bench/render.sh (source it, do not run it).
# Writes the setup.txt that sits next to every set of results: build flags and
# optimisation level (read from the binaries' debug info), machine, software.

# Compile flags of a binary's first compilation unit, from DWARF debug info
build_flags() {
    readelf --debug-dump=info "$1" 2> /dev/null \
        | grep -m1 DW_AT_producer | sed 's/.*): //' || true
}

# Optimisation level (-O0, -O2, ...) of a binary, empty if unknown
opt_level() {
    grep -o -- ' -O[0-9sgz]*' <<< "$(build_flags "$1")" | tail -1 | tr -d ' ' || true
}

# write_setup <label> <binary>... : print setup.txt to stdout.
# Set STR_GL_RENDERER to include the OpenGL renderer (the client prints it).
write_setup() {
    local label=$1; shift
    echo "Profiling setup: $label"
    echo "Recorded: $(date -Iseconds)"
    echo "DDS security: ${FUSE_DDS_SECURITY:-on}"
    echo "Application authentication: ${FUSE_AUTH:-on}"
    echo "Thread handoff: ${FUSE_HANDOFF:-spsc}"
    echo
    echo "== Build"
    for bin in "$@"; do
        echo "$bin:"
        echo "  Optimisation level: $(opt_level "$bin" | grep . || echo 'unknown (no debug info in binary)')"
        echo "  Compiler flags (DW_AT_producer): $(build_flags "$bin" | grep . || echo 'unknown (build with -g)')"
        echo "  sha256: $(sha256sum "$bin" | cut -d' ' -f1)"
    done
    # SOURCE_REF: set when the binaries were built from another commit or tag
    # (e.g. in a git worktree) than the one checked out here
    if [ -n "${SOURCE_REF:-}" ]; then
        echo "Git: binaries built from $SOURCE_REF ($(git rev-parse --short "$SOURCE_REF^{commit}"))"
    else
        echo "Git: $(git rev-parse --short HEAD) on $(git rev-parse --abbrev-ref HEAD)" \
             "$(git diff --quiet HEAD -- . ':!bench/results' || echo '(uncommitted changes)')"
    fi
    echo
    echo "== Machine"
    lscpu | grep -E '^(Model name|CPU\(s\)|Thread|Core|Socket|L1d|L1i|L2|L3)'
    grep MemTotal /proc/meminfo
    echo "P-cores (cpu_core): $(cat /sys/devices/cpu_core/cpus 2> /dev/null || echo n/a)"
    echo "E-cores (cpu_atom): $(cat /sys/devices/cpu_atom/cpus 2> /dev/null || echo n/a)"
    echo "Governor: $(sort -u /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | tr '\n' ' ')"
    echo "Turbo disabled: $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2> /dev/null || echo n/a)"
    [ -n "${STR_GL_RENDERER:-}" ] && echo "OpenGL renderer: $STR_GL_RENDERER"
    echo
    echo "== Software"
    grep PRETTY_NAME /etc/os-release | cut -d= -f2 | tr -d '"'
    echo "Kernel: $(uname -r)"
    gcc --version | head -1
    perf --version
    echo "Cyclone DDS: $(pkg-config --modversion CycloneDDS)"
    echo "GLFW: $(pkg-config --modversion glfw3)"
    echo "Mesa: $(dpkg-query -W -f '${Version}' libgl1-mesa-dri 2> /dev/null || echo n/a)"
    echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
    echo "kptr_restrict: $(cat /proc/sys/kernel/kptr_restrict)"
    echo "Display session: ${XDG_SESSION_TYPE:-unknown}"
}

# Turn a perf stat output file into "value,,event" lines (the layout of
# perf stat -x,) so one parser reads every run. Accepts both the normal
# human-readable output (current runs: raw/stat_*.txt) and CSV (older runs:
# raw/stat_*.csv). "# started on" lines are kept: each marks a new run.
# <not counted> events (E-core copies on the hybrid CPU) are dropped.
stat_normalize() {
    awk '
        /^# started/ { print; next }
        # CSV line: value,unit,event,...
        /^[0-9.]+,/ { print; next }
        # Human-readable line: "  1,234,567      event  # ..." or "  12.34 msec task-clock"
        $1 ~ /^[0-9][0-9,]*(\.[0-9]+)?$/ && $2 != "seconds" {
            v = $1; gsub(",", "", v)
            e = ($2 == "msec") ? $3 : $2
            print v ",," e
        }' "$1"
}
