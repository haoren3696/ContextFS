#!/usr/bin/env bash
# benchmarks/agent-sim/run.sh
set -uo pipefail   # NOT -e: per-method failures must not abort the run
(( BASH_VERSINFO[0] >= 5 )) || { echo "bash >= 5.0 required (have $BASH_VERSION)" >&2; exit 2; }

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO/benchmarks/agent-sim"
DRIVERS="$HERE/drivers"

QUICK_MODE="${QUICK_MODE:-0}"
SCALE="${SCALE:-1}"
BRANCHES="${BRANCHES:-0}"
AGENT_SIM_TMP="${AGENT_SIM_TMP:-/tmp}"
mkdir -p "$AGENT_SIM_TMP"
AGENT_SIM_TMP="$(cd "$AGENT_SIM_TMP" && pwd)"
if (( QUICK_MODE )); then
  TOTAL_Q=40
else
  TOTAL_Q=$((1000 * SCALE))
fi
CKPT_EVERY=20
BLOB_EVERY=50

RUN_ID="$(date +%Y%m%d-%H%M%S)-$(printf '%04x' $RANDOM)"
RESULTS="$HERE/results/$RUN_ID"
mkdir -p "$RESULTS"

# --- env.txt --------------------------------------------------------------
{
  echo "run_id=$RUN_ID"
  echo "date=$(date -Is)"
  echo "kernel=$(uname -srm)"
  echo "cpu=$(awk -F: '/model name/ {print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"
  echo "ram_bytes=$(awk '/MemTotal/ {print $2*1024}' /proc/meminfo)"
  echo "fusermount3=$(fusermount3 --version 2>&1 | head -1)"
  echo "bash=$BASH_VERSION"
  echo "agentvfs_sha=$(git -C "$REPO" rev-parse --verify HEAD 2>/dev/null || echo unknown)"
  echo "agentfs_sha=$(git -C "$REPO/baselines/agentfs" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "branchfs_sha=$(git -C "$REPO/baselines/branchfs" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "quick_mode=$QUICK_MODE"
  echo "scale=$SCALE"
  echo "branches=$BRANCHES"
  echo "total_q=$TOTAL_Q"
  echo "ckpt_every=$CKPT_EVERY"
  echo "blob_every=$BLOB_EVERY"
} > "$RESULTS/env.txt"

# --- CSV headers ---------------------------------------------------------
echo "method,label,q,ckpt_us,store_bytes_after,status"           > "$RESULTS/perop.csv"
echo "method,target_label,rollback_us,status"                    > "$RESULTS/rollback.csv"
echo "method,total_wall_s,n_checkpoints_ok,n_checkpoints_failed,peak_store_bytes" > "$RESULTS/totals.csv"
echo "method,branch_name,create_us,status"                       > "$RESULTS/branch.csv"

EXIT_CODE=0
_TMPDIRS=()
_cleanup() {
  trap '' INT TERM   # ignore signals during cleanup
  for d in "${_TMPDIRS[@]}"; do
    fusermount3 -uz "$d/store/mnt" 2>/dev/null || true
    rm -rf "$d" 2>/dev/null || true
  done
}
trap _cleanup EXIT INT TERM

# --- workload loop --------------------------------------------------------
_die_if_mount_dead() {
  local mount="$1" logfile="$2"
  if ! mountpoint -q "$mount" 2>/dev/null; then
    echo "agent-sim: mount $mount lost (daemon crashed?) — aborting method" >&2
    echo "agent-sim: mount $mount lost" >> "$logfile"
    return 1
  fi
}

run_workload() {
  local method="$1" driver="$2" mount="$3" logfile="$4" branch="${5:-}"
  mkdir -p "$mount/src" "$mount/build"
  local q slot fname label t0 t1 ckpt_us sz status
  for q in $(seq 0 $((TOTAL_Q - 1))); do
    for slot in 0 1 2; do
      fname="src/file_$(( (q + slot) % 50 )).txt"
      mkdir -p "$mount/src" 2>/dev/null || true
      if ! seq 1 100 | awk -v q="$q" -v s="$slot" \
        '{printf "Q%d file%d line%d\n", q, s, $1}' > "$mount/$fname" 2>/dev/null; then
        _die_if_mount_dead "$mount" "$logfile" || return 1
      fi
    done
    if (( (q + 1) % BLOB_EVERY == 0 )); then
      mkdir -p "$mount/build" 2>/dev/null || true
      if ! head -c 1048576 /dev/urandom > "$mount/build/q${q}.bin" 2>/dev/null; then
        _die_if_mount_dead "$mount" "$logfile" || return 1
      fi
    fi
    if (( (q + 1) % CKPT_EVERY == 0 )); then
      label="q$(printf '%03d' "$q")"
      [[ -n "$branch" ]] && label="${branch}-${label}"
      t0=$EPOCHREALTIME
      if [[ -n "$branch" && "$method" == "agentvfs" ]]; then
        bash "$driver" checkpoint "$label" "$branch" 2>>"$logfile" && status=ok || status="fail:checkpoint"
      else
        bash "$driver" checkpoint "$label" 2>>"$logfile" && status=ok || status="fail:checkpoint"
      fi
      if [[ "$status" == "ok" ]]; then
        t1=$EPOCHREALTIME
        ckpt_us=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%d", (b-a)*1e6}')
      else
        ckpt_us=""; EXIT_CODE=1
      fi
      sz=$(bash "$driver" storage_size 2>>"$logfile" || echo "")
      echo "$method,$label,$q,$ckpt_us,$sz,$status" >> "$RESULTS/perop.csv"
    fi
  done
}

# --- rollback sampler -----------------------------------------------------
sample_rollback() {
  local method="$1" driver="$2" logfile="$3" labels=("${@:4}")
  local label t0 t1 us status
  for label in "${labels[@]}"; do
    t0=$EPOCHREALTIME
    local rb_ok
    if [[ "$method" == "agentvfs" && "$label" == *-* ]]; then
      bash "$driver" rollback "$label" "${label%%-*}" 2>>"$logfile" && rb_ok=1 || rb_ok=0
    else
      bash "$driver" rollback "$label" 2>>"$logfile" && rb_ok=1 || rb_ok=0
    fi
    if (( rb_ok )); then
      t1=$EPOCHREALTIME
      us=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%d", (b-a)*1e6}')
      status=ok
    else
      us=""; status="fail:rollback"; EXIT_CODE=1
    fi
    echo "$method,$label,$us,$status" >> "$RESULTS/rollback.csv"
  done
}

# --- per-method runner ---------------------------------------------------
run_one_method() {
  local method="$1"
  local driver="$DRIVERS/$method.sh"
  local logfile="$RESULTS/$method.log"
  echo "==> $method" >&2

  local tmp; tmp="$(mktemp -d "$AGENT_SIM_TMP/agent-sim-XXXXXX")"
  _TMPDIRS+=("$tmp")
  local workspace="$tmp/workspace"
  local store="$tmp/store"
  mkdir -p "$workspace" "$store"

  case "$method" in
    agentvfs) export AGENTVFS_STORE="$store" ;;
    agentfs)  export AGENTFS_STORE="$store"  ;;
    branchfs) export BRANCHFS_STORE="$store" ;;
  esac

  local m_start=$EPOCHREALTIME
  local mount
  if ! mount="$(bash "$driver" mount "$workspace" "$store" 2>>"$logfile")"; then
    echo "FAIL: $method mount" >&2
    EXIT_CODE=1
    return
  fi

  if (( BRANCHES > 0 )); then
    run_branched "$method" "$driver" "$mount" "$logfile"
  else
    if run_workload "$method" "$driver" "$mount" "$logfile"; then
      sample_rollback "$method" "$driver" "$logfile" \
        "$(printf 'q%03d' $((CKPT_EVERY - 1)))" \
        "$(printf 'q%03d' $((TOTAL_Q - CKPT_EVERY - 1)))" \
        "$(printf 'q%03d' $((TOTAL_Q - 1)))"
    fi
  fi

  bash "$driver" unmount 2>>"$logfile" || EXIT_CODE=1
  fusermount3 -uz "$store/mnt" 2>/dev/null || true

  local m_end=$EPOCHREALTIME
  local total_wall_s n_ok n_fail peak
  total_wall_s=$(awk -v a="$m_start" -v b="$m_end" 'BEGIN{printf "%.3f", b-a}')
  n_ok=$(awk -F, -v m="$method" '$1==m && $6=="ok"  {c++} END{print c+0}'              "$RESULTS/perop.csv")
  n_fail=$(awk -F, -v m="$method" '$1==m && $6!="ok" && NR>1 {c++} END{print c+0}'     "$RESULTS/perop.csv")
  peak=$(awk -F, -v m="$method" '$1==m && $5!="" && ($5+0)>p {p=$5} END{print p+0}'     "$RESULTS/perop.csv")
  echo "$method,$total_wall_s,$n_ok,$n_fail,$peak" >> "$RESULTS/totals.csv"

  rm -rf "$tmp"
}

