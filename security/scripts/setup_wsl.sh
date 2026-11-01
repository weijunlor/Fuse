#!/usr/bin/env bash
# Install dependencies, then build the team's pinned DDS version in a user prefix.
set -euo pipefail
export PATH="/usr/local/bin:/usr/bin:/bin:$PATH"
if [ $# -gt 1 ] || { [ $# -eq 1 ] && [ "$1" != --no-packages ]; }; then
    echo 'Usage: setup_wsl.sh [--no-packages]' >&2; exit 1
fi
if [ "${1:-}" != --no-packages ]; then
    sudo apt-get update
    sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake git curl \
        pkg-config bison libssl-dev libglfw3-dev libstb-dev libgl1-mesa-dev openssl \
        tcpdump tshark libxml2-utils shellcheck xdotool
fi
str_prefix=${FUSE_CYCLONEDDS_PREFIX:-$HOME/.local/opt/cyclonedds-0.10.5}
str_source=$HOME/.cache/fuse/cyclonedds-0.10.5
if [ ! -d "$str_source" ]; then
    mkdir -p "$(dirname "$str_source")"
    git clone --depth 1 --branch 0.10.5 https://github.com/eclipse-cyclonedds/cyclonedds.git "$str_source"
fi
[ "$(git -C "$str_source" describe --tags --exact-match)" = 0.10.5 ] || {
    echo "Expected the 0.10.5 source tag in $str_source" >&2; exit 1;
}
cmake -S "$str_source" -B "$str_source/build-fuse" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$str_prefix" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON \
    -DENABLE_SECURITY=ON -DENABLE_SSL=ON -DENABLE_SHM=OFF -DBUILD_IDLC=ON \
    -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
cmake --build "$str_source/build-fuse" --parallel "$(nproc)"
cmake --install "$str_source/build-fuse"
echo "Installed Cyclone DDS 0.10.5. Source security/scripts/env.sh before make."
