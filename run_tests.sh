#!/usr/bin/env bash
# Builds and runs the host-side unit tests for the Zephyr-free logic modules
# (ax25.c, kiss.c, fx25.c, gps_format.c, afsk_modulator.cpp,
# afsk_demodulator.cpp) and reports gcov line coverage for each, failing if
# any drops below 80%.
#
# tnc.c, main.c, ptt.c, mode_select.c, gps.c, audio_tx_pwm.cpp and
# audio_rx_adc.cpp are excluded: they're Zephyr/hardware-coupled (UART,
# GPIO, PWM, ADC, BLE) and aren't unit-testable on host without mocking the
# whole Zephyr HAL. Those get exercised by hand on real promicro_nrf52840
# boards instead.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
BUILD_DIR="$SCRIPT_DIR/.host_test_build"
COVERAGE_MIN=80

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "[+] Building host test binaries..."
gcc -std=c11 -Wall --coverage -O0 -o "$BUILD_DIR/test_ax25" \
	"$SRC_DIR/test_ax25.c" "$SRC_DIR/ax25.c"
gcc -std=c11 -Wall --coverage -O0 -o "$BUILD_DIR/test_kiss" \
	"$SRC_DIR/test_kiss.c" "$SRC_DIR/kiss.c"
gcc -std=c11 -Wall --coverage -O0 -o "$BUILD_DIR/test_fx25" \
	"$SRC_DIR/test_fx25.c" "$SRC_DIR/fx25.c" "$SRC_DIR/ax25.c"
g++ -std=c++17 -Wall --coverage -O0 -o "$BUILD_DIR/test_afsk_loopback" \
	"$SRC_DIR/test_afsk_loopback.cpp" "$SRC_DIR/afsk_modulator.cpp" \
	"$SRC_DIR/afsk_demodulator.cpp" "$SRC_DIR/fx25.c" "$SRC_DIR/ax25.c"
gcc -std=c11 -Wall --coverage -O0 -o "$BUILD_DIR/test_gps_format" \
	"$SRC_DIR/test_gps_format.c" "$SRC_DIR/gps_format.c"

echo
echo "[+] Running tests..."
overall_status=0
for t in test_ax25 test_kiss test_fx25 test_afsk_loopback test_gps_format; do
	echo "--- $t ---"
	if ! "$BUILD_DIR/$t"; then
		overall_status=1
	fi
	echo
done

echo "[+] Coverage (line %, host-testable pure-logic modules only):"
cd "$BUILD_DIR"
coverage_status=0
declare -A file_to_gcda=(
	[ax25.c]="test_ax25-ax25.gcda"
	[kiss.c]="test_kiss-kiss.gcda"
	[fx25.c]="test_fx25-fx25.gcda"
	[gps_format.c]="test_gps_format-gps_format.gcda"
	[afsk_modulator.cpp]="test_afsk_loopback-afsk_modulator.gcda"
	[afsk_demodulator.cpp]="test_afsk_loopback-afsk_demodulator.gcda"
)
for src in "${!file_to_gcda[@]}"; do
	pct=$(gcov "$SRC_DIR/$src" -o "${file_to_gcda[$src]}" 2>/dev/null \
		| grep -A1 "File '$SRC_DIR/$src'" | grep "Lines executed" \
		| sed -E 's/.*: *([0-9.]+)% of.*/\1/')
	printf "  %-24s %s%%\n" "$src" "$pct"
	below=$(awk -v p="$pct" -v m="$COVERAGE_MIN" 'BEGIN { print (p+0 < m) ? 1 : 0 }')
	if [ "$below" = "1" ]; then
		coverage_status=1
	fi
done

echo
if [ "$overall_status" -ne 0 ]; then
	echo "[!] One or more test suites FAILED."
	exit 1
fi
if [ "$coverage_status" -ne 0 ]; then
	echo "[!] Coverage dropped below ${COVERAGE_MIN}% on at least one module."
	exit 1
fi
echo "[+] All host tests passed, all modules >= ${COVERAGE_MIN}% line coverage."
