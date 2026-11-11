#!/bin/sh
# Builds dist/Vaultik-x86_64.AppImage: the window, the engine and Qt in one file that runs from
# anywhere, also from the backup drive and on SteamOS (whose system is read-only).
#
# Usage: tools/appimage.sh            (from the source folder, on x86_64 Linux)
# Environment:
#   BUILD_DIR   where to build (default build/appimage; keep it on a Linux file system)
#   TOOLS_DIR   where linuxdeploy is kept (default ~/.cache/vaultik-appimage)
#   QMAKE       Qt 6's qmake, if it is not found (Debian/Ubuntu: /usr/lib/qt6/bin/qmake)
#
# Needs the build dependencies listed in CMakeLists.txt and curl. linuxdeploy and its Qt plugin are
# downloaded on the first run (their "continuous" release; delete TOOLS_DIR to update them).
#
# The AppImage runs on systems whose C library is at least as new as the build system's: build it
# on the oldest distribution it should run on (check with "ldd --version" on the Steam Deck).
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD_DIR:-$root/build/appimage}
tools=${TOOLS_DIR:-$HOME/.cache/vaultik-appimage}
dist=$root/dist
appdir=$build/AppDir

arch=$(uname -m)
if [ "$arch" != "x86_64" ]; then
    echo "Only x86_64 AppImages are built (the Steam Deck is x86_64)." >&2
    exit 1
fi

# WSL and containers often have no FUSE: let the tools unpack themselves instead.
export APPIMAGE_EXTRACT_AND_RUN=1

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DVAULTIK_BUILD_TESTS=OFF
cmake --build "$build"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build"

mkdir -p "$tools"
fetch() {
    if [ ! -x "$tools/$1" ]; then
        echo "Downloading $1"
        curl -fL --retry 3 -o "$tools/$1.part" "$2"
        chmod +x "$tools/$1.part"
        mv "$tools/$1.part" "$tools/$1"
    fi
}
fetch linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch linuxdeploy-plugin-qt-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage

if [ -z "${QMAKE:-}" ]; then
    for q in qmake6 /usr/lib/qt6/bin/qmake /usr/lib64/qt6/bin/qmake qmake; do
        if command -v "$q" >/dev/null 2>&1; then
            QMAKE=$(command -v "$q")
            break
        fi
    done
fi
export QMAKE="${QMAKE:?Qt 6 qmake not found; set QMAKE}"
# The SVG icon engine and image plugin, for the program's icons.
export EXTRA_QT_MODULES="svg"
# Wayland too, when Qt's Wayland plugin is installed (qt6-wayland); X11 (xcb) always.
if [ -f "$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms/libqwayland-generic.so" ]; then
    export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so"
fi
version=$(sed -n 's/^project(Vaultik VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
mkdir -p "$dist"
export OUTPUT="$dist/Vaultik-x86_64.AppImage"
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export PATH="$tools:$PATH"

cd "$build"
"$tools/linuxdeploy-x86_64.AppImage" \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/vaultik" \
    --executable "$appdir/usr/bin/vaultik-gui" \
    --desktop-file "$appdir/usr/share/applications/io.github.bigisoft.Vaultik.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/io.github.bigisoft.Vaultik.svg" \
    --custom-apprun "$root/packaging/linux/AppRun" \
    --plugin qt \
    --output appimage

(cd "$dist" && sha256sum "Vaultik-x86_64.AppImage" > Vaultik-x86_64.AppImage.sha256)
echo "Built $OUTPUT"
