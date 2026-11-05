#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/cbm-installer-safety.XXXXXX")
trap 'rm -rf "$TMP_ROOT"' EXIT

HOME_DIR="$TMP_ROOT/home"
INSTALL_DIR="$HOME_DIR/.local/bin"
RELEASE_DIR="$TMP_ROOT/release"
FAKE_BIN_DIR="$TMP_ROOT/fake-bin"
mkdir -p "$HOME_DIR" "$RELEASE_DIR" "$FAKE_BIN_DIR"

case "$(uname -s)" in
    Darwin) OS=darwin ;;
    Linux) OS=linux ;;
    *) echo "skip: unsupported test platform"; exit 0 ;;
esac
case "$(uname -m)" in
    arm64|aarch64) ARCH=arm64 ;;
    x86_64|amd64) ARCH=amd64 ;;
    *) echo "skip: unsupported test architecture"; exit 0 ;;
esac
PORTABLE=""
[[ "$OS" == linux ]] && PORTABLE="-portable"
ARCHIVE="codebase-memory-mcp-${OS}-${ARCH}${PORTABLE}.tar.gz"

make_release() {
    local marker=$1 stage="$TMP_ROOT/stage"
    rm -rf "$stage"
    mkdir -p "$stage"
    cat > "$stage/codebase-memory-mcp" <<SH
#!/bin/sh
if [ "\${1:-}" = "--version" ]; then
    printf 'codebase-memory-mcp %s\\n' "$marker"
    exit 0
fi
if [ "\${1:-}" = "install" ]; then
    printf '%s\\n' "\$*" >> "\${FAKE_INSTALL_LOG:?}"
    exit 0
fi
exit 0
SH
    chmod 755 "$stage/codebase-memory-mcp"
    tar -czf "$RELEASE_DIR/$ARCHIVE" -C "$stage" codebase-memory-mcp
    if command -v sha256sum >/dev/null 2>&1; then
        HASH=$(sha256sum "$RELEASE_DIR/$ARCHIVE" | awk '{print $1}')
    else
        HASH=$(shasum -a 256 "$RELEASE_DIR/$ARCHIVE" | awk '{print $1}')
    fi
    # Match the release workflow's `sha256sum ./archive` output exactly.
    printf '%s  ./%s\n' "$HASH" "$ARCHIVE" > "$RELEASE_DIR/checksums.txt"
}

cat > "$FAKE_BIN_DIR/curl" <<'SH'
#!/bin/sh
set -eu
out=""
url=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) out=$2; shift 2 ;;
        -*) shift ;;
        *) url=$1; shift ;;
    esac
done
cp "$FAKE_RELEASE_DIR/${url##*/}" "$out"
SH
chmod 755 "$FAKE_BIN_DIR/curl"

make_release one
HOME="$HOME_DIR" FAKE_RELEASE_DIR="$RELEASE_DIR" \
PATH="$FAKE_BIN_DIR:$PATH" CBM_DOWNLOAD_URL=http://localhost/releases \
    "$ROOT/install.sh" --skip-config
FIRST_HASH=$(shasum -a 256 "$INSTALL_DIR/codebase-memory-mcp" | awk '{print $1}')

OUTPUT=$(HOME="$HOME_DIR" FAKE_RELEASE_DIR="$RELEASE_DIR" \
PATH="$FAKE_BIN_DIR:$PATH" CBM_DOWNLOAD_URL=http://localhost/releases \
    "$ROOT/install.sh" --skip-config)
grep -q "already installed" <<<"$OUTPUT"

make_release two
if HOME="$HOME_DIR" FAKE_RELEASE_DIR="$RELEASE_DIR" \
PATH="$FAKE_BIN_DIR:$PATH" CBM_DOWNLOAD_URL=http://localhost/releases \
    "$ROOT/install.sh" --skip-config >/dev/null 2>&1; then
    echo "error: installer replaced a different binary without --replace" >&2
    exit 1
fi
[[ "$(shasum -a 256 "$INSTALL_DIR/codebase-memory-mcp" | awk '{print $1}')" == "$FIRST_HASH" ]]

HOME="$HOME_DIR" FAKE_RELEASE_DIR="$RELEASE_DIR" \
PATH="$FAKE_BIN_DIR:$PATH" CBM_DOWNLOAD_URL=http://localhost/releases \
    "$ROOT/install.sh" --skip-config --replace
grep -q "two" <("$INSTALL_DIR/codebase-memory-mcp" --version)
[[ ! -e "$INSTALL_DIR/codebase-memory-mcp.old" ]]

# Re-running an identical install must still repair/configure agent entries.
INSTALL_LOG="$TMP_ROOT/install-args.log"
: > "$INSTALL_LOG"
HOME="$HOME_DIR" FAKE_RELEASE_DIR="$RELEASE_DIR" FAKE_INSTALL_LOG="$INSTALL_LOG" \
PATH="$FAKE_BIN_DIR:$PATH" CBM_DOWNLOAD_URL=http://localhost/releases \
    "$ROOT/install.sh"
grep -qx 'install -y' "$INSTALL_LOG"

