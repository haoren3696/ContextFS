# agent-sim — three-method checkpoint benchmark

Runs a 1000-question mixed-workload simulation against three FUSE filesystems
(agentvfs, agentfs, branchfs) in series. Records per-checkpoint latency,
on-disk storage size, and a sampled rollback latency.

## Prerequisites

Pre-built binaries at:
- `build/agentvfs`, `build/agentvfs-ctl` (from `cmake -B build && cmake --build build -j`)
- `baselines/agentfs/cli/target/release/agentfs`
- `baselines/branchfs/target/release/branchfs`

Plus: `fusermount3`, Bash 5+, `awk`.

## Run

    bash benchmarks/agent-sim/run.sh                      # 1000 Q, 50 ckpts
    SCALE=10 bash benchmarks/agent-sim/run.sh              # 10000 Q, 500 ckpts
    SCALE=10 BRANCHES=5 bash benchmarks/agent-sim/run.sh   # 10000 Q, 500 ckpts, 5 branches
    QUICK_MODE=1 bash benchmarks/agent-sim/run.sh         # 40 Q, 2 ckpts (fast iteration)

Env vars:

| var | default | description |
|---|---|---|
| SCALE | 1 | multiplies question count (1000 × SCALE) |
| BRANCHES | 0 | fan-out mode: 0=linear, N=split into N branches |
| QUICK_MODE | 0 | overrides to 40 Q regardless of SCALE |
| AGENT_SIM_TMP | /tmp | base dir for per-method tmpdirs (use a large partition) |

Output lands in `results/<run-id>/`:

- `env.txt` — host/kernel/binary versions
- `perop.csv` — `method,label,q,ckpt_us,store_bytes_after,status`
- `rollback.csv` — `method,target_label,rollback_us,status`
- `totals.csv` — `method,total_wall_s,n_checkpoints_ok,n_checkpoints_failed,peak_store_bytes`
- `<method>.log` — daemon + driver stderr
- `summary.md` — human-readable table

Exit code is non-zero if any method had any failed verb.
