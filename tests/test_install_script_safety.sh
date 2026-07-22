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
    printf '#!/bin/sh\nprintf "codebase-memory-mcp %s\\n"\n' "$marker" \
        > "$stage/codebase-memory-mcp"
    chmod 755 "$stage/codebase-memory-mcp"
    tar -czf "$RELEASE_DIR/$ARCHIVE" -C "$stage" codebase-memory-mcp
    if command -v sha256sum >/dev/null 2>&1; then
        HASH=$(sha256sum "$RELEASE_DIR/$ARCHIVE" | awk '{print $1}')
    else
        HASH=$(shasum -a 256 "$RELEASE_DIR/$ARCHIVE" | awk '{print $1}')
    fi
    printf '%s  %s\n' "$HASH" "$ARCHIVE" > "$RELEASE_DIR/checksums.txt"
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

grep -q 'REPO="0ctacity/codebase-memory-mcp"' "$ROOT/install.sh"
grep -q '\$Repo = "0ctacity/codebase-memory-mcp"' "$ROOT/install.ps1"

echo "installer safety test passed"
