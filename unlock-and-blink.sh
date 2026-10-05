#!/usr/bin/env bash
#
# Unlock a factory-locked IKEA RODRET (EFR32MG21) and flash the blinky.
# Reproduces the full flow on a fresh unit. Run from the project root.
#
# Prereqs: a CMSIS-DAP probe wired to the RODRET SWD pads (GND/SWDIO/SWCLK,
# RESET if available), the board powered, and ./.venv with pyocd installed.
#
set -euo pipefail
cd "$(dirname "$0")"

PY=.venv/bin/python3
PYOCD=.venv/bin/pyocd
TARGET=efr32mg21a010f1024im32

echo "==> 1. Probe check"
$PYOCD list

echo "==> 2. Read Secure Engine lock status (non-destructive)"
$PY dci.py status

echo
read -r -p "==> This will DEVICE-ERASE the chip (wipes all flash). Continue? [y/N] " ans
[ "${ans:-N}" = "y" ] || { echo "aborted"; exit 1; }

echo "==> 3. DCI device erase (unlock)"
$PY dci.py erase

echo "==> 4. Confirm the core is reachable after unlock"
$PYOCD commander -t "$TARGET" -M under-reset \
    -c "reset halt" -c "status" -c "read32 0x0 16" 2>/dev/null

echo "==> 5. Flash the blinky at 0x0"
$PYOCD flash -t "$TARGET" --base-address 0x0 blinky.bin

echo "==> 6. Reset and run"
$PYOCD commander -t "$TARGET" -M under-reset -c "reset" -c "go" 2>/dev/null

echo "==> Done. PC0 should be toggling."
