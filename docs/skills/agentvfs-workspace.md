---
name: agentvfs-workspace
description: Use when starting, resuming, or stopping a single-agent agentvfs workspace - drives `agentvfs workspace` for a content-addressed FUSE workspace with checkpoint/rollback.
---

# AgentVFS Workspace

This Skill drives the `agentvfs workspace ...` command surface so an agent can
work inside a content-addressed FUSE workspace with checkpoint and rollback,
without managing the daemon or control socket directly.

## When to use

- The user wants the agent to work in a sandboxed, checkpointable copy of
  their project.
- The user mentions agentvfs, content-addressed snapshots, FUSE workspaces,
  or rollback.

## Commands

```bash
# Seed a workspace from an existing project (run once if the project is on disk).
agentvfs workspace init <name> --from /path/to/project

# Start (or reuse) the workspace. Prints `mount=<path>`; treat that as cwd.
agentvfs workspace start <name>

# Snapshot before risky changes.
agentvfs workspace checkpoint <name> <label>

# Roll back to a label or commit hash.
agentvfs workspace rollback <name> <label>

# Re-discover paths after context loss.
agentvfs workspace status <name>
agentvfs workspace list

# Tear down (final checkpoint + unmount). Add --no-checkpoint to skip the
# final snapshot.
agentvfs workspace stop <name>
```

## Output contract

All commands print line-oriented `key=value` pairs. Parse `mount=`,
`socket=`, `store=`, `telemetry=`, and `status=` with `grep` or `sed`.
`workspace list` is the exception: it prints tab-separated rows of
`<name>\t<status>\t<mount>`.

## Rules

- Treat the printed `mount=` path as the working directory for all
  filesystem operations.
- If the user's project is already on disk, run `init --from <dir>` before
  `start` instead of copying through the mount.
- Always `checkpoint` before broad transforms, merges, or risky edits, and
  pick a meaningful label.
- Never edit files under `source/`, `store/`, or anything outside `mount/`.
- After context loss, run `status <name>` (or `list` if the name is also
  lost) to recover the mount and socket paths.
- Stop the workspace at the end of the task unless the user asks to keep
  it mounted. `stop` checkpoints by default and refuses to unmount if the
  final checkpoint fails — pass `--no-checkpoint` only when the user has
  explicitly confirmed they accept losing uncheckpointed changes.

## Recovery

`status` exits 0 only when the workspace is healthy. Exit code 1 with
`status=stale` means the daemon died — run `start <name>` again to
recover. Exit code 1 with `status=stopped` means there is no live daemon;
`start` will boot a fresh one.

## Exit codes

- `0` — success, or (for `status`) the workspace is healthy.
- `1` — operational failure (daemon couldn't be reached, checkpoint
  failed, unmount failed, etc.). Stderr explains.
- `2` — usage error (bad subcommand, missing required argument, invalid
  workspace name). Stderr shows the offending input.
