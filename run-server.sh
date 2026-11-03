#!/usr/bin/env bash
# Run from any directory; assets and build paths are relative to the repo root.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

build=1
pairing=1
while (($#)); do
    case "$1" in
        --interface)
            if [[ -z ${2:-} || $2 == --* ]]; then
                echo '--interface requires a name.' >&2; exit 1
            fi
            export FUSE_DDS_INTERFACE=$2; shift 2 ;;
        --no-build) build=0; shift ;;
        --no-pairing) pairing=0; shift ;;
        -h|--help)
            echo 'Usage: bash run-server.sh [--interface NAME] [--no-build] [--no-pairing]'
            exit 0 ;;
        *) echo "Unknown option: $1. Use --help for usage." >&2; exit 1 ;;
    esac
done

# shellcheck source=security/scripts/env.sh
source security/scripts/env.sh
python3 security/scripts/lan.py host
if ((build)); then
    make -j4 build/server build/pairing
fi
if ((!pairing)); then
    exec bash security/scripts/run_secure.sh server ./build/server
fi
game_pid=
pair_pid=
cleanup() {
    trap - EXIT INT TERM
    if [[ -n $pair_pid ]]; then kill -TERM "$pair_pid" 2>/dev/null || true; fi
    if [[ -n $game_pid ]]; then kill -TERM "$game_pid" 2>/dev/null || true; fi
    if [[ -n $pair_pid ]]; then wait "$pair_pid" 2>/dev/null || true; fi
    if [[ -n $game_pid ]]; then wait "$game_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
bash security/scripts/run_secure.sh server ./build/server </dev/null &
game_pid=$!
# Explicit stdin inheritance lets terminal commands reach the pairing service.
./build/pairing host <&0 &
pair_pid=$!
wait "$game_pid"
