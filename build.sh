#!/bin/sh
# Build Vaultik on Linux with CMake + Ninja + vcpkg.
# Usage: ./build.sh [debug|release] [test]
set -eu

preset="linux-${1:-debug}"
if [ -z "${VCPKG_ROOT:-}" ]; then
    echo "Set VCPKG_ROOT to a vcpkg checkout (git clone https://github.com/microsoft/vcpkg)." >&2
    exit 1
fi

cmake --preset "$preset"
cmake --build --preset "$preset"
if [ "${2:-}" = "test" ]; then
    ctest --preset "$preset"
fi
