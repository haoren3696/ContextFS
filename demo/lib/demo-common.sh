#!/bin/bash
# demo-common.sh — Shared helpers for the AgentVFS conference demo
# Source this from the main demo script.

set -euo pipefail

# ── Tunables ──────────────────────────────────────────────────────────
PACE_SHORT="${PACE_SHORT:-0.8}"
PACE_LONG="${PACE_LONG:-2.5}"
# DEMO_VERBOSE=1 restores the engineer-readable layer: ok_plus's '↳' second
# lines, full /tmp paths, daemon pid, ls -la columns, and raw NDJSON. Default
# 0 produces the clean output used for live talks and public asciinema casts.
# Toggled by --verbose in the main script; honored as an env var too.
DEMO_VERBOSE="${DEMO_VERBOSE:-0}"
DEMO_ROOT="${DEMO_ROOT:-/tmp/agentvfs-demo-$$}"
DEMO_SRC="$DEMO_ROOT/source"
DEMO_STORE="$DEMO_ROOT/store"
DEMO_MNT="$DEMO_ROOT/mount"
DEMO_SOCK="$DEMO_ROOT/control.sock"

AGENTVFS_BIN="${AGENTVFS_BIN:-$(pwd)/build/agentvfs}"
AGENTVFS_CTL="${AGENTVFS_CTL:-$(pwd)/build/agentvfs-ctl}"
AGENTVFS_PRELOAD="${AGENTVFS_PRELOAD:-$(dirname "$AGENTVFS_BIN")/libcas_preload.so}"

DAEMON_PID=""

# ── Colors ────────────────────────────────────────────────────────────
BOLD='\033[1m'
DIM='\033[2m'
BLUE='\033[34m'
GREEN='\033[32m'
YELLOW='\033[33m'
RED='\033[31m'
CYAN='\033[36m'
NC='\033[0m'

# ── Output helpers ────────────────────────────────────────────────────
# section_banner <act_number> <short_title> <subtitle>
# Prints a three-part header: top rule, "Act N of 4 · <title>" + subtitle,
# bottom rule. Total count is hard-coded since the demo has 4 acts.
section_banner() {
    local n="$1" title="$2" subtitle="$3"
    echo "" >&2
    printf "${BLUE}${BOLD}══════════════════════════════════════════════════════════${NC}\n" >&2
    echo "" >&2
    printf "  ${BLUE}${BOLD}Act %s of 4${NC}  ·  ${BOLD}%s${NC}\n" "$n" "$title" >&2
    printf "  ${CYAN}%s${NC}\n" "$subtitle" >&2
    echo "" >&2
    printf "${BLUE}──────────────────────────────────────────────────────────${NC}\n" >&2
    echo "" >&2
}

# closing_banner <title> <subtitle>
# Same rules as section_banner, but without the "Act N of 4" prefix.
# Used for the demo's epilogue.
closing_banner() {
    local title="$1" subtitle="$2"
    echo "" >&2
    printf "${BLUE}${BOLD}══════════════════════════════════════════════════════════${NC}\n" >&2
    echo "" >&2
    printf "  ${BLUE}${BOLD}%s${NC}\n" "$title" >&2
    printf "  ${CYAN}%s${NC}\n" "$subtitle" >&2
    echo "" >&2
    printf "${BLUE}──────────────────────────────────────────────────────────${NC}\n" >&2
    echo "" >&2
}

agent_action() { printf "  ${YELLOW}→ %s${NC}\n" "$1" >&2; }
vfs_response() { printf "  ${CYAN}← %s${NC}\n" "$1" >&2; }
ok()           { printf "  ${GREEN}[OK]${NC} %s\n" "$1" >&2; }
warn()         { printf "  ${RED}[WARN]${NC} %s\n" "$1" >&2; }

# ok_plus prints a confirmation with an optional technical anchor on line 2.
# Both args are expected; missing args render as empty strings rather than
# crashing under `set -u`. The '↳' line is only shown when DEMO_VERBOSE=1 —
# default mode collapses to a single [OK] line for audience readability.
ok_plus() {
    printf "  ${GREEN}[OK]${NC} %s\n" "${1-}" >&2
    if [[ "${DEMO_VERBOSE:-0}" == "1" ]]; then
        printf "       ${CYAN}↳${NC} %s\n" "${2-}" >&2
    fi
}

