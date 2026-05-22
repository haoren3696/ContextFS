<p align="center">
  <img src="https://github.com/thustorage/ContextFS/raw/main/docs/agentvfs-logo.svg" alt="AgentVFS" width="400">
</p>

<p align="center"><strong>Checkpoint &amp; Rollback · Per-Agent Branch Views · Pluggable Telemetry</strong></p>

<p align="center">A checkpointable, branchable, and content-addressed FUSE filesystem for AI agents.</p>

## Key Features

| Feature | Description |
|---------|-------------|
| ⏪&nbsp;**Checkpoint&nbsp;&amp;&nbsp;Rollback** | Snapshot the working tree and roll back to any prior checkpoint |
| 🌿&nbsp;**Per&#8209;Agent&nbsp;Branches** | N agents over one source tree, routed by cgroup v2; three-way merge or surfaced conflicts |
| 🔗&nbsp;**Content&#8209;Addressed&nbsp;Store** | blake3-hashed objects deduplicate across checkpoints and branches — near-zero-cost snapshots |
| 🛰️&nbsp;**Pluggable&nbsp;Telemetry** | NDJSON audit via eBPF / fanotify / ptrace / `LD_PRELOAD`; Wasm or Lua to filter and verdict |
| 🖥️&nbsp;**Cross&#8209;Platform** | libfuse3 (Linux), fuse-t (macOS), WinFsp (Windows) |
| 🤖&nbsp;**Agent&#8209;CLI&nbsp;Skills** | `./start.sh` mounts a project and installs a Skill for Claude Code / Codex to checkpoint and roll back |

## Quick start (Linux)

Reference platform. eBPF additionally requires kernel BTF at `/sys/kernel/btf/vmlinux`; pass `-DAGENTVFS_EBPF=OFF` to skip.

```bash
# Dependencies (Debian/Ubuntu)
sudo apt install build-essential cmake libfuse3-dev pkg-config \
                 clang libbpf-dev linux-tools-generic

# Build & install
cmake -B build && cmake --build build -j
sudo cmake --install build

# Mount a project — prints `mount=<path>` and a `cd <path> && claude` (or `codex`) hint
export AGENTVFS_WORKSPACE_ROOT=~/.local/share/agentvfs
./start.sh /path/to/project
# equivalent: ./start.sh --root ~/.local/share/agentvfs /path/to/project
```

## Driving the daemon directly

```bash
agentvfs workspace start my-task --from /path/to/project
agentvfs workspace checkpoint my-task before-refactor
# ... agent makes changes ...
agentvfs workspace rollback my-task before-refactor
agentvfs workspace stop my-task
```

## Windows

Requires [WinFsp 2.0+](https://winfsp.dev). The control pipe name is printed at startup.

```powershell
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON
cmake --build build --config Release -j
.\build\Release\agentvfs.exe --source C:\some\dir --mountpoint Z:
.\build\Release\agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline
```

## macOS

Requires [fuse-t](https://www.fuse-t.org/). The control socket path is printed at startup.

```bash
brew install --cask macos-fuse-t/cask/fuse-t
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON
cmake --build build -j
./build/agentvfs --source ~/some-dir --mountpoint /tmp/agentvfs-mnt &
./build/agentvfs-ctl --sock /tmp/agentvfs-<hash>.sock checkpoint baseline
```

## How it works

```
agent process
   │  read/write
   ▼
FUSE / WinFsp / fuse-t  ──►  WorkingTree (in-memory, COW)  ──►  ObjectStore (blake3 CAS)
                                                            ▲
                                              checkpoint /  │  rollback
                                                            │
                                              control socket│ ─► agentvfs-ctl
                                                            │
                                              optional eBPF │ ─► NDJSON audit log
```

## Platform support

| Feature | Linux | macOS | Windows |
|---|:---:|:---:|:---:|
| Checkpoint &amp; Rollback | ✅ | ✅ | ✅ |
| Content-Addressed Store | ✅ | ✅ | ✅ |
| Per-Agent Branches | ✅ | Coming soon | Coming soon |
| Pluggable Telemetry | ✅ | Coming soon | Coming soon |
| `agentvfs workspace` CLI | ✅ | Coming soon | Coming soon |


## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE).
