#!/bin/sh

# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: Copyright 2026 n3n contributors

set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=$(mktemp -d /tmp/n3n-edge-build.XXXXXX)

cleanup() {
    rm -rf -- "$build_root"
}
trap cleanup EXIT HUP INT TERM

rsync -a \
    --exclude dist \
    --exclude '*.o' \
    --exclude '*.d' \
    "$project_root/" \
    "$build_root/"

cd "$build_root"
./autogen.sh
# macOS has /var/run (a link to /private/var/run), but no Linux-style /run.
# n3n creates its session/control socket directory while it still has root.
macos_cflags=${CFLAGS:--O2 -g}
CFLAGS="$macos_cflags -DN3N_IPV4_ONLY" ./configure --with-rundir=/var/run
make -j"$(sysctl -n hw.logicalcpu)"
make test.units

mkdir -p "$project_root/dist"
install -m 755 apps/n3n-edge "$project_root/dist/n3n-edge"
install -m 755 apps/n3n-supernode "$project_root/dist/n3n-supernode"

echo "Built and tested:"
echo "  $project_root/dist/n3n-edge"
echo "  $project_root/dist/n3n-supernode"
