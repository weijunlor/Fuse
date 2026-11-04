#!/usr/bin/env bash
# Full Linux application setup, also called by the Windows setup coordinator.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

role=
dependencies=1
while (($#)); do
    case "$1" in
        --role)
            role=${2:-}; shift
            (($#)) || { echo '--role requires host or player.' >&2; exit 1; }
            shift ;;
        --skip-dependencies) dependencies=0; shift ;;
        -h|--help)
            echo 'Usage: bash run-setup.sh --role host|player [--skip-dependencies]'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done
case "$role" in host|player) ;; *) echo 'Choose --role host or --role player.' >&2; exit 1 ;; esac
if ((EUID == 0)); then
    echo 'Run setup as your regular Linux user; package installation uses sudo when needed.' >&2
    exit 1
fi

if ((dependencies)); then
    bash security/scripts/setup_wsl.sh
fi
# shellcheck source=security/scripts/env.sh
source security/scripts/env.sh
if [[ $role == host ]]; then
    python3 security/scripts/setup_host.py
fi
make -j"$(nproc)" all baseline
echo "FUSE $role application setup complete."
if [[ $role == player ]]; then
    echo 'Run the client launcher, enter the host IP and pairing code, then choose Join Game.'
else
    echo 'Run the server launcher; it displays the code for pairing player PCs.'
fi
