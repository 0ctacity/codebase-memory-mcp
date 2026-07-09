#!/usr/bin/env bash
# Verify that a UI-capable binary accepts `install --ui` and persists UI mode.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/build/c/codebase-memory-mcp"

if [[ ! -x "$BIN" ]]; then
    echo "error: expected executable UI binary at $BIN" >&2
    echo "run: make -f Makefile.cbm cbm-with-ui" >&2
    exit 1
fi

TMP_BASE="${TMPDIR:-/tmp}"
TMP_BASE="${TMP_BASE%/}"
HOME_DIR="$(mktemp -d "$TMP_BASE/cbm-install-ui-home.XXXXXX")"
CACHE_DIR="$(mktemp -d "$TMP_BASE/cbm-install-ui-cache.XXXXXX")"
FAKE_BIN="$HOME_DIR/bin"

cleanup() {
    rm -rf "$HOME_DIR" "$CACHE_DIR"
}
trap cleanup EXIT

touch "$HOME_DIR/.zshrc"
mkdir -p "$FAKE_BIN"
printf '#!/bin/sh\nexit 1\n' > "$FAKE_BIN/pgrep"
chmod +x "$FAKE_BIN/pgrep"

HOME="$HOME_DIR" CBM_CACHE_DIR="$CACHE_DIR" PATH="$FAKE_BIN:$PATH" "$BIN" install --ui --force -y

CONFIG="$CACHE_DIR/config.json"
if [[ ! -f "$CONFIG" ]]; then
    echo "error: expected UI config at $CONFIG" >&2
    exit 1
fi

if ! grep -Eq '"ui_enabled"[[:space:]]*:[[:space:]]*true' "$CONFIG"; then
    echo "error: install --ui did not persist ui_enabled=true" >&2
    sed -n '1,80p' "$CONFIG" >&2
    exit 1
fi

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        INSTALLED="$HOME_DIR/.local/bin/codebase-memory-mcp.exe"
        ;;
    *)
        INSTALLED="$HOME_DIR/.local/bin/codebase-memory-mcp"
        ;;
esac

if [[ ! -f "$INSTALLED" ]]; then
    echo "error: expected installed binary at $INSTALLED" >&2
    exit 1
fi

HOME="$HOME_DIR" CBM_CACHE_DIR="$CACHE_DIR" "$INSTALLED" --help >/dev/null

echo "install --ui success test passed"
