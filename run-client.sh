#!/usr/bin/env bash
# Each game client selects a distinct DDS/login identity, locally or over LAN.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

player=
build=1
while (($#)); do
    case "$1" in
        --server|--interface)
            option=$1
            if [[ -z ${2:-} || $2 == --* ]]; then
                echo "$option requires a value." >&2; exit 1
            fi
            if [[ $option == --server ]]; then
                export FUSE_SERVER_IP=$2
            else
                export FUSE_DDS_INTERFACE=$2
            fi
            shift 2 ;;
        --player)
            if [[ ! ${2:-} =~ ^[1-5]$ ]]; then
                echo '--player requires a number from 1 to 5.' >&2
                exit 1
            fi
            player=$2; shift 2 ;;
        --no-build) build=0; shift ;;
        -h|--help)
            echo 'Usage: bash run-client.sh [--player 1..5] [--server IPv4] [--interface NAME] [--no-build]'
            echo 'Prompts for a player number when --player is omitted.'
            echo 'Omit --server for automatic discovery; use the host LAN IPv4 for direct connections.'
            exit 0 ;;
        *) echo "Unknown option: $1. Use --help for usage." >&2; exit 1 ;;
    esac
done

while [[ ! $player =~ ^[1-5]$ ]]; do
    printf 'Player number (1-5): ' >&2
    if ! IFS= read -r player; then
        printf '\nNo player number supplied. Use --player 1..5 for noninteractive runs.\n' >&2
        exit 1
    fi
    if [[ ! $player =~ ^[1-5]$ ]]; then
        echo 'Enter a number from 1 to 5.' >&2
    fi
done

# shellcheck source=security/scripts/env.sh
source security/scripts/env.sh
# Validate networking before compiling or opening a window.
python3 security/scripts/lan.py network --player "$player"
if ((build)); then
    make -j4 build/client
fi
exec bash security/scripts/run_secure.sh "client-$player" ./build/client
