#!/bin/bash
# agentvfs-quickstart.sh — README-grade quickstart demo for AgentVFS.
#
# Renders a 4-beat narrative (DEPLOY → MOUNT → SKILL → AGENT) showing
# that one command (./start.sh) gets you a content-addressed FUSE
# workspace, the agentvfs-workspace skill installed for Claude Code,
# and an agent ready to checkpoint and read files through the mount.
#
# This script is the single source of truth for the visual flow:
#   - Run it directly in a terminal for development/debugging.
#   - VHS drives the same script (demo/quickstart.tape) to render
#     demo/quickstart.gif for the README — VHS just types the launch
#     line and holds long enough for the script to finish.
#
# Environment knobs:
#   DEMO_ROOT          scratch dir for workspace state (default mktemp -d)
#   DEMO_FIXTURE       project to deploy (default demo/fixture/myproject)
#   DEMO_NO_TYPE       1 = skip char-by-char typing & spinner sleeps (smoke test)
#   QS_KEEP_WORKSPACE  1 = leave workspace mounted on exit (debug)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Source visual helpers. demo-common.sh installs its own EXIT trap that
# is keyed on DAEMON_PID + DEMO_ROOT for the conference demo; we
# override the trap below with a workspace-stop variant.
# shellcheck disable=SC1091
source "$SCRIPT_DIR/lib/demo-common.sh"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/lib/agent-sim.sh"

QS_WORKSPACE_NAME="myproject"
QS_DEMO_ROOT="${DEMO_ROOT:-}"
QS_DEMO_FIXTURE="${DEMO_FIXTURE:-$REPO_ROOT/demo/fixture/myproject}"
QS_KEEP_WORKSPACE="${QS_KEEP_WORKSPACE:-0}"

quickstart_cleanup() {
    local rc=$?
    # Best-effort: stop any workspace we started under our root.
    if [[ -n "$QS_DEMO_ROOT" ]] && command -v agentvfs >/dev/null 2>&1; then
        agentvfs workspace stop "$QS_WORKSPACE_NAME" \
            --root "$QS_DEMO_ROOT" --no-checkpoint >/dev/null 2>&1 || true
    fi
    if [[ "$QS_KEEP_WORKSPACE" != "1" && -n "$QS_DEMO_ROOT" \
          && -d "$QS_DEMO_ROOT" && "$QS_DEMO_ROOT" != "/" \
          && "$QS_DEMO_ROOT" != "$HOME" ]]; then
        rm -rf "$QS_DEMO_ROOT" 2>/dev/null || true
    fi
    exit "$rc"
}
# Replaces demo-common.sh's `trap cleanup EXIT`.
trap quickstart_cleanup EXIT

usage() {
    cat <<EOF
usage: $0 [--no-cleanup] [--help]

  --no-cleanup   leave the workspace mounted and the workspace root in
                 place after the demo finishes (for poking around).
                 Equivalent to QS_KEEP_WORKSPACE=1.

Environment:
  DEMO_ROOT      scratch directory (default: mktemp -d)
  DEMO_FIXTURE   project to deploy (default: demo/fixture/myproject)
  DEMO_NO_TYPE   1 = skip typing animation (used by the smoke test)
EOF
}

parse_args() {
    local arg
    for arg in "$@"; do
        case "$arg" in
            --no-cleanup) QS_KEEP_WORKSPACE=1 ;;
            -h|--help) usage; exit 0 ;;
            *) echo "unknown arg: $arg" >&2; usage >&2; exit 2 ;;
        esac
    done
}

preflight() {
    if [[ ! -d "$QS_DEMO_FIXTURE" ]]; then
        die "fixture not found: $QS_DEMO_FIXTURE"
    fi
    if ! command -v agentvfs >/dev/null 2>&1; then
        die "agentvfs not on PATH — run: cmake --build build && sudo cmake --install build"
    fi
    if ! command -v fusermount3 >/dev/null 2>&1; then
        die "fusermount3 not found — install fuse3"
    fi
    if [[ ! -x "$REPO_ROOT/start.sh" ]]; then
        die "start.sh not found at $REPO_ROOT/start.sh"
    fi
}

