#!/usr/bin/env bash
# Source this file before building/running; use Linux tools ahead of Windows PATH.
export PATH="/usr/local/bin:/usr/bin:/bin:$PATH"
export FUSE_CYCLONEDDS_PREFIX="${FUSE_CYCLONEDDS_PREFIX:-$HOME/.local/opt/cyclonedds-0.10.5}"
if [ -x "$FUSE_CYCLONEDDS_PREFIX/bin/idlc" ]; then
    export PATH="$FUSE_CYCLONEDDS_PREFIX/bin:$PATH"
    export PKG_CONFIG_PATH="$FUSE_CYCLONEDDS_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi
if ! pkg-config --exists CycloneDDS; then
    echo 'Cyclone DDS not found. Run: bash security/scripts/setup_wsl.sh' >&2
    return 1 2>/dev/null || exit 1
fi
str_dds_libdir=$(pkg-config --variable=libdir CycloneDDS)
export LD_LIBRARY_PATH="$str_dds_libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ -z "${FUSE_DDS_PLUGIN_DIR:-}" ]; then
    if [ -f "$str_dds_libdir/libdds_security_auth.so" ]; then
        export FUSE_DDS_PLUGIN_DIR="$str_dds_libdir"
    elif [ -f "$str_dds_libdir/libddsc0debian/libdds_security_auth.so" ]; then
        export FUSE_DDS_PLUGIN_DIR="$str_dds_libdir/libddsc0debian"
    fi
fi
export FUSE_SECURITY_DIR="${FUSE_SECURITY_DIR:-$HOME/.local/share/fuse/security}"
unset str_dds_libdir