# ok_suffix prints `[OK] <plain>` plus a suffix in verbose mode only.
# Used for setup banners where verbose adds " at /tmp/..." or " (pid=N, ...)".
ok_suffix() {
    if [[ "${DEMO_VERBOSE:-0}" == "1" ]]; then
        printf "  ${GREEN}[OK]${NC} %s %s\n" "${1-}" "${2-}" >&2
    else
        printf "  ${GREEN}[OK]${NC} %s\n" "${1-}" >&2
    fi
}

# beat_header <n> <total> <title>
# Numbered sub-beat marker for use inside an act. Adds a leading blank
# line so beats stay visually separated from one another.
beat_header() {
    local n="$1" total="$2" title="$3"
    echo "" >&2
    printf "  ${BLUE}${BOLD}[%s/%s]${NC} ${BOLD}%s${NC}\n" "$n" "$total" "$title" >&2
}

# demo_ls — audience-friendly file listing.
# Default: indented `  <size>  <basename>` per file (no owner/perms/timestamp).
# Verbose: today's `ls -la` so engineers see permissions and ownership.
# Accepts files or directories; missing paths are silently skipped (matches
# the prior `2>/dev/null || true` usage at call sites).
demo_ls() {
    if [[ "${DEMO_VERBOSE:-0}" == "1" ]]; then
        ls -la "$@" 2>/dev/null || true
        return
    fi
    local p
    for p in "$@"; do
        [[ -e "$p" ]] || continue
        if [[ -d "$p" ]]; then
            local entry
            for entry in "$p"/*; do
                [[ -e "$entry" ]] || continue
                printf '  %4s  %s\n' "$(du --apparent-size -h "$entry" 2>/dev/null | awk '{print $1}')" "$(basename "$entry")"
            done
        else
            printf '  %4s  %s\n' "$(du --apparent-size -h "$p" 2>/dev/null | awk '{print $1}')" "$(basename "$p")"
        fi
    done
}

# demo_telemetry_event — render one NDJSON line for the audience.
# Default: `    <op>  <path>  pid=N` (just the three fields the audience cares about).
# Verbose: today's raw line, indented 4 spaces. Robust to field order via
# three independent sed extractions instead of one combined regex.
demo_telemetry_event() {
    local line="$1"
    if [[ "${DEMO_VERBOSE:-0}" == "1" ]]; then
        printf '    %s\n' "$line"
        return
    fi
    local op path pid
    op="$(printf '%s' "$line" | sed -nE 's/.*"op":"([^"]+)".*/\1/p')"
    path="$(printf '%s' "$line" | sed -nE 's/.*"path":"([^"]+)".*/\1/p')"
    pid="$(printf '%s' "$line" | sed -nE 's/.*"pid":([0-9]+).*/\1/p')"
    printf '    %-6s %-28s pid=%s\n' "${op:-?}" "${path:-?}" "${pid:-?}"
}

pace()         { sleep "${1:-${PACE_SHORT}}"; }
long_pace()    { sleep "${1:-${PACE_LONG}}"; }

# ── Quickstart visual primitives ──────────────────────────────────────
# These are used by demo/agentvfs-quickstart.sh and may be reused elsewhere.
# They write to stdout (not stderr) so they end up in the VHS-recorded
# terminal exactly as a user would see them.

# banner_snap <line1> <line2>
# Draws a 60-col double-rule banner with two left-justified content lines.
# Pads with explicit space runs (not printf "%-*s") because the content
# can include multi-byte UTF-8 chars (·, →) and printf's width counts
# bytes, not display columns — which would misalign the right border.
banner_snap() {
    local line1="$1" line2="$2"
    local W=60
    local inner=$((W - 4))
    local rule
    rule="$(printf '═%.0s' $(seq 1 $((W-2))))"
    local pad1=$((inner - ${#line1}))
    local pad2=$((inner - ${#line2}))
    (( pad1 < 0 )) && pad1=0
    (( pad2 < 0 )) && pad2=0
    local pad1s="" pad2s=""
    (( pad1 > 0 )) && pad1s="$(printf '%*s' "$pad1" '')"
    (( pad2 > 0 )) && pad2s="$(printf '%*s' "$pad2" '')"
    printf "${BOLD}${GREEN}╔%s╗${NC}\n" "$rule"
    printf "${BOLD}${GREEN}║${NC} %s%s ${BOLD}${GREEN}║${NC}\n" "$line1" "$pad1s"
    printf "${BOLD}${GREEN}║${NC} ${CYAN}%s%s${NC} ${BOLD}${GREEN}║${NC}\n" "$line2" "$pad2s"
    printf "${BOLD}${GREEN}╚%s╝${NC}\n" "$rule"
}

# beat_divider <name>
# Single-line chapter divider: "─── ▸ NAME ─────────────────".
beat_divider() {
    local name="$1"
    echo ""
    printf "${DIM}───${NC} ${BOLD}${GREEN}▸ %s${NC} " "$name"
    local pad=$((60 - 6 - ${#name} - 1))
    if (( pad > 0 )); then
        printf "${DIM}%s${NC}\n" "$(printf '─%.0s' $(seq 1 $pad))"
    else
        echo ""
    fi
}

# type_line <text> [<delay_ms>]
# Char-by-char "typing" animation. Used to render `$ <command>` lines so
# they look like a fast operator typing rather than a printf'd string.
# delay_ms defaults to 25; omit/short-circuit when DEMO_NO_TYPE=1 (set by
# the smoke test so it doesn't sleep its way through CI).
type_line() {
    local text="$1"
    local delay_ms="${2:-25}"
    if [[ "${DEMO_NO_TYPE:-0}" == "1" ]]; then
        printf '%s\n' "$text"
        return
    fi
    local i ch
    local delay_s
    delay_s="$(awk "BEGIN { printf \"%.3f\", $delay_ms/1000 }")"
    for (( i=0; i<${#text}; i++ )); do
        ch="${text:$i:1}"
        printf '%s' "$ch"
        sleep "$delay_s"
    done
    printf '\n'
}

# ── Preflight ─────────────────────────────────────────────────────────
preflight_check() {
    local missing=0

    if [[ ! -x "$AGENTVFS_BIN" ]]; then
        warn "agentvfs binary not found at $AGENTVFS_BIN — build with: cmake -B build && cmake --build build -j"
        missing=1
    fi
    if [[ ! -x "$AGENTVFS_CTL" ]]; then
        warn "agentvfs-ctl not found at $AGENTVFS_CTL"
        missing=1
    fi
    if ! command -v fusermount3 >/dev/null 2>&1; then
        warn "fusermount3 not found — install fuse3"
        missing=1
    fi
    if ! command -v gcc >/dev/null 2>&1; then
        warn "gcc not found — install gcc to build demo project"
        missing=1
    fi
    if ! command -v nc >/dev/null 2>&1; then
        warn "nc (netcat) not found"
        missing=1
    fi
    if [[ "$missing" -ne 0 ]]; then
        die "Preflight failed. Install missing dependencies and rebuild."
    fi

    # cgroup v2 only: the branch router parses 0::<path> lines (cgroup v2 format).
    # A cgroup v1 or hybrid mount would let mkdir succeed but isolation would silently
    # not engage, so insist on the unified hierarchy.
    if [[ "$(stat -fc %T /sys/fs/cgroup 2>/dev/null)" == "cgroup2fs" ]]; then
        DEMO_CGROUP_AVAILABLE=1
    else
        warn "cgroup v2 not mounted at /sys/fs/cgroup — Act 3 (multi-branch isolation) will be skipped"
        DEMO_CGROUP_AVAILABLE=0
    fi

    # The act intros contain Unicode box-drawing chars (─►●┊▼↳ …). A non-UTF-8
    # locale renders them as mojibake — not a runtime failure, but unusable
    # in a recording. Warn (don't fail) so operators can re-export and retry.
    local _locale="${LC_ALL:-${LC_CTYPE:-${LANG:-}}}"
    if [[ "$_locale" != *UTF-8* && "$_locale" != *utf8* ]]; then
        warn "Non-UTF-8 locale ('${_locale:-unset}') — diagrams may render as mojibake. Export LC_ALL=C.UTF-8 before recording."
    fi

    ok "Preflight passed"
}

# ── Setup / teardown ──────────────────────────────────────────────────
setup_demo_env() {
    if [[ -z "$DEMO_ROOT" || "$DEMO_ROOT" == "/" || "${DEMO_ROOT:0:5}" != "/tmp/" ]]; then
        die "DEMO_ROOT is unsafe or not a /tmp path: '$DEMO_ROOT'"
    fi
    rm -rf "$DEMO_ROOT"
    mkdir -p "$DEMO_SRC" "$DEMO_STORE" "$DEMO_MNT"

    cat > "$DEMO_SRC/Makefile" <<'MAKEFILE'
all: hello

hello: main.c
	gcc -o hello main.c

clean:
	rm -f hello
MAKEFILE

    cat > "$DEMO_SRC/main.c" <<'CCODE'
#include <stdio.h>

int main(void) {
    printf("hello, world\n");
    return 0;
}
CCODE

    ok_suffix "Demo environment ready" "at $DEMO_ROOT"
}

# start_daemon — launch the agentvfs daemon in foreground/background.
# All arguments are passed through as separate flags to the daemon
# (e.g., start_daemon --telemetry=ldpreload --telemetry-ldpreload-socket=PATH).
# Runs WITHOUT -s so FUSE serves multi-threaded — this is required to
# demonstrate Act 2 (rollback overlapping a live write).
#
# In verbose mode, stderr is filtered through a process substitution to strip
# the spurious libfuse 3.14 "Ignoring invalid max threads value ..." warning
# (Debian bug #1037410) — harmless but ugly in the recording. In default mode
# stderr is captured to $DEMO_ROOT/daemon.log so the daemon's own startup
# chatter ("agentvfs: control socket at ...") stays off the audience screen.
# On daemon-start failure, die() surfaces the log path so it's still
# diagnosable.
start_daemon() {
    local daemon_log="$DEMO_ROOT/daemon.log"
    if [[ "${DEMO_VERBOSE:-0}" == "1" ]]; then
        "$AGENTVFS_BIN" \
            --source "$DEMO_SRC" \
            --mountpoint "$DEMO_MNT" \
            --store "$DEMO_STORE" \
            --control-sock "$DEMO_SOCK" \
            "$@" \
            -f 2> >(grep --line-buffered -v 'Ignoring invalid max threads value' >&2) &
    else
        "$AGENTVFS_BIN" \
            --source "$DEMO_SRC" \
            --mountpoint "$DEMO_MNT" \
            --store "$DEMO_STORE" \
            --control-sock "$DEMO_SOCK" \
            "$@" \
            -f 2>"$daemon_log" &
    fi
    DAEMON_PID=$!

    # Cold-start can take >5s on slow disks while Bootstrap walks the source.
    for _ in $(seq 1 100); do
        if [[ -S "$DEMO_SOCK" ]] && mountpoint -q "$DEMO_MNT"; then
            ok_suffix "Daemon ready" "(pid=$DAEMON_PID, mount=$DEMO_MNT)"
            return 0
        fi
        sleep 0.1
    done
    die "Daemon did not start within 10s — check $DEMO_SOCK, $DEMO_MNT, and $daemon_log"
}

stop_daemon() {
    if [[ -n "${DAEMON_PID:-}" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        fusermount3 -u "$DEMO_MNT" 2>/dev/null || true
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
        ok "Daemon stopped"
    fi
    rm -rf "$DEMO_ROOT"
}

cleanup() {
    stop_daemon
}
# Trap fires on any exit path (normal, error, signal). Other code paths should
# NOT call cleanup directly — let the trap handle it to avoid double runs.
trap cleanup EXIT

die() {
    printf "${RED}FATAL: %s${NC}\n" "$1" >&2
    exit 1
}

skip_act() {
    printf "${YELLOW}[SKIP]${NC} Act %s: %s\n" "$1" "$2" >&2
}

# ── Control socket helpers ────────────────────────────────────────────
ctl_json() {
    printf '%s\n' "$1" | nc -U -w 2 "$DEMO_SOCK"
}

ctl() {
    "$AGENTVFS_CTL" --sock "$DEMO_SOCK" "$@"
}
