SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright 2020 n2n contributors
SPDX-FileCopyrightText: Copyright Hamish Coleman

# Build on macOS
 
The macOS build essentially can use the generic build instructions,
but first needs a couple of other packages installed:
 
```bash
brew install automake
```
 
On current macOS versions, n3n first tries to use an available legacy
`/dev/tap` interface. If none is installed, it automatically uses a pair of
built-in `feth` interfaces with BPF as the packet interface. The fallback does
not install a kernel extension and does not require a reboot, but the edge must
run as root so it can create the interfaces and open a BPF device:

```bash
sudo n3n-edge -c example -l supernode.example:7654 -a 192.168.100.1 start
```

The visible interface is the even-numbered side of the pair (for example,
`feth0`), while n3n attaches BPF to the odd-numbered peer (`feth1`). The pair
is removed when the edge exits. Use `-Otuntap.name=feth2` to request a specific
unused even-numbered pair; otherwise the first available pair is selected.

Legacy TAP drivers remain supported when `/dev/tap` devices are already
available, but they are no longer required for a normal build.

The BPF reader disables capture of frames sent on the peer interface. This
prevents frames injected by n3n from being captured again and forming a packet
loop.
