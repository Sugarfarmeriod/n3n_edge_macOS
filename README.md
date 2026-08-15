# n3n_edge_macOS

Native macOS `feth` + BPF backend and local launcher for n3n edge.

适用于现代 macOS 的 n3n edge 原生 `feth` + BPF 后端与本地启动工具。

## Features / 功能

- Native macOS `feth` pair with BPF capture/injection; no TAP kernel extension
  or reboot required.
- 使用 macOS 原生 `feth` 接口对和 BPF 捕获/注入，不需要 TAP 内核扩展或重启。
- Cleans up the interfaces owned by the edge after normal or interrupted exit.
- 正常退出或中断退出后自动清理本 edge 创建的虚拟接口。
- IPv4-only local build for the included launcher.
- 随附启动器使用 IPv4-only 构建，避免启用 IPv6 多播。
- Build wrapper works when the checkout path contains spaces.
- 构建脚本支持包含空格的项目路径。
- Launcher supports hidden AES-key input and quiet logging.
- 启动器支持隐藏输入 AES 密钥和低噪声日志。

## Build / 构建

```bash
cd n3n_edge_macOS
./scripts/build-macos-feth.sh
```

The tested binaries are written to `dist/`. The included build currently
targets Apple Silicon and macOS 26 or newer.

编译后的二进制文件写入 `dist/`。当前构建目标是 Apple Silicon，并要求 macOS 26
或更高版本。

## Run / 运行

Copy `n3n-edge.local.example` to `n3n-edge.local`, then set the supernode,
community, and a unique overlay IP for this Mac:

将 `n3n-edge.local.example` 复制为 `n3n-edge.local`，然后填写 supernode 地址、
社区名，以及本机唯一的虚拟网段 IP：

```bash
cp n3n-edge.local.example n3n-edge.local
./scripts/run-local-macos-edge.sh -k 'shared-aes-key'
```

All edge nodes in the same community must use the same AES key and distinct
overlay IP addresses. The supernode does not need the payload key.

同一社区中的所有 edge 必须使用完全相同的 AES 密钥，并使用不同的虚拟 IP；
supernode 不需要 payload AES 密钥。

The local configuration file is ignored by Git. Do not put the AES key in it
or commit it.

本机配置文件已被 Git 忽略。不要把 AES 密钥写入该文件，也不要提交该文件。

## Design notes / 设计说明

See [MACOS_FETH_STATUS.md](MACOS_FETH_STATUS.md) for the current status and
[docs/develop/macos-feth-bpf.md](docs/develop/macos-feth-bpf.md) for packet-path
and lifecycle details.

当前状态见 [MACOS_FETH_STATUS.md](MACOS_FETH_STATUS.md)，数据包路径和生命周期见
[docs/develop/macos-feth-bpf.md](docs/develop/macos-feth-bpf.md)。

This work remains under the upstream project's GPL license; see
[LICENCE.md](LICENCE.md).

本项目继续遵循上游项目的 GPL 许可证，详见 [LICENCE.md](LICENCE.md)。
