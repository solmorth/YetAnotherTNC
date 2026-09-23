#!/usr/bin/env bash
set -e

# Project root directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

VENV_DIR="$SCRIPT_DIR/.venv"
ZEPHYR_WORKSPACE="$SCRIPT_DIR/zephyrproject"
export ZEPHYR_BASE="${ZEPHYR_BASE:-$ZEPHYR_WORKSPACE/zephyr}"
rm $SCRIPT_DIR/build -r || true

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

# 3. Setup Zephyr SDK if not present
if [ -z "$ZEPHYR_SDK_INSTALL_DIR" ]; then
    SDK_DIR="$SCRIPT_DIR/.zephyr-sdk"
    DETECTED_SDK=""
    for cand in "$SDK_DIR" "$HOME"/zephyr-sdk-* /opt/zephyr-sdk-* "$HOME"/.local/zephyr-sdk-*; do
        if [ -d "$cand" ] && { [ -f "$cand/cmake/Zephyr-sdkConfig.cmake" ] || [ -f "$cand/Zephyr-sdkConfig.cmake" ]; } && [ -f "$cand/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ]; then
            DETECTED_SDK="$cand"
            break
        fi
    done

    if [ -n "$DETECTED_SDK" ]; then
        export ZEPHYR_SDK_INSTALL_DIR="$DETECTED_SDK"
    else
        if [ ! -f "$SDK_DIR/setup.sh" ]; then
            echo "[+] Zephyr SDK not found or incomplete. Downloading SDK to $SDK_DIR..."
            SDK_VERSION="$(cat "$ZEPHYR_BASE/SDK_VERSION" 2>/dev/null || echo "1.0.1")"
            HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
            HOST_ARCH="$(uname -m)"
            SDK_ARCH_STR="${HOST_OS}-${HOST_ARCH}"
            SDK_TAR="zephyr-sdk-${SDK_VERSION}_${SDK_ARCH_STR}_minimal.tar.xz"
            SDK_URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VERSION}/${SDK_TAR}"

            TMP_DIR="$(mktemp -d)"
            echo "[+] Downloading $SDK_URL..."
            if command -v curl >/dev/null 2>&1; then
                curl -fL "$SDK_URL" -o "$TMP_DIR/$SDK_TAR"
            elif command -v wget >/dev/null 2>&1; then
                wget -O "$TMP_DIR/$SDK_TAR" "$SDK_URL"
            else
                python3 -c "import urllib.request; urllib.request.urlretrieve('$SDK_URL', '$TMP_DIR/$SDK_TAR')"
            fi

            echo "[+] Extracting Zephyr SDK..."
            mkdir -p "$SDK_DIR"
            tar -xf "$TMP_DIR/$SDK_TAR" -C "$SDK_DIR" --strip-components=1
            rm -rf "$TMP_DIR"
        fi

        if [ ! -f "$SDK_DIR/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ]; then
            rm -rf "$SDK_DIR/gnu/arm-zephyr-eabi"
        fi

        echo "[+] Setting up Zephyr SDK (toolchain: arm-zephyr-eabi)..."
        (
            cd "$SDK_DIR"
            ./setup.sh -t arm-zephyr-eabi -h -c
        )
        export ZEPHYR_SDK_INSTALL_DIR="$SDK_DIR"
    fi
fi

# 4. Determine Board & Run Build
BOARD="${BOARD:-promicro_nrf52840}"

if [ $# -eq 0 ]; then
    echo "[+] Building project for board: $BOARD"
    west build -b "$BOARD" .
else
    echo "[+] Running west build with arguments: $@"
    west build "$@"
fi

echo "[+] Build completed successfully!"
