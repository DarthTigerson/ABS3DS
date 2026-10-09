#!/bin/sh
# Builds ABS3DS inside the abs3ds-build Docker image (devkitARM + makerom + bannertool).
#   ./build.sh          -> abs3ds.3dsx
#   ./build.sh cia      -> abs3ds.3dsx + abs3ds.cia
#   ./build.sh clean
set -e
cd "$(dirname "$0")"
if ! docker image inspect abs3ds-build >/dev/null 2>&1; then
    docker build -t abs3ds-build docker
fi
exec docker run --rm -v "$PWD":/src -w /src abs3ds-build make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" "$@"
