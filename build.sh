#!/bin/sh
# Builds Shelf3DS inside the shelf3ds-build Docker image (devkitARM + makerom + bannertool).
#   ./build.sh          -> shelf3ds.3dsx
#   ./build.sh cia      -> shelf3ds.3dsx + shelf3ds.cia
#   ./build.sh clean
set -e
cd "$(dirname "$0")"
if ! docker image inspect shelf3ds-build >/dev/null 2>&1; then
    docker build -t shelf3ds-build docker
fi
exec docker run --rm -v "$PWD":/src -w /src shelf3ds-build make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" "$@"
