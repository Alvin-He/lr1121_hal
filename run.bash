#! /bin/bash

set -e

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname -- "${SCRIPT_PATH}")"

cd "${SCRIPT_DIR}"

cd ./build

cmake --build . -j 4 && sudo ./lr11xx_firmware_updater 27 8 22 /dev/spidev0.0 ./lr1121_transceiver_0103.bin 0 > output.log 2>&1
 