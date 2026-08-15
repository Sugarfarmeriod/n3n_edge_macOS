# n3n edge on modern macOS

This repository contains a local macOS build of [n3n](https://github.com/n42n/n3n)
with a native `feth` + BPF packet backend. It is intended for Apple Silicon
macOS systems where the legacy TAP kernel extension is unavailable or
undesirable.

## What is included

- Native macOS `feth` pair with BPF capture/injection; no TAP kext or reboot.
- Cleanup of the owned `feth` pair after normal or interrupted exit.
- IPv4-only local build for the included launcher.
- A build wrapper that works when the checkout path contains spaces.
- A local launcher with hidden AES-key input and quiet logging.

## Build

```bash
cd n3n_edge
./scripts/build-macos-feth.sh
```

The tested binaries are written to `dist/`. The included build currently
targets Apple Silicon and macOS 26 or newer.

## Run

Copy `n3n-edge.local.example` to `n3n-edge.local` and set the supernode,
community, and unique overlay IP for this Mac. Then run:

```bash
./scripts/run-local-macos-edge.sh -k 'shared-aes-key'
```

The local file is ignored by Git. On another edge, use the same community and
AES key but a different overlay address.

All edge nodes in the same community must use the same AES key and distinct
overlay IP addresses. The supernode does not need the payload key.

## Design notes

See [MACOS_FETH_STATUS.md](MACOS_FETH_STATUS.md) for the current status and
[docs/develop/macos-feth-bpf.md](docs/develop/macos-feth-bpf.md) for the packet
path and lifecycle details.

This work remains under the upstream project's GPL license; see
[LICENCE.md](LICENCE.md).
