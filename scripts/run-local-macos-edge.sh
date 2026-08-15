#!/bin/sh

# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: Copyright 2026 n3n contributors

set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
edge="$project_root/dist/n3n-edge"

# Keep site-specific addresses and community names out of the repository.
local_config="$project_root/n3n-edge.local"
if [ -f "$local_config" ]; then
    # shellcheck disable=SC1090
    . "$local_config"
fi

: "${N3N_COMMUNITY:=n3n-macos}"
: "${N3N_SUPERNODE:=127.0.0.1:9527}"
: "${N3N_ADDRESS:=192.168.77.1/24}"
: "${N3N_TUNTAP_NAME:=feth0}"
: "${N3N_MTU:=1290}"

case $# in
    0)
        ;;
    2)
        if [ "$1" != "-k" ]; then
            echo "Usage: $0 [-k AES_KEY]" >&2
            exit 2
        fi
        N3N_KEY=$2
        export N3N_KEY
        ;;
    *)
        echo "Usage: $0 [-k AES_KEY]" >&2
        exit 2
        ;;
esac

if [ ! -x "$edge" ]; then
    echo "Missing $edge" >&2
    echo "Run ./scripts/build-macos-feth.sh first." >&2
    exit 1
fi

sudo -v

# Locally linked binaries are only ad-hoc signed, so macOS may prompt for an
# inbound firewall decision after a rebuild. This dedicated test launcher
# explicitly allows only this edge binary; it does not disable the firewall.
firewall=/usr/libexec/ApplicationFirewall/socketfilterfw
if [ -x "$firewall" ]; then
    sudo "$firewall" --unblockapp "$edge" >/dev/null
fi

# A build prior to the cleanup-helper fix could leave this dedicated local
# test pair behind after Control-C. Only reclaim it when both sides still point
# at each other and no n3n edge process is running.
if ifconfig feth0 >/dev/null 2>&1 || ifconfig feth1 >/dev/null 2>&1; then
    if pgrep -x n3n-edge >/dev/null 2>&1; then
        echo "Refusing to remove feth0/feth1 while an n3n-edge process is running." >&2
        exit 1
    fi
    if ifconfig feth0 2>/dev/null | grep -q 'peer: feth1' &&
       ifconfig feth1 2>/dev/null | grep -q 'peer: feth0'; then
        echo "Removing stale local test interfaces feth0 <-> feth1"
        sudo /sbin/ifconfig feth0 destroy || true
        sudo /sbin/ifconfig feth1 destroy || true
    else
        echo "feth0 or feth1 already exists but is not the expected peer pair; refusing to remove it." >&2
        exit 1
    fi
fi

if [ -z "${N3N_KEY:-}" ]; then
    printf "AES key (input hidden; empty disables encryption): " >&2
    stty -echo
    trap 'stty echo; printf "\n" >&2; exit 130' HUP INT TERM
    IFS= read -r N3N_KEY || {
        stty echo
        trap - HUP INT TERM
        printf "\nUnable to read AES key.\n" >&2
        exit 1
    }
    stty echo
    trap - HUP INT TERM
    printf "\n" >&2
fi

if [ -n "${N3N_KEY:-}" ]; then
    export N3N_KEY
    sudo_prefix="sudo --preserve-env=N3N_KEY"
else
    sudo_prefix="sudo"
    echo "WARNING: starting without payload encryption" >&2
fi

# shellcheck disable=SC2086 # sudo_prefix intentionally contains two arguments.
exec $sudo_prefix "$edge" \
    -c "$N3N_COMMUNITY" \
    -l "$N3N_SUPERNODE" \
    -Oconnection.bind=0.0.0.0:0 \
    -a "static:$N3N_ADDRESS" \
    -Otuntap.mtu="$N3N_MTU" \
    -Otuntap.name="$N3N_TUNTAP_NAME" \
    -Ologging.verbose=1 \
    start
