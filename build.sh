#!/bin/sh
# Build Vaultik on Linux with CMake + Ninja and the distribution's packages (listed at the top of
# CMakeLists.txt), or with vcpkg when VCPKG_ROOT is set and the first argument is "vcpkg".
# Usage: ./build.sh [debug|release] [test]
#        ./build.sh vcpkg [debug|release] [test]
# BUILD_DIR sets the build folder (default build/linux-<type>); keep it on a Linux file system,
# not on a Windows drive under /mnt, when building in WSL.
set -eu

if [ "${1:-}" = "vcpkg" ]; then
    shift
    if [ -z "${VCPKG_ROOT:-}" ]; then
        echo "Set VCPKG_ROOT to a vcpkg checkout (git clone https://github.com/microsoft/vcpkg)." >&2
        exit 1
    fi
    preset="linux-vcpkg-${1:-debug}"
    cmake --preset "$preset"
    cmake --build --preset "$preset"
    if [ "${2:-}" = "test" ]; then
        ctest --preset "$preset"
    fi
    exit 0
fi

type="${1:-debug}"
case "$type" in
    debug) config=Debug ;;
    release) config=Release ;;
    *) echo "usage: $0 [vcpkg] [debug|release] [test]" >&2; exit 2 ;;
esac
root=$(cd "$(dirname "$0")" && pwd)
build="${BUILD_DIR:-$root/build/linux-$type}"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE="$config" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build"
if [ "${2:-}" = "test" ]; then
    ctest --test-dir "$build" --output-on-failure
fi