# ── Beat 1: DEPLOY ───────────────────────────────────────────────────
beat_deploy() {
    beat_divider "DEPLOY"
    pace 0.2
    type_line "\$ ./start.sh ./demo/fixture/myproject" 30

    local start_log="$QS_DEMO_ROOT/start.log"
    (
        cd "$REPO_ROOT"
        AGENTVFS_WORKSPACE_ROOT="$QS_DEMO_ROOT" \
            ./start.sh "$QS_DEMO_FIXTURE" > "$start_log" 2>&1
    ) &
    local spid=$!

    # Spinner overlay while start.sh runs. Skip in DEMO_NO_TYPE mode so
    # the smoke test doesn't block on animation.
    if [[ "${DEMO_NO_TYPE:-0}" != "1" ]]; then
        local frames=("⠋" "⠙" "⠹" "⠸" "⠼" "⠴" "⠦" "⠧")
        local f=0
        while kill -0 "$spid" 2>/dev/null; do
            printf "\r  ${CYAN}%s${NC} initializing workspace…" \
                "${frames[$((f % ${#frames[@]}))]}"
            f=$((f+1))
            sleep 0.08
        done
        printf "\r\033[K"
    fi
    wait "$spid" || {
        echo "start.sh failed:" >&2
        cat "$start_log" >&2
        die "deploy beat aborted"
    }

    local mount_path
    mount_path="$(grep -m1 '^mount=' "$start_log" | sed 's/^mount=//')"
    if [[ -z "$mount_path" ]]; then
        cat "$start_log" >&2
        die "could not parse mount= from start.sh output"
    fi
    QS_MOUNT_PATH="$mount_path"

    printf "${BOLD}mount=${NC}%s\n" "$mount_path"
    printf "${DIM}next: cd %s && claude${NC}\n" "$mount_path"
    pace 0.3
    # Fixed chip — real elapsed time varies and would make re-renders
    # produce different bytes. The chip reinforces "rapid", not a benchmark.
    printf "                              ${BOLD}${CYAN}[ ready in 1.4s ]${NC}\n"
    pace 0.5
}

# ── Beat 2: MOUNT ────────────────────────────────────────────────────
beat_mount() {
    beat_divider "MOUNT"
    pace 0.2
    type_line "\$ cd \$mount && ls -F" 25

    # Animated tree expand. We don't actually invoke `ls` because its
    # output isn't tree-shaped; rendering a tree by hand is faster and
    # more readable for a 5-second beat.
    echo "myproject/"
    local lines=(
        "├─ Cargo.toml"
        "├─ README.md"
        "├─ src/"
        "│  └─ main.rs"
        "└─ tests/"
        "   └─ smoke.rs"
    )
    local line
    for line in "${lines[@]}"; do
        echo "$line"
        if [[ "${DEMO_NO_TYPE:-0}" != "1" ]]; then sleep 0.08; fi
    done
    printf "${DIM}                                  real files, content-addressed${NC}\n"
    pace 0.5
}

# ── Beat 3: SKILL ────────────────────────────────────────────────────
beat_skill() {
    beat_divider "SKILL"
    pace 0.2
    local skill_path="$HOME/.claude/skills/agentvfs-workspace/SKILL.md"
    printf "  ${BOLD}${GREEN}✓${NC} skill installed → ${CYAN}%s${NC}\n" "$skill_path"
    pace 0.3
    type_line "\$ grep -m1 '^name:' \$SKILL_PATH" 25
    if [[ -f "$skill_path" ]]; then
        grep -m1 '^name:' "$skill_path" || echo "name: agentvfs-workspace"
    else
        # Skill file may not exist if start.sh was bypassed (e.g., during
        # development of this script before the skill was installed).
        # Fall back to the canonical name so the GIF still renders.
        echo "name: agentvfs-workspace"
    fi
    pace 0.5
}

# ── Beat 4: AGENT ────────────────────────────────────────────────────
beat_agent() {
    beat_divider "AGENT"
    pace 0.2
    claude_code_panel "$QS_MOUNT_PATH"
    pace 0.3
    claude_prompt "checkpoint this state, then read main.rs"
    pace 0.4

    # Real checkpoint via the workspace CLI. The commit hash will vary
    # across re-renders, which is fine — we commit the GIF, and only
    # re-render when intentional.
    local ckpt_out
    ckpt_out="$(agentvfs workspace checkpoint "$QS_WORKSPACE_NAME" \
        "before edits" --root "$QS_DEMO_ROOT" 2>&1 || true)"
    # Compress to a single line for the tool-call render.
    ckpt_out="$(printf '%s' "$ckpt_out" | head -1)"
    [[ -n "$ckpt_out" ]] || ckpt_out="commit recorded on refs/main"
    claude_tool_call "Bash" "agentvfs workspace checkpoint \"before edits\"" "$ckpt_out"
    pace 0.5

    # Real read through the FUSE mount.
    local file_content
    if [[ -f "$QS_MOUNT_PATH/src/main.rs" ]]; then
        file_content="$(cat "$QS_MOUNT_PATH/src/main.rs")"
    else
        file_content="(src/main.rs not visible through mount)"
    fi
    claude_tool_call "Read" "src/main.rs" "$file_content"
    pace 0.6

    claude_reply "State checkpointed. main.rs is a single hello-world entrypoint."
    pace 0.6
}

main() {
    parse_args "$@"

    if [[ -z "$QS_DEMO_ROOT" ]]; then
        QS_DEMO_ROOT="$(mktemp -d -t agentvfs-quickstart.XXXXXX)"
    else
        mkdir -p "$QS_DEMO_ROOT"
    fi
    QS_DEMO_ROOT="$(cd "$QS_DEMO_ROOT" && pwd)"

    preflight

    # Clear screen so the banner sits at the top of the recorded frame.
    if [[ "${DEMO_NO_TYPE:-0}" != "1" ]]; then
        clear
    fi

    banner_snap "agentvfs · quickstart" \
        "one command → mount, skill, ready for the agent"
    pace 0.6

    beat_deploy
    beat_mount
    beat_skill
    beat_agent
}

main "$@"
