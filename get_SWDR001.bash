#! /bin/bash

set -e

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname -- "${SCRIPT_PATH}")"

cd "${SCRIPT_DIR}"
mkdir -p ./deps
cd ./deps

echo "Downloading LR11XX Driver"
git clone https://github.com/Lora-net/SWDR001
cd SWDR001
#v3.0 is a333238acfa0a9dee9ce2824ce52e89b98f3d24b, but we are keeping on older vers untill we know this works
git checkout f99fe41538e351c4c0d1975a4138532fe7869d65
echo "Building LR11XX Driver"
cmake -S . -B ./build
cmake --build ./build --config Release -j $(nrpoc)
echo "LR11XX driver built"