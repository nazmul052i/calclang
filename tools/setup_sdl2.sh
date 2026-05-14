#!/bin/sh
# setup_sdl2.sh — one-time setup for the CalcLang GUI runtime.
# Fetches SDL2 (the window/renderer/event library) and stb_truetype
# (single-header TTF rasterizer used by gui_text) into third_party/.
# Skips work that's already done. Run this before building CalcLang
# programs that use the gui_* builtins.
#
#   tools/setup_sdl2.sh
#
# After it succeeds, calcnat auto-detects the vendored libs and links
# every program against them; non-GUI programs aren't affected.

set -e

mkdir -p third_party

# --- SDL2 ----------------------------------------------------------
SDL2_VERSION="2.30.10"
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.tar.gz"
SDL2_DEST="third_party/SDL2-${SDL2_VERSION}"
SDL2_TARBALL="third_party/SDL2-${SDL2_VERSION}-mingw.tar.gz"

fetch() {
    url="$1"
    out="$2"
    echo "Downloading $url..."
    if command -v curl >/dev/null 2>&1; then
        curl -L -o "$out" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$out" "$url"
    else
        echo "setup_sdl2: neither curl nor wget found. Install one and retry." >&2
        exit 1
    fi
}

if [ -d "${SDL2_DEST}" ]; then
    echo "SDL2 already at ${SDL2_DEST}."
else
    if [ ! -f "${SDL2_TARBALL}" ]; then
        fetch "${SDL2_URL}" "${SDL2_TARBALL}"
    fi
    echo "Extracting ${SDL2_TARBALL}..."
    tar xzf "${SDL2_TARBALL}" -C third_party
    if [ ! -d "${SDL2_DEST}" ]; then
        echo "setup_sdl2: tarball did not produce ${SDL2_DEST}." >&2
        exit 1
    fi
fi

# --- stb_truetype --------------------------------------------------
STB_URL="https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h"
STB_DEST="third_party/stb/stb_truetype.h"
if [ -f "${STB_DEST}" ]; then
    echo "stb_truetype already at ${STB_DEST}."
else
    mkdir -p third_party/stb
    fetch "${STB_URL}" "${STB_DEST}"
fi

echo
echo "GUI runtime libraries are set up:"
echo "  SDL2:          ${SDL2_DEST}"
echo "  stb_truetype:  ${STB_DEST}"
echo
echo "gui_text uses the host's system font (Segoe UI on Windows,"
echo "Helvetica on macOS, DejaVu Sans on Linux) — nothing else to install."
echo
echo "Now rebuild calcnat and try a GUI demo:"
echo "    make                                     # builds calcnat + everything else"
echo "    build/calcnat examples/tetris_gui.calc -o build/tetris_gui.exe"
echo "    build/tetris_gui.exe"
