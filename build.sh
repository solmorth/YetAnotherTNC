#!/usr/bin/env bash
set -e

# Project root directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

VENV_DIR="$SCRIPT_DIR/.venv"
ZEPHYR_WORKSPACE="$SCRIPT_DIR/zephyrproject"
export ZEPHYR_BASE="${ZEPHYR_BASE:-$ZEPHYR_WORKSPACE/zephyr}"
rm $SCRIPT_DIR/build -r || true

# Auto-detect Zephyr SDK if not explicitly set
if [ -z "$ZEPHYR_SDK_INSTALL_DIR" ]; then
    # Search for installed zephyr-sdk in common locations
    DETECTED_SDK=$(ls -d "$HOME"/zephyr-sdk-* /opt/zephyr-sdk-* "$HOME"/.local/zephyr-sdk-* "$SCRIPT_DIR"/.zephyr-sdk 2>/dev/null | head -n 1 || true)
    if [ -n "$DETECTED_SDK" ]; then
        export ZEPHYR_SDK_INSTALL_DIR="$DETECTED_SDK"
    fi
fi

# 1. Setup Virtual Environment
if [ ! -d "$VENV_DIR" ]; then
    echo "[+] Creating virtual environment in $VENV_DIR..."
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --upgrade pip
    "$VENV_DIR/bin/pip" install west pyelftools cmake ninja
fi

source "$VENV_DIR/bin/activate"

# 2. Download / Init Zephyr Workspace if not present
if [ ! -d "$ZEPHYR_BASE" ]; then
    echo "[+] Zephyr not found locally. Initializing Zephyr workspace inside project..."
    mkdir -p "$ZEPHYR_WORKSPACE"
    (
        unset ZEPHYR_BASE
        cd "$ZEPHYR_WORKSPACE"
        west init -m https://github.com/zephyrproject-rtos/zephyr --mr main .
        west update
    )
fi

# Install Python requirements if needed
if [ -f "$ZEPHYR_BASE/scripts/requirements.txt" ]; then
    "$VENV_DIR/bin/pip" install -q -r "$ZEPHYR_BASE/scripts/requirements.txt"
fi

# 3. Determine Board & Run Build
BOARD="${BOARD:-promicro_nrf52840}"

if [ $# -eq 0 ]; then
    echo "[+] Building project for board: $BOARD"
    west build -b "$BOARD" .
else
    echo "[+] Running west build with arguments: $@"
    west build "$@"
fi

echo "[+] Build completed successfully!"
