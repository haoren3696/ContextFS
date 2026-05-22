#!/bin/bash
# render-quickstart.sh — Build agentvfs (if needed) and render
# demo/quickstart.gif via VHS.
#
# Usage:
#   ./scripts/render-quickstart.sh
#
# VHS install:
#   go install github.com/charmbracelet/vhs@latest
#   # or: brew install vhs
#   # or: see https://github.com/charmbracelet/vhs#installation
#
# VHS also needs ffmpeg for GIF encoding.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if ! command -v vhs >/dev/null 2>&1; then
    cat >&2 <<'EOF'
render-quickstart.sh: vhs not found.

Install with one of:
  go install github.com/charmbracelet/vhs@latest
  brew install vhs
  https://github.com/charmbracelet/vhs#installation

VHS also requires ffmpeg.
EOF
    exit 127
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "render-quickstart.sh: ffmpeg not found (VHS depends on it)." >&2
    exit 127
fi

# agentvfs must be on PATH for the demo to deploy. Build only if the
# binaries are missing — don't rebuild on every render.
if ! command -v agentvfs >/dev/null 2>&1; then
    if [[ -x "$REPO_ROOT/build/agentvfs" ]]; then
        export PATH="$REPO_ROOT/build:$PATH"
    else
        echo "render-quickstart.sh: agentvfs not on PATH and build/agentvfs missing." >&2
        echo "Run: cmake -B build && cmake --build build -j" >&2
        exit 1
    fi
fi

if ! command -v fusermount3 >/dev/null 2>&1; then
    echo "render-quickstart.sh: fusermount3 not found (FUSE 3 required)." >&2
    exit 1
fi

echo "==> rendering demo/quickstart.gif via vhs"
vhs demo/quickstart.tape

if [[ -f demo/quickstart.gif ]]; then
    size="$(du -h demo/quickstart.gif | cut -f1)"
    echo "==> wrote demo/quickstart.gif ($size)"
else
    echo "render-quickstart.sh: vhs reported success but demo/quickstart.gif is missing." >&2
    exit 1
fi
