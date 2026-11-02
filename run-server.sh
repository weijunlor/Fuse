#!/usr/bin/env bash
# Run from any directory; assets and build paths are relative to the repo root.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

build=1
while (($#)); do
    case "$1" in
        --interface)
            if [[ -z ${2:-} || $2 == --* ]]; then
                echo '--interface requires a name.' >&2; exit 1
            fi
            export FUSE_DDS_INTERFACE=$2; shift 2 ;;
        --no-build) build=0; shift ;;
        -h|--help)
            echo 'Usage: bash run-server.sh [--interface NAME] [--no-build]'
            exit 0 ;;
        *) echo "Unknown option: $1. Use --help for usage." >&2; exit 1 ;;
    esac
done

# shellcheck source=security/scripts/env.sh
source security/scripts/env.sh
python3 security/scripts/lan.py host
if ((build)); then
    make -j4 build/server
fi
exec bash security/scripts/run_secure.sh server ./build/server
