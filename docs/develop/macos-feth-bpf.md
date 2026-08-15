SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2026 n3n contributors

# macOS feth and BPF TAP backend

Modern macOS includes neither a TAP character device nor a public API that
provides an Ethernet TAP file descriptor. It does include paired fake Ethernet
interfaces (`feth`) and the Berkeley Packet Filter (`BPF`). Together they can
provide the semantics required by n3n without a third-party kernel extension.

## Packet path

The edge creates an unused even/odd pair, such as `feth0` and `feth1`, and
configures the overlay address on `feth0`:

```text
macOS IP and Ethernet stack
          |
        feth0  (overlay address and visible MAC)
          |
        feth1  (no IP address)
          |
       BPF capture
          |
       n3n-edge
```

Frames transmitted by macOS on `feth0` arrive as inbound frames on `feth1`.
BPF captures those frames and passes them to n3n. Frames received from n3n are
written through BPF on `feth1`, transmitted to `feth0`, and received by the
macOS network stack.

`BIOCSSEESENT=0` is important. Without it, BPF can also capture frames that n3n
has just injected and feed them back into the overlay indefinitely.

## BPF batching

A BPF `read()` can contain multiple aligned records. The existing n3n TAP API
expects exactly one Ethernet frame per `tuntap_read()` call. A reader thread
therefore parses BPF records and sends each frame as one datagram over a Unix
socket pair. The edge main loop polls the receiving side of that socket pair,
preserving the existing interface contract.

The sender side is non-blocking. If the main loop cannot drain frames quickly
enough, new captured frames are dropped rather than allowing the reader thread
to deadlock during shutdown.

## Lifecycle

The fallback only uses an interface pair that did not exist before startup.
It records ownership and destroys only that pair on shutdown. Because n3n
drops privileges after opening its packet interface, the backend starts a
minimal privileged cleanup monitor before the drop. The monitor only waits on
a pipe; normal exit or process termination closes the pipe, after which the
monitor destroys the owned pair and exits. It does not process network data.
The monitor runs in a separate session so a terminal interrupt delivered to
the edge cannot terminate the monitor before cleanup.

A requested interface name must be an unused, even-numbered `feth` name. If no
name is requested, the first unused pair is selected.

The legacy `/dev/tap` path remains the first choice when such a device is
already installed.

## Privileges

Creating and destroying `feth` interfaces and opening `/dev/bpf*` require root
on macOS. No kernel extension, Recovery-mode security change, or reboot is
required. macOS builds should configure n3n with `--with-rundir=/var/run`;
unlike Linux, macOS does not provide `/run`. The included build wrapper applies
that setting.

The macOS application firewall can prompt when a newly linked, ad-hoc-signed
edge first receives traffic. The local test launcher explicitly permits its
own `dist/n3n-edge` binary without disabling the firewall globally.

The test launcher reads the optional AES key without terminal echo and passes
it in `N3N_KEY`, rather than exposing it in command-line process listings. All
edges in a community must use the same payload cipher and key; the supernode
does not need that key.

## Manual validation

Run an edge in the foreground after substituting your community, supernode,
and overlay values:

```bash
sudo ./n3n-edge \
  -c <community> \
  -l <supernode-host:port> \
  -a static:<overlay-ip>/<prefix> \
  -Otuntap.mtu=1290 \
  -Otuntap.name=feth0 \
  -v \
  start
```

In another terminal, verify the visible interface, ARP, and overlay traffic:

```bash
ifconfig feth0
arp -an
ping <remote-overlay-ip>
```

After stopping the edge with Control-C, both interfaces should be absent:

```bash
ifconfig feth0
ifconfig feth1
```
