#!/usr/bin/env bash
# Each local game client selects a distinct DDS/login identity.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

player=1
build=1
while (($#)); do
    case "$1" in
        --player)
            if [[ ! ${2:-} =~ ^[1-5]$ ]]; then
                echo '--player requires a number from 1 to 5.' >&2
                exit 1
            fi
            player=$2; shift 2 ;;
        --no-build) build=0; shift ;;
        -h|--help)
            echo 'Usage: bash run-client.sh [--player 1..5] [--no-build]'
            exit 0 ;;
        *) echo "Unknown option: $1. Use --help for usage." >&2; exit 1 ;;
    esac
done

# shellcheck source=security/scripts/env.sh
source security/scripts/env.sh
if ((build)); then
    make -j4 build/client
fi
exec bash security/scripts/run_secure.sh "client-$player" ./build/client
