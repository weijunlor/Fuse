#!/usr/bin/env bash
# Source from launchers. Keep the requested mode explicit and reproducible.
export FUSE_DDS_SECURITY=${FUSE_DDS_SECURITY:-on}
case $FUSE_DDS_SECURITY in
    on) source security/scripts/env.sh ;;
    off) unset CYCLONEDDS_URI ;;
    *) echo 'FUSE_DDS_SECURITY must be on or off' >&2; return 1 ;;
esac

# Exec preserves the process ID used by perf and the launcher's cleanup.
# Secure XML also gives historical baseline binaries the same security policy.
dds_run() {
    local str_identity=$1; shift
    if [ "$FUSE_DDS_SECURITY" = on ]; then
        exec bash security/scripts/run_secure.sh "$str_identity" "$@"
    else
        exec "$@"
    fi
}

dds_check_clients() {
    if [ "$FUSE_DDS_SECURITY" = on ] && [ "$1" -gt 5 ]; then
        echo 'Secure benchmarks support client-1 through client-5; use at most five clients.' >&2
        return 1
    fi
}
