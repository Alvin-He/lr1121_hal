#! /bin/bash

VERSION="libgpiod-2.2.4"

set -e

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname -- "${SCRIPT_PATH}")"

cd "${SCRIPT_DIR}"
mkdir -p ./deps
cd ./deps

wget "https://mirrors.edge.kernel.org/pub/software/libs/libgpiod/${VERSION}.tar.gz"
tar -xvf "./${VERSION}.tar.gz"
cd "./${VERSION}/"
./configure --prefix="${SCRIPT_DIR}/deps" --enable-shared=no
make -j $(nproc)
make install

cd ..
rm -r "./${VERSION}/"
rm "${VERSION}.tar.gz"