# README and Install UX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `install.sh` + `install.ps1` (one-line installers that fetch GitHub Releases prebuilts), a `release.yml` workflow that publishes those artifacts on tag push, and a restructured README whose hero is two commands (install, then mount) — so a first-time visitor can mount a project in under 30 seconds.

**Architecture:** Both installers implement the same contract — detect host OS+arch, resolve version, download archive + checksums file from GitHub Releases, verify SHA-256, probe FUSE runtime host dep (warn but don't auto-install), extract `agentvfs` / `agentvfs-ctl` (+ `agentvfs-quickstart` on Linux/macOS) to `~/.local/bin`. `release.yml` builds five platform tuples on tag push and creates a release with checksums. README is restructured so the hero is install + mount; the existing cmake quickstarts become a demoted "Build from source" section. `start.sh` is unchanged in the repo; the release workflow copies it to `agentvfs-quickstart` when packaging.

**Tech Stack:** POSIX `sh` for `install.sh`, PowerShell 5+ for `install.ps1`, GitHub Actions for `release.yml`, `sha256sum` / `curl` / `gh` CLI in the publish path. Tests use bash + local `file://` URL fixtures.

**Spec:** `docs/superpowers/specs/2026-05-23-readme-and-install-ux-design.md`

---

## File structure

**New files:**
- `install.sh` — POSIX sh, root of repo. The Linux/macOS installer.
- `install.ps1` — PowerShell 5+, root of repo. The Windows installer.
- `.github/workflows/release.yml` — GitHub Actions workflow triggered on `v*` tag push.
- `tests/install/build_fixture.sh` — bash, builds fake release archives + a `checksums.txt` for local install.sh tests.
- `tests/install/test_install_sh.sh` — bash, integration test runner for install.sh.

**Modified files:**
- `README.md` — restructured per spec § README restructure.

**Unchanged but referenced:**
- `start.sh` — copied verbatim into release archives under the name `agentvfs-quickstart`.

**Test scope:** install.sh is exercised end-to-end via `tests/install/test_install_sh.sh` against local file:// URL fixtures. install.ps1 is smoke-tested manually on a Windows runner (no in-repo automated tests; the release workflow's matrix run is the regression net). release.yml is validated by `gh workflow run` against a `v0.1.0-rc1` tag before cutting v0.1.0.

**Testability escape hatch:** install.sh and install.ps1 honor `AGENTVFS_INSTALL_URL_BASE` to override the GitHub Releases URL (defaults to `https://github.com/thustorage/ContextFS/releases/download`). Tests set this to `file://$(pwd)/tests/install/fixtures` and `AGENTVFS_INSTALL_VERSION=v0.0.0-test`. Document the env var in `install.sh --help` under a "Testing" subsection.

---

## Task 1: Test scaffolding and install.sh skeleton

**Files:**
- Create: `tests/install/build_fixture.sh`
- Create: `tests/install/test_install_sh.sh`
- Create: `install.sh`

- [ ] **Step 1.1: Write the fixture builder**

Create `tests/install/build_fixture.sh`:

```bash
#!/bin/bash
# Builds fake release archives at tests/install/fixtures/<version>/.
# Used by test_install_sh.sh; install.sh reads them via AGENTVFS_INSTALL_URL_BASE=file://...
set -euo pipefail

VERSION="${1:-v0.0.0-test}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/fixtures/$VERSION"

rm -rf "$OUT"
mkdir -p "$OUT"

TUPLES=(linux-x86_64 darwin-arm64 darwin-x86_64)
WIN_TUPLE=windows-x86_64

make_payload() {
    local dir="$1"
    cat >"$dir/agentvfs" <<'EOF'
#!/bin/sh
echo "fake agentvfs"
EOF
    cat >"$dir/agentvfs-ctl" <<'EOF'
#!/bin/sh
echo "fake agentvfs-ctl"
EOF
    cat >"$dir/agentvfs-quickstart" <<'EOF'
#!/bin/sh
echo "fake agentvfs-quickstart"
EOF
    chmod +x "$dir/agentvfs" "$dir/agentvfs-ctl" "$dir/agentvfs-quickstart"
    printf 'fake license\n' >"$dir/LICENSE"
}

for tuple in "${TUPLES[@]}"; do
    work="$(mktemp -d)"
    make_payload "$work"
    tar -C "$work" -czf "$OUT/agentvfs-$VERSION-$tuple.tar.gz" \
        agentvfs agentvfs-ctl agentvfs-quickstart LICENSE
    rm -rf "$work"
done

# Windows: zip with no agentvfs-quickstart.
work="$(mktemp -d)"
make_payload "$work"
rm "$work/agentvfs-quickstart"
mv "$work/agentvfs" "$work/agentvfs.exe"
mv "$work/agentvfs-ctl" "$work/agentvfs-ctl.exe"
(cd "$work" && zip -q "$OUT/agentvfs-$VERSION-$WIN_TUPLE.zip" agentvfs.exe agentvfs-ctl.exe LICENSE)
rm -rf "$work"

# Compute checksums. Use a .tmp suffix so the redirect target isn't
# itself matched by the glob (and hashed as an empty file).
(cd "$OUT" && sha256sum \
    "agentvfs-$VERSION-linux-x86_64.tar.gz" \
    "agentvfs-$VERSION-darwin-arm64.tar.gz" \
    "agentvfs-$VERSION-darwin-x86_64.tar.gz" \
    "agentvfs-$VERSION-windows-x86_64.zip" \
    > "agentvfs-$VERSION-checksums.txt")

echo "Fixture built at $OUT"
```

Mark it executable: `chmod +x tests/install/build_fixture.sh`.

- [ ] **Step 1.2: Write the test runner (failing)**

Create `tests/install/test_install_sh.sh`:

```bash
#!/bin/bash
# Integration tests for install.sh. Builds fixtures, points install.sh at
# them via AGENTVFS_INSTALL_URL_BASE=file://..., asserts behavior.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INSTALLER="$REPO_ROOT/install.sh"
TEST_DIR="$REPO_ROOT/tests/install"
VERSION="v0.0.0-test"

bash "$TEST_DIR/build_fixture.sh" "$VERSION"

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok: $*"; }

# Test 1: --help exits 0 and prints "Usage:".
out="$($INSTALLER --help 2>&1)" || fail "--help exited non-zero"
grep -q "Usage:" <<<"$out" || fail "--help missing 'Usage:' (got: $out)"
ok "--help"

echo "PASS"
```

Mark it executable: `chmod +x tests/install/test_install_sh.sh`.

- [ ] **Step 1.3: Run the test, verify it fails**

```
bash tests/install/test_install_sh.sh
```

Expected: FAIL because `install.sh` doesn't exist yet.

- [ ] **Step 1.4: Write the install.sh skeleton**

Create `install.sh` at the repo root:

```sh
#!/bin/sh
# install.sh — installs the agentvfs prebuilt to ~/.local/bin (default).
# Usage: curl -fsSL https://raw.githubusercontent.com/thustorage/ContextFS/main/install.sh | sh
set -eu

DEFAULT_URL_BASE="https://github.com/thustorage/ContextFS/releases/download"
URL_BASE="${AGENTVFS_INSTALL_URL_BASE:-$DEFAULT_URL_BASE}"
VERSION="${AGENTVFS_INSTALL_VERSION:-}"
PREFIX="${AGENTVFS_INSTALL_PREFIX:-$HOME/.local/bin}"
SYSTEM=0
YES=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: install.sh [--version <tag>] [--prefix <dir>] [--system] [--yes]
                  [--dry-run] [--help]

Installs the agentvfs prebuilt binary, agentvfs-ctl, and agentvfs-quickstart
to PREFIX. Default PREFIX is ~/.local/bin (no sudo). --system installs to
/usr/local/bin (sudo prompted).

Environment:
  AGENTVFS_INSTALL_VERSION  pin a release tag (e.g. v0.1.0)
  AGENTVFS_INSTALL_PREFIX   override install destination
  AGENTVFS_INSTALL_URL_BASE override the release URL base (testing)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        --prefix)  PREFIX="$2"; shift 2 ;;
        --prefix=*) PREFIX="${1#--prefix=}"; shift ;;
        --system)  SYSTEM=1; shift ;;
        --yes|-y)  YES=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "install.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

# Subsequent tasks fill these in.
echo "install.sh skeleton: PREFIX=$PREFIX VERSION=${VERSION:-latest} URL_BASE=$URL_BASE SYSTEM=$SYSTEM YES=$YES DRY_RUN=$DRY_RUN"
```

Mark it executable: `chmod +x install.sh`.

- [ ] **Step 1.5: Run the test, verify it passes**

```
bash tests/install/test_install_sh.sh
```

Expected: PASS, prints `ok: --help` and `PASS`.

- [ ] **Step 1.6: Commit**

```bash
git add tests/install/build_fixture.sh tests/install/test_install_sh.sh install.sh
git commit -m "feat(install): scaffold install.sh + test fixtures

Adds an empty install.sh that handles --help/--dry-run/flag parsing
plus the fixture builder and integration test runner that future
tasks will extend.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Host detection

**Files:**
- Modify: `install.sh`
- Modify: `tests/install/test_install_sh.sh`

- [ ] **Step 2.1: Write the failing test**

Append to `tests/install/test_install_sh.sh` before the `echo "PASS"` line:

```bash
# Test 2: --dry-run prints a host tuple line.
out="$(AGENTVFS_INSTALL_VERSION=$VERSION \
       AGENTVFS_INSTALL_URL_BASE=file://$TEST_DIR/fixtures \
       $INSTALLER --dry-run --yes 2>&1)" || fail "--dry-run exited non-zero ($out)"
grep -qE "host=(linux|darwin)-(x86_64|aarch64|arm64)" <<<"$out" \
    || fail "--dry-run missing host= line (got: $out)"
ok "--dry-run host detection"
```

- [ ] **Step 2.2: Run the test, verify it fails**

```
bash tests/install/test_install_sh.sh
```

Expected: FAIL at "Test 2" because install.sh doesn't print `host=`.

- [ ] **Step 2.3: Implement host detection in install.sh**

In `install.sh`, replace the final `echo "install.sh skeleton: ..."` line with:

```sh
detect_host() {
    os="$(uname -s | tr '[:upper:]' '[:lower:]')"
    arch="$(uname -m)"
    case "$os" in
        linux)  os=linux ;;
        darwin) os=darwin ;;
        *) echo "install.sh: unsupported OS '$os' — build from source: https://github.com/thustorage/ContextFS#build-from-source" >&2; exit 1 ;;
    esac
    case "$arch" in
        x86_64|amd64) arch=x86_64 ;;
        aarch64|arm64)
            if [ "$os" = darwin ]; then arch=arm64; else arch=aarch64; fi
            ;;
        *) echo "install.sh: unsupported arch '$arch' — build from source" >&2; exit 1 ;;
    esac
    # v0 supports linux-x86_64, darwin-arm64, darwin-x86_64. aarch64 Linux is out.
    case "$os-$arch" in
        linux-x86_64|darwin-arm64|darwin-x86_64) : ;;
        linux-aarch64)
            echo "install.sh: aarch64 Linux prebuilt not yet published — build from source" >&2; exit 1 ;;
        *) echo "install.sh: no prebuilt for $os-$arch — build from source" >&2; exit 1 ;;
    esac
    HOST_OS="$os"
    HOST_ARCH="$arch"
    HOST_TUPLE="$os-$arch"
}

