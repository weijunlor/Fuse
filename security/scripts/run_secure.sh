#!/usr/bin/env bash
# Also works for the unchanged baseline: DDS reads the generated configuration.
set -euo pipefail
str_script_dir=$(cd "$(dirname "$0")" && pwd)
source "$str_script_dir/env.sh"
if [ $# -lt 2 ]; then
    echo 'Usage: run_secure.sh <server|client-1..client-5> <executable> [args...]' >&2
    exit 1
fi
export FUSE_DDS_IDENTITY=$1 FUSE_DDS_SECURITY=on
shift
export CYCLONEDDS_URI
CYCLONEDDS_URI=$(/usr/bin/python3 "$str_script_dir/security.py" config "$FUSE_DDS_IDENTITY")
cd "$str_script_dir/../.."
exec "$@"
