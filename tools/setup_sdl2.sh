#!/bin/sh
# setup_sdl2.sh — fetch + unpack SDL2 mingw development libraries into
# third_party/. Run this once before building CalcLang programs that
# use the gui_* builtins. Skips download if the tree is already there.
#
#   tools/setup_sdl2.sh
#
# After it succeeds, calcnat auto-detects the vendored SDL2 and links
# every program against it; non-GUI programs aren't affected.

set -e

SDL2_VERSION="2.30.10"
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.tar.gz"
DEST_DIR="third_party/SDL2-${SDL2_VERSION}"
TARBALL="third_party/SDL2-${SDL2_VERSION}-mingw.tar.gz"

mkdir -p third_party

if [ -d "${DEST_DIR}" ]; then
    echo "SDL2 already at ${DEST_DIR} — nothing to do."
    exit 0
fi

if [ ! -f "${TARBALL}" ]; then
    echo "Downloading ${SDL2_URL}..."
    if command -v curl >/dev/null 2>&1; then
        curl -L -o "${TARBALL}" "${SDL2_URL}"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "${TARBALL}" "${SDL2_URL}"
    else
        echo "setup_sdl2: neither curl nor wget found. Install one and retry." >&2
        exit 1
    fi
fi

echo "Extracting ${TARBALL}..."
tar xzf "${TARBALL}" -C third_party

if [ ! -d "${DEST_DIR}" ]; then
    echo "setup_sdl2: tarball did not produce ${DEST_DIR}." >&2
    exit 1
fi

echo
echo "SDL2 is set up at ${DEST_DIR}."
echo
echo "Now rebuild calcnat to pick up the new GUI runtime path,"
echo "then build any GUI example, e.g.:"
echo "    make                                     # builds calcnat + everything else"
echo "    build/calcnat examples/tetris_gui.calc -o build/tetris_gui.exe"
echo "    build/tetris_gui.exe"
