#!/usr/bin/env bash
set -euo pipefail
exec /usr/bin/python3 "$(dirname "$0")/security.py" ca "$@"
