#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/build/agentvfs"
CTL="$REPO/build/agentvfs-ctl"

_store_path() {
  : "${AGENTVFS_STORE:?must export AGENTVFS_STORE before non-mount verbs}"
  echo "$AGENTVFS_STORE"
}
_sock_path() { echo "$(_store_path)/agentvfs.sock"; }

cmd_mount() {
  local workspace="$1" store="$2"
  local mount="$store/mnt"
  local sock="$store/agentvfs.sock"
  local pidfile="$store/agentvfs.pid"
  mkdir -p "$mount" "$store/objects"
  "$BIN" --source "$workspace" --mountpoint "$mount" --store "$store/objects" \
    --control-sock "$sock" -f > "$store/daemon.log" 2>&1 &
  echo $! > "$pidfile"
  for _ in $(seq 1 100); do
    [[ -S "$sock" ]] && mountpoint -q "$mount" && break
    sleep 0.05
  done
  [[ -S "$sock" ]] || { echo "agentvfs daemon socket missing" >&2; exit 1; }
  mountpoint -q "$mount" || { echo "agentvfs mount not live" >&2; exit 1; }
  echo "$mount"
}

cmd_checkpoint() {
  local label="$1" branch="${2:-}"
  local store="$(_store_path)" hashfile
  hashfile="$store/hash-$label"
  [[ -n "$branch" ]] && hashfile="$store/hash-${branch}-$label"
  local args=(--sock "$(_sock_path)" checkpoint "$label")
  [[ -n "$branch" ]] && args+=(--branch "$branch")
  "$CTL" "${args[@]}" > "$hashfile"
}

cmd_rollback() {
  local label="$1" branch="${2:-}"
  local store hashfile target
  store="$(_store_path)"
  # prefer branch-scoped hashfile, fall back to plain label
  hashfile="$store/hash-$label"
  [[ -n "$branch" && -f "$store/hash-${branch}-$label" ]] && hashfile="$store/hash-${branch}-$label"
  if [[ -f "$hashfile" ]]; then
    target="$(<"$hashfile")"
  else
    target="$label"
  fi
  local args=(--sock "$(_sock_path)" rollback "$target")
  [[ -n "$branch" ]] && args+=(--branch "$branch")
  "$CTL" "${args[@]}" >/dev/null
}

cmd_branch_create() {
  local name="$1" parent="${2:-main}"
  "$CTL" --sock "$(_sock_path)" branch create "$name" --from "$parent" >/dev/null
}

cmd_storage_size() {
  du -sb "$(_store_path)/objects" 2>/dev/null | awk '{print $1}'
}

cmd_unmount() {
  local store; store="$(_store_path)"
  local mount="$store/mnt"
  local pidfile="$store/agentvfs.pid"
  fusermount3 -u "$mount" 2>/dev/null || true
  if [[ -f "$pidfile" ]]; then
    wait "$(<"$pidfile")" 2>/dev/null || true
    rm -f "$pidfile"
  fi
}

case "${1:-}" in
  mount)         cmd_mount "$2" "$3" ;;
  checkpoint)    cmd_checkpoint "$2" "${3:-}" ;;
  rollback)      cmd_rollback "$2" "${3:-}" ;;
  branch-create) cmd_branch_create "$2" "${3:-main}" ;;
  storage_size)  cmd_storage_size ;;
  unmount)       cmd_unmount ;;
  *) echo "usage: $0 <mount|checkpoint|rollback|branch-create|storage_size|unmount> ..." >&2; exit 2 ;;
esac
