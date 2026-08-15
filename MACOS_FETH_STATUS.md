# Native macOS feth/BPF edge backend

## Goal

Run `n3n-edge` on current Apple Silicon macOS without installing a legacy TAP
kernel extension or rebooting the computer.

## Current implementation

- Preserves the existing `/dev/tapN` backend when a legacy driver is present.
- Automatically falls back to a built-in `feth` interface pair and BPF.
- Presents an ordinary Ethernet interface with the configured overlay address
  to macOS.
- Uses a BPF reader thread to split batched BPF records into one Ethernet frame
  per existing n3n TAP API read.
- Disables capture of frames sent by n3n to prevent an injection loop.
- Starts a minimal privileged monitor that removes the owned feth pair after
  normal exit or an unexpected edge-process exit, even though the main edge
  drops privileges.
- Keeps the cleanup monitor outside the edge terminal's foreground process
  group so Control-C cannot kill it before interface cleanup.
- Configures the requested IPv4 address in the byte order used by n3n's
  configuration parser.
- Requires no kernel extension, Recovery-mode change, or reboot.

The detailed design is in `docs/develop/macos-feth-bpf.md`.

## Build

The upstream Makefile does not handle spaces in source paths. This repository
is intentionally stored under `Side project`, so use the included wrapper. It
builds in a temporary path without spaces and copies the tested binaries back:

```bash
cd n3n_edge
./scripts/build-macos-feth.sh
```

The wrapper also configures the n3n session directory under `/var/run`. This is
required because macOS has no Linux-style `/run` directory. This local build
also defines `N3N_IPV4_ONLY`, retaining IPv4 multicast peer discovery while
omitting the IPv6 multicast socket and membership code.

Outputs:

```text
dist/n3n-edge
dist/n3n-supernode
```

## Test command for this machine

Copy `n3n-edge.local.example` to `n3n-edge.local`, set the supernode address,
community, and unique overlay address, then run the edge in a Terminal so the
administrator password can be entered interactively:

```bash
./scripts/run-local-macos-edge.sh
```

The launcher allows this specific local edge binary through the macOS
application firewall and safely removes a stale `feth0`/`feth1` test pair left
by an older build. It does not disable the firewall. It then prompts for the
AES key with terminal echo disabled and passes it through `N3N_KEY`, keeping
the secret out of the process command line. An empty key selects cleartext.
The launcher logs only warnings and errors during normal operation.

Expected startup evidence includes:

```text
No legacy /dev/tap device is available; trying the built-in macOS feth/BPF backend
Using built-in macOS feth/BPF TAP backend: feth0 <-> feth1
edge started
edge <<< ================ >>> supernode
```

While it is running:

```bash
ifconfig feth0
ping <remote-overlay-ip>
```

Stop with Control-C, then verify cleanup:

```bash
ifconfig feth0
ifconfig feth1
```

Both commands should report that the interface does not exist.

## Validation already completed

- Compiled on arm64 macOS 26.5 with the system SDK.
- Compiled `src/tuntap_osx.c` with `-Wall -Wextra -Werror`.
- Completed the upstream `make test.units` suite.
- Confirmed the built binary uses `/var/run/n3n` for its session sockets.
- `git diff --check` passes.

The remaining validation requires one interactive `sudo` run because macOS
restricts both feth creation and BPF access to root. No restart is involved.
