#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/baselines/branchfs/target/release/branchfs"

_store_path() {
  : "${BRANCHFS_STORE:?must export BRANCHFS_STORE before non-mount verbs}"
  echo "$BRANCHFS_STORE"
}
_mount_path()           { echo "$(_store_path)/mnt"; }
_pid_path()             { echo "$(_store_path)/branchfs.pid"; }
_replay_counter_path()  { echo "$(_store_path)/replay-counter"; }

cmd_mount() {
  local workspace="$1" store="$2"
  export BRANCHFS_STORE="$store"
  local mount; mount="$(_mount_path)"
  mkdir -p "$mount" "$store/data"
  echo 0 > "$(_replay_counter_path)"
  "$BIN" mount "$mount" --base "$workspace" --storage "$store/data" \
    > "$store/daemon.log" 2>&1 &
  echo $! > "$(_pid_path)"
  for _ in $(seq 1 100); do
    mountpoint -q "$mount" && break
    sleep 0.05
  done
  mountpoint -q "$mount" || { echo "branchfs mount failed" >&2; exit 1; }
  echo "$mount"
}

cmd_checkpoint() {
  local label="$1"
  "$BIN" create "$label" "$(_mount_path)" --storage "$(_store_path)/data" >/dev/null
}

cmd_rollback() {
  local label="$1"
  local counter
  counter="$(<"$(_replay_counter_path)")"
  counter=$((counter + 1))
  echo "$counter" > "$(_replay_counter_path)"
  "$BIN" create "${label}_replay_${counter}" "$(_mount_path)" \
    --parent "$label" --storage "$(_store_path)/data" >/dev/null
}

cmd_branch_create() {
  local name="$1" parent="${2:-main}"
  "$BIN" create "$name" "$(_mount_path)" \
    --parent "$parent" --storage "$(_store_path)/data" >/dev/null
}

cmd_storage_size() {
  du -sb "$(_store_path)/data" 2>/dev/null | awk '{print $1}'
}

cmd_unmount() {
  local mount pidfile
  mount="$(_mount_path)"; pidfile="$(_pid_path)"
  fusermount3 -u "$mount" 2>/dev/null || true
  if [[ -f "$pidfile" ]]; then
    wait "$(<"$pidfile")" 2>/dev/null || true
    rm -f "$pidfile"
  fi
}

case "${1:-}" in
  mount)         cmd_mount "$2" "$3" ;;
  checkpoint)    cmd_checkpoint "$2" ;;
  rollback)      cmd_rollback "$2" ;;
  branch-create) cmd_branch_create "$2" "${3:-main}" ;;
  storage_size)  cmd_storage_size ;;
  unmount)       cmd_unmount ;;
  *) echo "usage: $0 <mount|checkpoint|rollback|branch-create|storage_size|unmount> ..." >&2; exit 2 ;;
esac
