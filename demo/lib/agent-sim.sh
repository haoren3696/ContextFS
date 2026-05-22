#!/bin/bash
# agent-sim.sh — Simulated agent functions for the AgentVFS demo
# Source this after demo-common.sh; requires DEMO_MNT to be set.

set -euo pipefail

# ── agent_build_project ───────────────────────────────────────────────
# Simulates an agent creating source files and building.
agent_build_project() {
    agent_action "Agent creates a C project..."
    pace

    cat > "$DEMO_MNT/main.c" <<'CCODE'
#include <stdio.h>

int main(void) {
    printf("hello, world\n");
    return 0;
}
CCODE

    cat > "$DEMO_MNT/Makefile" <<'MAKEFILE'
all: hello

hello: main.c
	gcc -o hello main.c

clean:
	rm -f hello
MAKEFILE

    (cd "$DEMO_MNT" && make >/dev/null 2>&1) || die "agent_build_project: make failed — gcc passed preflight but build did not produce 'hello'"
    demo_ls "$DEMO_MNT"/main.c "$DEMO_MNT"/Makefile "$DEMO_MNT"/hello
    ok "Project built: main.c, Makefile, hello binary"
    pace
}

# ── agent_small_edit ──────────────────────────────────────────────────
# Tiny one-line tweak — used in Act 1 to show that snapshot cost is
# proportional to the change delta, not to the working-tree size.
# Rewrites main.c via heredoc rather than sed -i, because sed -i uses
# a temp-file + rename pattern that triggers a FUSE "preserve
# permissions: Function not implemented" warning on our daemon.
agent_small_edit() {
    agent_action "Agent tweaks the greeting string..."
    pace
    cat > "$DEMO_MNT/main.c" <<'CCODE'
#include <stdio.h>

int main(void) {
    printf("hi, agentvfs!\n");
    return 0;
}
CCODE
    ok "main.c edited (1 line changed)"
    pace
}

# ── agent_add_feature ─────────────────────────────────────────────────
# Simulates an agent adding a useful feature.
agent_add_feature() {
    agent_action "Agent adds a greet() function..."
    pace

    cat >> "$DEMO_MNT/main.c" <<'CCODE'

void greet(void) {
    printf("hi from agentvfs demo!\n");
}
CCODE

    (cd "$DEMO_MNT" && make >/dev/null 2>&1) || true
    ok "Feature added and compiled"
    pace
}

# ── agent_hallucinate_destroy ─────────────────────────────────────────
# Simulates the destructive hallucination: rm build + corrupt source.
agent_hallucinate_destroy() {
    agent_action "Agent hallucinates: deletes build artifacts and corrupts main.c..."
    pace 1.2

    rm -rf "$DEMO_MNT"/hello "$DEMO_MNT"/*.o 2>/dev/null || true
    echo "// GARBAGE — agent overwrote this file by mistake" > "$DEMO_MNT/main.c"
    echo "// This is not recoverable without a rollback" >> "$DEMO_MNT/main.c"

    vfs_response "main.c contents are now garbage:"
    head -2 "$DEMO_MNT/main.c"
    if [[ -e "$DEMO_MNT/hello" ]]; then
        demo_ls "$DEMO_MNT/hello"
        warn "hello still exists"
    else
        echo "  (hello binary is gone)"
    fi
    pace
}

# ── Claude Code rendering ─────────────────────────────────────────────
# These helpers render a Claude-Code-styled panel for the quickstart
# demo's AGENT beat. They are pure rendering — no filesystem mutation,
# no dependency on DEMO_MNT — so they can be reused in any demo that
# needs to depict an agent turn. They write to stdout (not stderr) so
# the lines land in the VHS recording exactly as a viewer would see them.

# claude_code_panel <cwd>
# Draws the Claude Code session header — a rounded box with the
# product name and the current working directory. Matches the visual
# language of the real CLI so a viewer reads "this is Claude Code"
# at a glance.
claude_code_panel() {
    local cwd="$1"
    local W=60
    local rule
    rule="$(printf '─%.0s' $(seq 1 $((W-2))))"
    echo ""
    printf "${BOLD}${CYAN}╭%s╮${NC}\n" "$rule"
    printf "${BOLD}${CYAN}│${NC} ${BOLD}Claude Code${NC}%*s${BOLD}${CYAN}│${NC}\n" $((W-2-1-11)) ""
    printf "${BOLD}${CYAN}│${NC} ${DIM}cwd: %-*s${NC}${BOLD}${CYAN}│${NC}\n" $((W-2-1-5)) "$cwd"
    printf "${BOLD}${CYAN}╰%s╯${NC}\n" "$rule"
    echo ""
}

# claude_prompt <text>
# Renders the user's turn — "> <text>" — with a typed feel via type_line.
claude_prompt() {
    local text="$1"
    printf "${BOLD}>${NC} "
    type_line "$text" 30
}

# claude_tool_call <kind> <args> <output>
# Renders one tool call as Claude Code does:
#   ⏺ Kind(args)
#     ⎿  <line 1 of output>
#        <line 2 of output>   (continuation lines aligned under the first)
# `output` may contain literal newlines — they're split and rendered
# with the continuation indent so multi-line tool output reads cleanly.
claude_tool_call() {
    local kind="$1" args="$2" output="$3"
    echo ""
    printf "${BOLD}${GREEN}⏺${NC} ${BOLD}%s${NC}(${CYAN}%s${NC})\n" "$kind" "$args"
    pace 0.4
    local first=1
    local line
    while IFS= read -r line; do
        if (( first )); then
            printf "  ${DIM}⎿${NC}  %s\n" "$line"
            first=0
        else
            printf "     %s\n" "$line"
        fi
    done <<< "$output"
}

# claude_reply <text>
# Plain-weight final response from the model after tool calls have
# settled. No animation — it just appears, like Claude Code's
# streamed reply settling on screen.
claude_reply() {
    local text="$1"
    echo ""
    printf "%s\n" "$text"
}