# --- branched mode --------------------------------------------------------
last_ckpt_q() { echo $(( ($1 / CKPT_EVERY) * CKPT_EVERY - 1 )); }

run_branched() {
  local method="$1" driver="$2" mount="$3" logfile="$4"

  local warmup_q=$(( TOTAL_Q * 25 / 100 ))            # 25% on main
  local per_branch_q=$(( (TOTAL_Q - warmup_q) / BRANCHES ))

  # Phase 1: warm-up on main
  local saved_total=$TOTAL_Q
  TOTAL_Q=$warmup_q
  if ! run_workload "$method" "$driver" "$mount" "$logfile" ""; then
    TOTAL_Q=$saved_total
    return
  fi
  TOTAL_Q=$saved_total

  # Phase 2+3: create branches + work on each
  local b bname t0 t1 create_us
  local rb_labels=()
  for b in $(seq 1 $BRANCHES); do
    bname="b$(printf '%02d' "$b")"

    # create branch
    t0=$EPOCHREALTIME
    if bash "$driver" branch-create "$bname" 2>>"$logfile"; then
      t1=$EPOCHREALTIME
      create_us=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%d", (b-a)*1e6}')
      echo "$method,$bname,$create_us,ok" >> "$RESULTS/branch.csv"
    else
      echo "$method,$bname,,fail:branch-create" >> "$RESULTS/branch.csv"
      EXIT_CODE=1
      continue
    fi

    # for agentfs pseudo-branches: rollback to branch baseline first
    if [[ "$method" == "agentfs" ]]; then
      bash "$driver" rollback "__branch-${bname}__" 2>>"$logfile" || true
    fi

    TOTAL_Q=$per_branch_q
    if ! run_workload "$method" "$driver" "$mount" "$logfile" "$bname"; then
      TOTAL_Q=$saved_total
      return
    fi
    TOTAL_Q=$saved_total

    # collect last-checkpoint label from this branch
    rb_labels+=("$(printf '%s-q%03d' "$bname" "$(last_ckpt_q "$per_branch_q")")")
  done

  # rollback sampler: last warmup ckpt + first/last branch endpoints
  local warmup_label="$(printf 'q%03d' "$(last_ckpt_q "$warmup_q")")"
  local all_labels=("$warmup_label")
  if (( ${#rb_labels[@]} >= 2 )); then
    all_labels+=("${rb_labels[0]}" "${rb_labels[-1]}")
  else
    all_labels+=("${rb_labels[@]}")
  fi
  sample_rollback "$method" "$driver" "$logfile" "${all_labels[@]}"
}

# --- main -----------------------------------------------------------------
for m in agentvfs agentfs branchfs; do
  run_one_method "$m"
done

awk -v results="$RESULTS" -f "$HERE/lib/summary.awk" > "$RESULTS/summary.md"
echo "results: $RESULTS"
echo "summary: $RESULTS/summary.md"
exit $EXIT_CODE