# A source-hash receipt must not hide later modification of the installed file.
printf '%s\n' tampered >> "$INSTALL_DIR/codebase-memory-mcp"
if HOME="$HOME_DIR" FAKE_RELEASE_DIR="$RELEASE_DIR" \
PATH="$FAKE_BIN_DIR:$PATH" CBM_DOWNLOAD_URL=http://localhost/releases \
    "$ROOT/install.sh" --skip-config >/dev/null 2>&1; then
    echo "error: installer trusted a modified installed binary" >&2
    exit 1
fi
HOME="$HOME_DIR" FAKE_RELEASE_DIR="$RELEASE_DIR" \
PATH="$FAKE_BIN_DIR:$PATH" CBM_DOWNLOAD_URL=http://localhost/releases \
    "$ROOT/install.sh" --skip-config --replace >/dev/null

# An installer shipped inside a release archive must use its adjacent binary
# instead of downloading "latest" again.
LOCAL_RELEASE="$TMP_ROOT/local-release"
LOCAL_HOME="$TMP_ROOT/local-home"
mkdir -p "$LOCAL_RELEASE" "$LOCAL_HOME"
cp "$ROOT/install.sh" "$LOCAL_RELEASE/install.sh"
cp "$TMP_ROOT/stage/codebase-memory-mcp" "$LOCAL_RELEASE/codebase-memory-mcp"
chmod 755 "$LOCAL_RELEASE/install.sh" "$LOCAL_RELEASE/codebase-memory-mcp"
HOME="$LOCAL_HOME" FAKE_INSTALL_LOG="$INSTALL_LOG" PATH="$FAKE_BIN_DIR:$PATH" \
CBM_DOWNLOAD_URL=http://localhost/must-not-download \
    "$LOCAL_RELEASE/install.sh" --skip-config
test -x "$LOCAL_HOME/.local/bin/codebase-memory-mcp"

# A network install without a checksum file must fail closed.
NO_HASH_RELEASE="$TMP_ROOT/no-hash-release"
NO_HASH_HOME="$TMP_ROOT/no-hash-home"
mkdir -p "$NO_HASH_RELEASE" "$NO_HASH_HOME"
cp "$RELEASE_DIR/$ARCHIVE" "$NO_HASH_RELEASE/$ARCHIVE"
if HOME="$NO_HASH_HOME" FAKE_RELEASE_DIR="$NO_HASH_RELEASE" \
PATH="$FAKE_BIN_DIR:$PATH" CBM_DOWNLOAD_URL=http://localhost/releases \
    "$ROOT/install.sh" --skip-config >/dev/null 2>&1; then
    echo "error: installer accepted a release without checksums.txt" >&2
    exit 1
fi

grep -q 'REPO="octacity-org/codebase-memory-mcp"' "$ROOT/install.sh"
grep -q '\$Repo = "octacity-org/codebase-memory-mcp"' "$ROOT/install.ps1"
grep -q '\$AlreadyInstalled = \$true' "$ROOT/install.ps1"
grep -q '\$PSScriptRoot' "$ROOT/install.ps1"
# TLS 1.3 only where schannel can negotiate it (build 20348+); an unsupported
# bit hard-fails the first HTTPS request on Windows 10 (upstream #2188).
if grep -q 'Tls12 -bor \[Net.SecurityProtocolType\]::Tls13' "$ROOT/install.ps1"; then
    echo "error: install.ps1 sets the TLS 1.3 bit unconditionally" >&2
    exit 1
fi
grep -q 'OSVersion.Version.Build -ge 20348' "$ROOT/install.ps1"
grep -q "GetNames(\[Net.SecurityProtocolType\]) -contains 'Tls13'" "$ROOT/install.ps1"
grep -q 'SecurityProtocol = \$CbmProtocols' "$ROOT/install.ps1"
grep -q 'canonical install.sh' "$ROOT/scripts/setup.sh"
grep -q 'canonical install.ps1' "$ROOT/scripts/setup-windows.ps1"
if grep -q 'Go 1\.23\\|CGO\\|Configure Claude Code' \
    "$ROOT/scripts/setup.sh" "$ROOT/scripts/setup-windows.ps1"; then
    echo "error: deprecated setup wrappers still contain original-CBM installation logic" >&2
    exit 1
fi
if grep -q -- 'printf("  --reset-indexes\\|if (update_clear_indexes(home' \
    "$ROOT/src/cli/commands.zig"; then
    echo "error: install/update still expose legacy per-project index deletion" >&2
    exit 1
fi
grep -q '"  database: {s}' "$ROOT/src/cli_zig/cbm_cli_zig.zig"
grep -q '"database: {s}' "$ROOT/src/cli_zig/cbm_cli_zig.zig"
if grep -q 'project databases\\|project_databases' \
    "$ROOT/src/cli/cli.zig" "$ROOT/src/cli/commands.zig" "$ROOT/src/cli_zig/cbm_cli_zig.zig"; then
    echo "error: installer diagnostics still describe per-project databases" >&2
    exit 1
fi

echo "installer safety test passed"
