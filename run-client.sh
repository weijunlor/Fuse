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
            echo 'Without --player, opens the saved connection or in-game pairing screen.'
            echo 'Simultaneous windows use separate saved player profiles; pair each new player once.'
            echo 'Omit --server for automatic discovery; use the host LAN IPv4 for direct connections.'
            exit 0 ;;
        *) echo "Unknown option: $1. Use --help for usage." >&2; exit 1 ;;
    esac
done

# shellcheck source=security/scripts/env.sh
source security/scripts/env.sh
# Validate networking before compiling or opening a window.
if [[ -n $player ]]; then
    python3 security/scripts/lan.py network --player "$player"
fi
if ((build)); then
    make -j4 build/client build/pairing
fi
if [[ -n $player ]]; then
    exec bash security/scripts/run_secure.sh "client-$player" ./build/client
fi
exec python3 security/scripts/client_session.py ./build/client