detect_host
echo "host=$HOST_TUPLE"

if [ "$DRY_RUN" = 1 ]; then
    echo "version=${VERSION:-latest}"
    echo "url_base=$URL_BASE"
    echo "prefix=$PREFIX"
    echo "system=$SYSTEM"
    echo "dry-run: exit 0"
    exit 0
fi
```

- [ ] **Step 2.4: Run the test, verify it passes**

```
bash tests/install/test_install_sh.sh
```

Expected: PASS.

- [ ] **Step 2.5: Commit**

```bash
git add install.sh tests/install/test_install_sh.sh
git commit -m "feat(install): host OS+arch detection

uname-based detection mapped to the four supported tuples
(linux-x86_64, darwin-arm64, darwin-x86_64). aarch64 Linux and
unknown OSes exit with a clear 'build from source' message.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Version resolution

**Files:**
- Modify: `install.sh`
- Modify: `tests/install/test_install_sh.sh`

This task adds the latest-version resolution path. The automated test exercises only `--version` pinning (a regression check that the new code path doesn't break the pinned-version flow). The "default = latest from GitHub API" path is covered by Task 11's end-to-end smoke test, since faithfully mocking the GitHub API offline adds more machinery than it's worth.

- [ ] **Step 3.1: Write the regression test**

Append to `tests/install/test_install_sh.sh` before `echo "PASS"`:

```bash
# Test 3: --version pinning still works (regression check for Task 3).
out="$(AGENTVFS_INSTALL_URL_BASE=file://$TEST_DIR/fixtures \
       $INSTALLER --version $VERSION --dry-run --yes 2>&1)" \
    || fail "--version --dry-run exited non-zero ($out)"
grep -q "version=$VERSION" <<<"$out" \
    || fail "version line should pin $VERSION (got: $out)"
ok "--version pinning"
```

- [ ] **Step 3.2: Run the test, confirm baseline**

```
bash tests/install/test_install_sh.sh
```

Expected: PASS for all three tests. The new test passes today because the dry-run flow from Task 2 already echoes `version=${VERSION:-latest}` and `$VERSION` is set by `--version`. Confirming this baseline gives us a regression guard for the next step.

- [ ] **Step 3.3: Implement latest-version resolution (for non-pinned default)**

If a user invokes `install.sh` without `--version` or `AGENTVFS_INSTALL_VERSION`, the script must resolve "latest" from the GitHub Releases API. Add this function in `install.sh` after `detect_host()`:

```sh
resolve_version() {
    if [ -n "$VERSION" ]; then return; fi
    api_url="https://api.github.com/repos/thustorage/ContextFS/releases/latest"
    if command -v curl >/dev/null; then
        VERSION="$(curl -fsSL "$api_url" | grep -m1 '"tag_name"' | sed -E 's/.*"tag_name":[[:space:]]*"([^"]+)".*/\1/')"
    elif command -v wget >/dev/null; then
        VERSION="$(wget -qO- "$api_url" | grep -m1 '"tag_name"' | sed -E 's/.*"tag_name":[[:space:]]*"([^"]+)".*/\1/')"
    else
        echo "install.sh: need curl or wget on PATH" >&2; exit 1
    fi
    if [ -z "$VERSION" ]; then
        echo "install.sh: could not resolve latest release; pin with --version vX.Y.Z" >&2; exit 1
    fi
}
```

Call `resolve_version` immediately after `detect_host` and before the `DRY_RUN` block. Replace `version=${VERSION:-latest}` with `version=$VERSION` in the dry-run output.

- [ ] **Step 3.4: Run the test, verify it passes**

```
bash tests/install/test_install_sh.sh
```

Expected: PASS for all three tests. (The version-resolution call returns early when `--version` is provided, so the test fixture doesn't hit github.com.)

- [ ] **Step 3.5: Commit**

```bash
git add install.sh tests/install/test_install_sh.sh
git commit -m "feat(install): version resolution

Default: query the GitHub Releases API for the latest tag.
Override: --version v0.1.0 or AGENTVFS_INSTALL_VERSION=v0.1.0
(skips the API call). Works with either curl or wget.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Download and SHA-256 verification

**Files:**
- Modify: `install.sh`
- Modify: `tests/install/test_install_sh.sh`

- [ ] **Step 4.1: Write the failing test**

Append to `tests/install/test_install_sh.sh` before `echo "PASS"`:

```bash
# Test 4: a tampered archive is rejected at checksum verification.
tampered_dir="$TEST_DIR/fixtures/v0.0.0-tampered"
mkdir -p "$tampered_dir"
# Copy legit archives but rewrite checksums.txt to all-zeroes.
cp "$TEST_DIR/fixtures/$VERSION"/*.tar.gz "$tampered_dir/" 2>/dev/null || true
cp "$TEST_DIR/fixtures/$VERSION"/*.zip    "$tampered_dir/" 2>/dev/null || true
for f in "$tampered_dir"/agentvfs-v0.0.0-test-*.{tar.gz,zip}; do
    [ -f "$f" ] || continue
    base="$(basename "$f" | sed "s/v0.0.0-test/v0.0.0-tampered/")"
    mv "$f" "$tampered_dir/$base"
done
# Write garbage checksums.
{
    for f in "$tampered_dir"/agentvfs-v0.0.0-tampered-*.{tar.gz,zip}; do
        [ -f "$f" ] || continue
        printf '0000000000000000000000000000000000000000000000000000000000000000  %s\n' \
            "$(basename "$f")"
    done
} > "$tampered_dir/agentvfs-v0.0.0-tampered-checksums.txt"

prefix="$(mktemp -d)"
set +e
out="$(AGENTVFS_INSTALL_URL_BASE=file://$TEST_DIR/fixtures \
       AGENTVFS_INSTALL_VERSION=v0.0.0-tampered \
       AGENTVFS_INSTALL_PREFIX="$prefix" \
       $INSTALLER --yes 2>&1)"
rc=$?
set -e
[ $rc -ne 0 ] || fail "tampered checksum should have caused non-zero exit"
grep -q "checksum mismatch\|verification failed" <<<"$out" \
    || fail "expected checksum error (got: $out)"
[ ! -f "$prefix/agentvfs" ] || fail "tampered archive should not have been installed"
rm -rf "$prefix" "$tampered_dir"
ok "checksum verification rejects tampered archive"
```

- [ ] **Step 4.2: Run the test, verify it fails**

```
bash tests/install/test_install_sh.sh
```

Expected: FAIL because download + verify code doesn't exist yet (the installer doesn't actually download or check anything beyond the dry-run path).

- [ ] **Step 4.3: Implement download + verify in install.sh**

After `resolve_version()` and before the `DRY_RUN` block, add:

```sh
download() {
    url="$1"; dest="$2"
    if command -v curl >/dev/null; then
        curl -fsSL --retry 3 --retry-delay 2 -o "$dest" "$url" || {
            echo "install.sh: download failed: $url" >&2; exit 1; }
    elif command -v wget >/dev/null; then
        wget -q --tries=3 -O "$dest" "$url" || {
            echo "install.sh: download failed: $url" >&2; exit 1; }
    else
        echo "install.sh: need curl or wget on PATH" >&2; exit 1
    fi
}

verify_sha256() {
    archive="$1"; checksums="$2"
    expected="$(grep -E "  $(basename "$archive")\$" "$checksums" \
                | awk '{print $1}')"
    if [ -z "$expected" ]; then
        echo "install.sh: $(basename "$archive") not listed in checksums" >&2; exit 1
    fi
    if command -v sha256sum >/dev/null; then
        actual="$(sha256sum "$archive" | awk '{print $1}')"
    elif command -v shasum >/dev/null; then
        actual="$(shasum -a 256 "$archive" | awk '{print $1}')"
    else
        echo "install.sh: need sha256sum or shasum on PATH" >&2; exit 1
    fi
    if [ "$expected" != "$actual" ]; then
        echo "install.sh: checksum mismatch for $(basename "$archive")" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        exit 1
    fi
}
```

Then replace the `DRY_RUN` exit block with:

```sh
ARCHIVE_EXT=tar.gz
ARCHIVE="agentvfs-$VERSION-$HOST_TUPLE.$ARCHIVE_EXT"
CHECKSUMS="agentvfs-$VERSION-checksums.txt"
ARCHIVE_URL="$URL_BASE/$VERSION/$ARCHIVE"
CHECKSUMS_URL="$URL_BASE/$VERSION/$CHECKSUMS"

if [ "$DRY_RUN" = 1 ]; then
    echo "version=$VERSION"
    echo "url_base=$URL_BASE"
    echo "archive=$ARCHIVE_URL"
    echo "checksums=$CHECKSUMS_URL"
    echo "prefix=$PREFIX"
    echo "system=$SYSTEM"
    echo "dry-run: exit 0"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

download "$ARCHIVE_URL"    "$WORK/$ARCHIVE"
download "$CHECKSUMS_URL"  "$WORK/$CHECKSUMS"
verify_sha256 "$WORK/$ARCHIVE" "$WORK/$CHECKSUMS"
```

Note: for `file://` URLs, both curl and wget support them. The fixture-based tests work without a local HTTP server.

- [ ] **Step 4.4: Run the test, verify it passes**

```
bash tests/install/test_install_sh.sh
```

Expected: PASS through test 4. The end-to-end install still doesn't complete (no extract step), but the checksum-rejection path is exercised.

- [ ] **Step 4.5: Commit**

```bash
git add install.sh tests/install/test_install_sh.sh
git commit -m "feat(install): download + SHA-256 verification

curl-or-wget download, sha256sum-or-shasum verification against the
per-release checksums.txt fetched alongside the archive. Mismatch
exits non-zero before any files are installed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Glibc probe (Linux)

**Files:**
- Modify: `install.sh`
- Modify: `tests/install/test_install_sh.sh`

- [ ] **Step 5.1: Write the failing test**

Append to `tests/install/test_install_sh.sh` before `echo "PASS"`:

```bash
# Test 5: a fake too-old glibc is refused on Linux.
if [ "$(uname -s)" = Linux ]; then
    fake_bin="$(mktemp -d)"
    cat >"$fake_bin/ldd" <<'EOF'
#!/bin/sh
echo "ldd (fake) 2.31"
EOF
    chmod +x "$fake_bin/ldd"
    prefix="$(mktemp -d)"
    set +e
    out="$(PATH="$fake_bin:$PATH" \
           AGENTVFS_INSTALL_URL_BASE=file://$TEST_DIR/fixtures \
           AGENTVFS_INSTALL_VERSION=$VERSION \
           AGENTVFS_INSTALL_PREFIX="$prefix" \
           $INSTALLER --yes 2>&1)"
    rc=$?
    set -e
    [ $rc -ne 0 ] || fail "glibc 2.31 should be refused (got rc=0, out=$out)"
    grep -q "glibc" <<<"$out" || fail "expected glibc error (got: $out)"
    rm -rf "$fake_bin" "$prefix"
    ok "glibc floor refuses 2.31"
else
    ok "glibc floor test skipped (non-Linux)"
fi
```

- [ ] **Step 5.2: Run the test, verify it fails**

```
bash tests/install/test_install_sh.sh
```

Expected: FAIL because install.sh doesn't probe glibc.

- [ ] **Step 5.3: Implement glibc probe**

In `install.sh`, after `detect_host` and before `resolve_version`, add:

```sh
probe_glibc() {
    [ "$HOST_OS" = linux ] || return 0
    if ! command -v ldd >/dev/null; then
        echo "install.sh: ldd not found; cannot verify glibc version. Build from source." >&2
        exit 1
    fi
    ldd_ver="$(ldd --version 2>&1 | head -1 | awk '{print $NF}')"
    # Compare ldd_ver against 2.35. POSIX-safe via awk.
    ok="$(awk -v v="$ldd_ver" 'BEGIN {
        split(v, a, ".")
        if ((a[1]+0) > 2) { print 1; exit }
        if ((a[1]+0) < 2) { print 0; exit }
        if ((a[2]+0) >= 35) { print 1 } else { print 0 }
    }')"
    if [ "$ok" != 1 ]; then
        echo "install.sh: glibc $ldd_ver is older than the 2.35 floor for prebuilts." >&2
        echo "  Build from source: https://github.com/thustorage/ContextFS#build-from-source" >&2
        exit 1
    fi
}

probe_glibc
```

- [ ] **Step 5.4: Run the test, verify it passes**

```
bash tests/install/test_install_sh.sh
```

Expected: PASS. The fake `ldd` returns 2.31, install.sh refuses with the glibc error.

- [ ] **Step 5.5: Commit**

```bash
git add install.sh tests/install/test_install_sh.sh
git commit -m "feat(install): glibc 2.35 floor probe on Linux

Reads 'ldd --version' and refuses with a clear message + link to the
source-build path when glibc is older than 2.35. Failure mode is
informative instead of letting the dynamic linker fail confusingly
at first mount.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: FUSE runtime probe (Linux + macOS)

**Files:**
- Modify: `install.sh`
- Modify: `tests/install/test_install_sh.sh`

Install isn't end-to-end yet (extract lands in Task 7), so the test checks only that the warning string appears — not the final exit code.

- [ ] **Step 6.1: Write the failing test**

Append to `tests/install/test_install_sh.sh` before `echo "PASS"`:

```bash
# Test 6: missing FUSE runtime emits a warning. Doesn't check exit
# code since end-to-end install lands in Task 7.
if [ "$(uname -s)" = Linux ]; then
    prefix="$(mktemp -d)"
    set +e
    out="$(AGENTVFS_INSTALL_URL_BASE=file://$TEST_DIR/fixtures \
           AGENTVFS_INSTALL_VERSION=$VERSION \
           AGENTVFS_INSTALL_PREFIX="$prefix" \
           AGENTVFS_TEST_FAKE_FUSE_MISSING=1 \
           $INSTALLER --yes 2>&1)"
    set -e
    grep -qi "libfuse3\|FUSE runtime not detected" <<<"$out" \
        || fail "expected FUSE warning (got: $out)"
    rm -rf "$prefix"
    ok "FUSE-missing warning (Linux)"
fi
```

- [ ] **Step 6.2: Run the test, verify it fails**

```
bash tests/install/test_install_sh.sh
```

Expected: FAIL at test 6 because there's no FUSE probe yet — no warning string is printed.

- [ ] **Step 6.3: Implement FUSE probe**

In `install.sh`, after `verify_sha256` (between download/verify and extract), add:

```sh
probe_fuse_runtime() {
    if [ "${AGENTVFS_TEST_FAKE_FUSE_MISSING:-0}" = 1 ]; then
        FUSE_MISSING_HINT="(test override) sudo apt install libfuse3-3"
        return 0
    fi
    case "$HOST_OS" in
        linux)
            if command -v ldconfig >/dev/null \
               && ldconfig -p 2>/dev/null | grep -q libfuse3.so.3; then
                return 0
            fi
            for p in /usr/lib/x86_64-linux-gnu/libfuse3.so.3 \
                     /usr/lib64/libfuse3.so.3 \
                     /lib/x86_64-linux-gnu/libfuse3.so.3; do
                [ -f "$p" ] && return 0
            done
            FUSE_MISSING_HINT="sudo apt install libfuse3-3   # or your distro equivalent"
            ;;
        darwin)
            for p in /opt/homebrew/lib/libfuse-t.dylib \
                     /usr/local/lib/libfuse-t.dylib; do
                [ -f "$p" ] && return 0
            done
            FUSE_MISSING_HINT="brew install --cask macos-fuse-t/cask/fuse-t"
            ;;
    esac
}

FUSE_MISSING_HINT=""
probe_fuse_runtime
if [ -n "$FUSE_MISSING_HINT" ]; then
    echo "install.sh: FUSE runtime not detected — agentvfs won't mount until you run:"
    echo "  $FUSE_MISSING_HINT"
fi
```

- [ ] **Step 6.4: Run the test, verify it passes**

```
bash tests/install/test_install_sh.sh
```

Expected: PASS.

- [ ] **Step 6.5: Commit**

```bash
git add install.sh tests/install/test_install_sh.sh
git commit -m "feat(install): FUSE runtime probe

Checks for libfuse3.so.3 (Linux) / libfuse-t.dylib (macOS). On miss,
prints the exact next-command (apt / brew) and continues with a
warning rather than installing host deps automatically. The final
success message will reprint this warning so the user sees both
commands together.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Extract, install, and print final message

**Files:**
- Modify: `install.sh`
- Modify: `tests/install/test_install_sh.sh`

- [ ] **Step 7.1: Write the failing test**

Append to `tests/install/test_install_sh.sh` before `echo "PASS"`:

```bash
# Test 7: end-to-end install lands all three binaries on PATH.
prefix="$(mktemp -d)"
out="$(AGENTVFS_INSTALL_URL_BASE=file://$TEST_DIR/fixtures \
       AGENTVFS_INSTALL_VERSION=$VERSION \
       AGENTVFS_INSTALL_PREFIX="$prefix" \
       $INSTALLER --yes 2>&1)" || fail "install failed: $out"

for f in agentvfs agentvfs-ctl agentvfs-quickstart; do
    [ -x "$prefix/$f" ] || fail "$f not installed at $prefix"
done

# Final message includes prefix, version, and a "next:" line.
grep -q "Installed agentvfs $VERSION" <<<"$out" || fail "missing install line (got: $out)"
grep -q "agentvfs-quickstart" <<<"$out" || fail "missing next: hint (got: $out)"
rm -rf "$prefix"
ok "end-to-end install"
```

- [ ] **Step 7.2: Run the test, verify it fails**

```
bash tests/install/test_install_sh.sh
```

Expected: FAIL because extract/install/message aren't implemented.

- [ ] **Step 7.3: Implement extract + install + final message**

In `install.sh`, append after the `probe_fuse_runtime` invocation block:

```sh
extract_archive() {
    tar -xzf "$WORK/$ARCHIVE" -C "$WORK"
}

install_files() {
    mkdir -p "$PREFIX"
    install_cmd="install -m 0755"
    if [ "$SYSTEM" = 1 ]; then
        # Sudo only when --system was requested.
        install_cmd="sudo $install_cmd"
    fi
    for f in agentvfs agentvfs-ctl agentvfs-quickstart; do
        [ -f "$WORK/$f" ] && $install_cmd "$WORK/$f" "$PREFIX/$f"
    done
}

print_final_message() {
    echo
    echo "Installed agentvfs $VERSION to $PREFIX"
    echo
    if [ -n "$FUSE_MISSING_HINT" ]; then
        echo "Install the FUSE runtime first:"
        echo "  $FUSE_MISSING_HINT"
        echo
    fi
    case "$HOST_OS" in
        linux|darwin)
            echo "next: agentvfs-quickstart /path/to/project"
            ;;
    esac
    # PATH hygiene hint when installing to ~/.local/bin.
    case ":$PATH:" in
        *":$PREFIX:"*) : ;;
        *) echo "  (add $PREFIX to PATH if it isn't already)" ;;
    esac
}

extract_archive
install_files
print_final_message
```

- [ ] **Step 7.4: Run the test, verify it passes**

```
bash tests/install/test_install_sh.sh
```

Expected: PASS for all seven tests.

- [ ] **Step 7.5: Smoke test against the real PATH**

```
bash tests/install/build_fixture.sh v0.0.0-test
AGENTVFS_INSTALL_URL_BASE=file://$(pwd)/tests/install/fixtures \
AGENTVFS_INSTALL_VERSION=v0.0.0-test \
AGENTVFS_INSTALL_PREFIX=/tmp/agentvfs-smoke \
./install.sh --yes
/tmp/agentvfs-smoke/agentvfs
/tmp/agentvfs-smoke/agentvfs-ctl
/tmp/agentvfs-smoke/agentvfs-quickstart
rm -rf /tmp/agentvfs-smoke
```

Expected: each of the three calls prints `fake <name>`.

- [ ] **Step 7.6: Commit**

```bash
git add install.sh tests/install/test_install_sh.sh
git commit -m "feat(install): extract, install, final message

End-to-end install: untar into a temp dir, install three binaries
into PREFIX with 0755, print a success block with the next command
and PATH hygiene hint. Repeats the FUSE-host warning if any.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: install.ps1 — Windows installer

**Files:**
- Create: `install.ps1`

Tested manually on a Windows runner (via the `release.yml` workflow in Task 9). No in-repo automated tests for PowerShell.

- [ ] **Step 8.1: Write install.ps1**

Create `install.ps1` at the repo root:

```powershell
# install.ps1 — installs the agentvfs prebuilt to %LOCALAPPDATA%\Programs\agentvfs.
# Usage: iwr -useb https://raw.githubusercontent.com/thustorage/ContextFS/main/install.ps1 | iex

[CmdletBinding()]
param(
    [string]$Version = $env:AGENTVFS_INSTALL_VERSION,
    [string]$Prefix  = $env:AGENTVFS_INSTALL_PREFIX,
    [switch]$System,
    [switch]$Yes,
    [switch]$DryRun,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'

$DefaultUrlBase = 'https://github.com/thustorage/ContextFS/releases/download'
$UrlBase = if ($env:AGENTVFS_INSTALL_URL_BASE) { $env:AGENTVFS_INSTALL_URL_BASE } else { $DefaultUrlBase }

if ($Help) {
    @'
Usage: install.ps1 [-Version <tag>] [-Prefix <dir>] [-System] [-Yes] [-DryRun]

Installs the agentvfs prebuilt binary and agentvfs-ctl to Prefix.
Default Prefix is %LOCALAPPDATA%\Programs\agentvfs (no admin).
-System installs to %ProgramFiles%\agentvfs (admin prompted).

Environment:
  AGENTVFS_INSTALL_VERSION   pin a release tag (e.g. v0.1.0)
  AGENTVFS_INSTALL_PREFIX    override install destination
  AGENTVFS_INSTALL_URL_BASE  override the release URL base (testing)
'@ | Write-Output
    return
}

# Detect arch.
$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($arch -ne 'X64') {
    Write-Error "install.ps1: unsupported arch '$arch' — only windows-x86_64 has a prebuilt. Build from source: https://github.com/thustorage/ContextFS#build-from-source"
    exit 1
}
$Tuple = 'windows-x86_64'

# Resolve version.
if (-not $Version) {
    $api = 'https://api.github.com/repos/thustorage/ContextFS/releases/latest'
    try {
        $rel = Invoke-RestMethod -UseBasicParsing -Uri $api
        $Version = $rel.tag_name
    } catch {
        Write-Error "install.ps1: could not resolve latest release; pin with -Version vX.Y.Z"
        exit 1
    }
}

# Default prefix.
if (-not $Prefix) {
    if ($System) {
        $Prefix = Join-Path $env:ProgramFiles 'agentvfs'
    } else {
        $Prefix = Join-Path $env:LOCALAPPDATA 'Programs\agentvfs'
    }
}

# Probe WinFsp.
$winfspMissing = $false
$winfspKey = 'HKLM:\SOFTWARE\WOW6432Node\WinFsp'
if (-not (Test-Path $winfspKey) -and -not (Test-Path 'HKLM:\SOFTWARE\WinFsp')) {
    $winfspMissing = $true
}

$archive    = "agentvfs-$Version-$Tuple.zip"
$checksums  = "agentvfs-$Version-checksums.txt"
$archiveUrl   = "$UrlBase/$Version/$archive"
$checksumsUrl = "$UrlBase/$Version/$checksums"

if ($DryRun) {
    Write-Output "host=$Tuple"
    Write-Output "version=$Version"
    Write-Output "url_base=$UrlBase"
    Write-Output "archive=$archiveUrl"
    Write-Output "checksums=$checksumsUrl"
    Write-Output "prefix=$Prefix"
    Write-Output "system=$($System.IsPresent)"
    Write-Output "dry-run: exit 0"
    return
}

$work = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "agentvfs-install-$(Get-Random)") -Force
try {
    Invoke-WebRequest -UseBasicParsing -Uri $archiveUrl   -OutFile (Join-Path $work $archive)
    Invoke-WebRequest -UseBasicParsing -Uri $checksumsUrl -OutFile (Join-Path $work $checksums)

    $expected = (Get-Content (Join-Path $work $checksums) | Where-Object { $_ -match "  $archive$" }) -replace '\s.*',''
    if (-not $expected) {
        throw "install.ps1: $archive not listed in checksums file"
    }
    $actual = (Get-FileHash -Algorithm SHA256 (Join-Path $work $archive)).Hash.ToLower()
    if ($expected.ToLower() -ne $actual) {
        throw "install.ps1: checksum mismatch for $archive`n  expected: $expected`n  actual:   $actual"
    }

    Expand-Archive -Path (Join-Path $work $archive) -DestinationPath $work -Force
    New-Item -ItemType Directory -Path $Prefix -Force | Out-Null
    foreach ($f in 'agentvfs.exe','agentvfs-ctl.exe') {
        Copy-Item -Path (Join-Path $work $f) -Destination (Join-Path $Prefix $f) -Force
    }

    Write-Output ""
    Write-Output "Installed agentvfs $Version to $Prefix"
    Write-Output ""
    if ($winfspMissing) {
        Write-Output "Install WinFsp first: https://winfsp.dev"
        Write-Output ""
    }
    Write-Output 'next:'
    Write-Output '  agentvfs.exe --source C:\some\dir --mountpoint Z:'
    Write-Output '  agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline'

    # PATH hygiene.
    $userPath = [Environment]::GetEnvironmentVariable('Path','User')
    if (-not $System -and $userPath -notlike "*$Prefix*") {
        Write-Output ""
        Write-Output "  (add $Prefix to your PATH)"
    }
} finally {
    Remove-Item -Recurse -Force $work
}
```

- [ ] **Step 8.2: Quick syntax check (Linux machine has no pwsh; defer to CI)**

If `pwsh` is on PATH:

```
pwsh -NoProfile -Command "[System.Management.Automation.Language.Parser]::ParseFile('install.ps1', [ref]\$null, [ref]\$null) | Out-Null; Write-Host OK"
```

Expected: `OK`. If pwsh isn't available, the next CI run on a Windows runner will catch syntax errors.

- [ ] **Step 8.3: Commit**

```bash
git add install.ps1
git commit -m "feat(install): Windows installer (install.ps1)

Mirrors install.sh contract for Windows: detect arch (x86_64 only),
resolve version, download .zip + checksums, verify SHA-256, probe
WinFsp registry key, extract agentvfs.exe + agentvfs-ctl.exe to
%LOCALAPPDATA%\\Programs\\agentvfs (default) or %ProgramFiles%\\agentvfs
(-System). No agentvfs-quickstart on Windows per spec.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Release workflow

**Files:**
- Create: `.github/workflows/release.yml`

- [ ] **Step 9.1: Write release.yml**

Create `.github/workflows/release.yml`:

```yaml
name: Release

on:
  push:
    tags: ['v*']

permissions:
  contents: write

jobs:
  build-linux-x86_64:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install deps
        run: sudo apt-get update && sudo apt-get install -y libfuse3-dev fuse3
      - name: Configure
        run: cmake -B build -DAGENTVFS_EBPF=OFF -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build -j
      - name: Package
        run: |
          version="${GITHUB_REF_NAME}"
          stage="$(mktemp -d)"
          cp build/agentvfs build/agentvfs-ctl start.sh LICENSE "$stage/"
          mv "$stage/start.sh" "$stage/agentvfs-quickstart"
          chmod +x "$stage/agentvfs-quickstart"
          tar -C "$stage" -czf "agentvfs-${version}-linux-x86_64.tar.gz" \
              agentvfs agentvfs-ctl agentvfs-quickstart LICENSE
      - uses: actions/upload-artifact@v4
        with:
          name: linux-x86_64
          path: agentvfs-*-linux-x86_64.tar.gz

  build-darwin-arm64:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install fuse-t
        run: brew install --cask macos-fuse-t/cask/fuse-t
      - name: Configure
        run: cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build -j
      - name: Package
        run: |
          version="${GITHUB_REF_NAME}"
          stage="$(mktemp -d)"
          cp build/agentvfs build/agentvfs-ctl start.sh LICENSE "$stage/"
          mv "$stage/start.sh" "$stage/agentvfs-quickstart"
          chmod +x "$stage/agentvfs-quickstart"
          tar -C "$stage" -czf "agentvfs-${version}-darwin-arm64.tar.gz" \
              agentvfs agentvfs-ctl agentvfs-quickstart LICENSE
      - uses: actions/upload-artifact@v4
        with:
          name: darwin-arm64
          path: agentvfs-*-darwin-arm64.tar.gz

  build-darwin-x86_64:
    runs-on: macos-13
    steps:
      - uses: actions/checkout@v4
      - name: Install fuse-t
        run: brew install --cask macos-fuse-t/cask/fuse-t
      - name: Configure
        run: cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build -j
      - name: Package
        run: |
          version="${GITHUB_REF_NAME}"
          stage="$(mktemp -d)"
          cp build/agentvfs build/agentvfs-ctl start.sh LICENSE "$stage/"
          mv "$stage/start.sh" "$stage/agentvfs-quickstart"
          chmod +x "$stage/agentvfs-quickstart"
          tar -C "$stage" -czf "agentvfs-${version}-darwin-x86_64.tar.gz" \
              agentvfs agentvfs-ctl agentvfs-quickstart LICENSE
      - uses: actions/upload-artifact@v4
        with:
          name: darwin-x86_64
          path: agentvfs-*-darwin-x86_64.tar.gz

  build-windows-x86_64:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install WinFsp
        shell: powershell
        run: choco install winfsp -y --no-progress
      - name: Configure
        run: cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build --config Release -j
      - name: Package
        shell: powershell
        run: |
          $version = "$env:GITHUB_REF_NAME"
          $stage = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "stage-$(Get-Random)") -Force
          Copy-Item build\Release\agentvfs.exe     $stage
          Copy-Item build\Release\agentvfs-ctl.exe $stage
          Copy-Item LICENSE                        $stage
          Compress-Archive -Path "$stage\*" -DestinationPath "agentvfs-$version-windows-x86_64.zip"
      - uses: actions/upload-artifact@v4
        with:
          name: windows-x86_64
          path: agentvfs-*-windows-x86_64.zip

  publish:
    needs: [build-linux-x86_64, build-darwin-arm64, build-darwin-x86_64, build-windows-x86_64]
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/download-artifact@v4
        with:
          merge-multiple: true
      - name: Generate checksums
        run: |
          version="${GITHUB_REF_NAME}"
          sha256sum agentvfs-${version}-*.tar.gz agentvfs-${version}-*.zip \
              > "agentvfs-${version}-checksums.txt"
      - name: Create release
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          version="${GITHUB_REF_NAME}"
          gh release create "$version" \
              --title "$version" \
              --generate-notes \
              "agentvfs-${version}-linux-x86_64.tar.gz" \
              "agentvfs-${version}-darwin-arm64.tar.gz" \
              "agentvfs-${version}-darwin-x86_64.tar.gz" \
              "agentvfs-${version}-windows-x86_64.zip" \
              "agentvfs-${version}-checksums.txt"
```

- [ ] **Step 9.2: Validate the YAML parses**

```
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/release.yml')); print('ok')"
```

Expected: `ok`.

- [ ] **Step 9.3: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "feat(release): GitHub Actions release workflow

Tag-triggered (v*) workflow. Four parallel build jobs produce
artifacts for linux-x86_64 (ubuntu-22.04, glibc 2.35 floor),
darwin-arm64, darwin-x86_64, windows-x86_64. Publish job
aggregates, generates SHA-256 checksums.txt, and creates the
GitHub Release via gh CLI.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: README restructure

**Files:**
- Modify: `README.md`

- [ ] **Step 10.1: Rewrite README.md**

Replace the entire file with:

```markdown
<p align="center">
  <img src="https://github.com/thustorage/ContextFS/raw/main/docs/agentvfs-logo.svg" alt="AgentVFS" width="400">
</p>

<p align="center"><strong>Checkpointable, branchable FUSE workspace for AI agents.</strong></p>

## Install

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/thustorage/ContextFS/main/install.sh | sh

# Windows (PowerShell)
iwr -useb https://raw.githubusercontent.com/thustorage/ContextFS/main/install.ps1 | iex
```

The Linux prebuilt requires glibc ≥ 2.35 (Debian 12, Ubuntu 22.04+, RHEL 9+). Older distros: see **Build from source** below.

## Mount a project

```bash
# Linux / macOS
agentvfs-quickstart /path/to/project
# prints `mount=<path>` and a `cd <path> && claude` (or `codex`) hint
```

```powershell
# Windows
agentvfs.exe --source C:\some\dir --mountpoint Z:
agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline
```

## What you get

| Feature | Description |
|---------|-------------|
| ⏪&nbsp;**Checkpoint&nbsp;&amp;&nbsp;Rollback** | Snapshot the working tree and roll back to any prior checkpoint |
| 🌿&nbsp;**Per&#8209;Agent&nbsp;Branches** | N agents over one source tree, routed by cgroup v2; three-way merge or surfaced conflicts |
| 🔗&nbsp;**Content&#8209;Addressed&nbsp;Store** | blake3-hashed objects deduplicate across checkpoints and branches — near-zero-cost snapshots |
| 🛰️&nbsp;**Pluggable&nbsp;Telemetry** | NDJSON audit via eBPF / fanotify / ptrace / `LD_PRELOAD`; Wasm or Lua to filter and verdict |
| 🖥️&nbsp;**Cross&#8209;Platform** | libfuse3 (Linux), fuse-t (macOS), WinFsp (Windows) |
| 🤖&nbsp;**Agent&#8209;CLI&nbsp;Skills** | `agentvfs-quickstart` mounts a project and installs a Skill for Claude Code / Codex to checkpoint and roll back |

## Driving the daemon directly

```bash
agentvfs workspace start my-task --from /path/to/project
agentvfs workspace checkpoint my-task before-refactor
# ... agent makes changes ...
agentvfs workspace rollback my-task before-refactor
agentvfs workspace stop my-task
```

## How it works

```
agent process
   │  read/write
   ▼
FUSE / WinFsp / fuse-t  ──►  WorkingTree (in-memory, COW)  ──►  ObjectStore (blake3 CAS)
                                                            ▲
                                              checkpoint /  │  rollback
                                                            │
                                              control socket│ ─► agentvfs-ctl
                                                            │
                                              optional eBPF │ ─► NDJSON audit log
```

## Platform support

| Feature | Linux | macOS | Windows |
|---|:---:|:---:|:---:|
| Prebuilt installer (`install.sh` / `install.ps1`) | ✅ x86_64 (glibc ≥ 2.35) | ✅ arm64 + x86_64 | ⚠️ no `agentvfs-quickstart` |
| Checkpoint &amp; Rollback | ✅ | ✅ | ✅ |
| Content-Addressed Store | ✅ | ✅ | ✅ |
| Per-Agent Branches | ✅ | Coming soon | Coming soon |
| Pluggable Telemetry | ✅ | Coming soon | Coming soon |
| `agentvfs workspace` CLI | ✅ | Coming soon | Coming soon |

## Build from source

Use this path if you need a Linux build older than glibc 2.35, want eBPF telemetry compiled in, or are contributing.

```bash
# Linux (Debian/Ubuntu)
sudo apt install build-essential cmake libfuse3-dev pkg-config \
                 clang libbpf-dev linux-tools-generic
cmake -B build && cmake --build build -j
sudo cmake --install build
./start.sh /path/to/project

# eBPF telemetry (Linux, optional)
cmake -B build -DAGENTVFS_EBPF=ON   # default; needs /sys/kernel/btf/vmlinux
```

```bash
# macOS
brew install --cask macos-fuse-t/cask/fuse-t
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON
cmake --build build -j
./build/agentvfs --source ~/some-dir --mountpoint /tmp/agentvfs-mnt &
./build/agentvfs-ctl --sock /tmp/agentvfs-<hash>.sock checkpoint baseline
```

```powershell
# Windows — requires WinFsp 2.0+ from https://winfsp.dev
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON
cmake --build build --config Release -j
.\build\Release\agentvfs.exe --source C:\some\dir --mountpoint Z:
.\build\Release\agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline
```

**Installer flags**: `install.sh --version v0.1.0 --prefix ~/bin --system --dry-run`. Same for `install.ps1` with PowerShell flag syntax. Pin a release with `AGENTVFS_INSTALL_VERSION=v0.1.0`.

**Benchmarks**: `benchmarks/agent-sim/run.sh` — three-method checkpoint comparison. See `benchmarks/agent-sim/README.md` for prerequisites.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE).
```

- [ ] **Step 10.2: Verify markdown renders cleanly**

```
head -50 README.md
grep -n "^#" README.md
```

Expected: clean heading hierarchy, no broken backticks.

- [ ] **Step 10.3: Commit**

```bash
git add README.md
git commit -m "docs: restructure README around install.sh + install.ps1

Hero is two commands: install, then mount. Per-OS cmake quickstarts
demoted to a 'Build from source' section. Platform support matrix
gets a row for prebuilt-installer coverage. Feature table stays
intact, just below the fold.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 11: Pre-release smoke test

This task ships a `v0.1.0-rc1` tag end-to-end before cutting `v0.1.0`. **Requires push access to `thustorage/ContextFS`; ask the user before running.**

- [ ] **Step 11.1: Push the rc1 tag**

```bash
git tag v0.1.0-rc1
git push origin v0.1.0-rc1
```

- [ ] **Step 11.2: Watch release.yml**

```
gh run watch
```

Expected: four parallel build jobs succeed; publish job creates the release with five artifacts attached.

- [ ] **Step 11.3: Verify the prebuilt installs on a clean host**

On a fresh Linux VM or container:

```
curl -fsSL https://raw.githubusercontent.com/thustorage/ContextFS/main/install.sh \
    | AGENTVFS_INSTALL_VERSION=v0.1.0-rc1 sh
~/.local/bin/agentvfs --help
```

Expected: install completes, `agentvfs --help` prints the daemon usage.

- [ ] **Step 11.4: If anything's wrong, delete the tag and iterate**

```
git push --delete origin v0.1.0-rc1
git tag -d v0.1.0-rc1
# fix; re-tag
```

- [ ] **Step 11.5: Once green, cut v0.1.0**

```bash
git tag v0.1.0
git push origin v0.1.0
gh run watch
```

Expected: real `v0.1.0` release published. install.sh in the README now resolves to it via the latest-API call.

- [ ] **Step 11.6: Verify the README hero works verbatim**

On a clean Linux host:

```
curl -fsSL https://raw.githubusercontent.com/thustorage/ContextFS/main/install.sh | sh
agentvfs-quickstart /tmp/some-project
```

Expected: install succeeds, agentvfs-quickstart prints `mount=<path>` and a `cd <path> && claude` hint.

---

## Self-review notes

- **Spec coverage**: § Installer contract (Tasks 1–7 for sh, Task 8 for ps1); § Release artifact format + § CI workflow (Task 9); § README restructure (Task 10); § Quickstart rename (Task 9's package step renames `start.sh` → `agentvfs-quickstart` per spec); § Risks (Task 5 implements glibc floor; Tasks 6 + 7 implement FUSE-probe warn-only behavior; Task 11 smoke-tests `curl … | sh` end-to-end).
- **Placeholders**: none.
- **Type consistency**: env-var names (`AGENTVFS_INSTALL_*`), flag names (`--version`, `--prefix`, `--system`, `--yes`, `--dry-run`, `--help`), function names, and artifact filename patterns are identical across install.sh, install.ps1, release.yml, and the README.
- **Out-of-scope reminder**: `start.sh` is not edited, only copied into release archives under a new name. The agentvfs daemon, agentvfs-ctl, control-socket protocol, and `docs/skills/agentvfs-workspace.md` are untouched.
