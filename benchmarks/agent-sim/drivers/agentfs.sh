#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/baselines/agentfs/cli/target/release/agentfs"

_store_path() {
  : "${AGENTFS_STORE:?must export AGENTFS_STORE before non-mount verbs}"
  echo "$AGENTFS_STORE"
}
_db_path()    { echo "$(_store_path)/.agentfs/agent.db"; }
_mount_path() { echo "$(_store_path)/mnt"; }
_pid_path()   { echo "$(_store_path)/agentfs.pid"; }

_mount_db() {
  local mount db pidfile
  mount="$(_mount_path)"; db="$(_db_path)"; pidfile="$(_pid_path)"
  mkdir -p "$mount"
  "$BIN" mount "$db" "$mount" > "$(_store_path)/daemon.log" 2>&1 &
  echo $! > "$pidfile"
  for _ in $(seq 1 100); do
    mountpoint -q "$mount" && break
    sleep 0.05
  done
  mountpoint -q "$mount" || { echo "agentfs mount failed" >&2; exit 1; }
}

_unmount_db() {
  local mount pidfile
  mount="$(_mount_path)"; pidfile="$(_pid_path)"
  sync
  fusermount3 -u "$mount" 2>/dev/null || true
  if [[ -f "$pidfile" ]]; then
    wait "$(<"$pidfile")" 2>/dev/null || true
    rm -f "$pidfile"
  fi
}

cmd_mount() {
  local workspace="$1" store="$2"
  export AGENTFS_STORE="$store"
  mkdir -p "$store"
  cd "$store"
  "$BIN" init --force --base "$workspace" agent > "$store/init.log" 2>&1
  _mount_db
  echo "$(_mount_path)"
}

cmd_checkpoint() {
  local label="$1"
  local store snap db
  store="$(_store_path)"; snap="$store/snap-$label"; db="$(_db_path)"
  mkdir -p "$snap"
  _unmount_db
  cp "$db" "$snap/agent.db"
  [[ -f "${db}-wal" ]] && cp "${db}-wal" "$snap/agent.db-wal" || true
  [[ -f "${db}-shm" ]] && cp "${db}-shm" "$snap/agent.db-shm" || true
  _mount_db
}

cmd_rollback() {
  local label="$1"
  local store snap db
  store="$(_store_path)"; snap="$store/snap-$label"; db="$(_db_path)"
  [[ -d "$snap" ]] || { echo "rollback: snapshot $label missing" >&2; exit 1; }
  _unmount_db
  cp "$snap/agent.db" "$db"
  [[ -f "$snap/agent.db-wal" ]] && cp "$snap/agent.db-wal" "${db}-wal" || rm -f "${db}-wal"
  [[ -f "$snap/agent.db-shm" ]] && cp "$snap/agent.db-shm" "${db}-shm" || rm -f "${db}-shm"
  _mount_db
}

# pseudo-branch: save current state as a named baseline that can be
# rolled back to later. the orchestrator rolls back to this baseline
# before starting work on each "branch".
cmd_branch_create() {
  local name="$1"
  cmd_checkpoint "__branch-${name}__"
}

cmd_storage_size() {
  du -sb "$(_store_path)" 2>/dev/null | awk '{print $1}'
}

cmd_unmount() { _unmount_db; }

case "${1:-}" in
  mount)         cmd_mount "$2" "$3" ;;
  checkpoint)    cmd_checkpoint "$2" ;;
  rollback)      cmd_rollback "$2" ;;
  branch-create) cmd_branch_create "$2" ;;
  storage_size)  cmd_storage_size ;;
  unmount)       cmd_unmount ;;
  *) echo "usage: $0 <mount|checkpoint|rollback|branch-create|storage_size|unmount> ..." >&2; exit 2 ;;
esac
