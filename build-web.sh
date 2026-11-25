#!/usr/bin/env bash
# Build the Velvet UI for the browser -> web/velvet-web.js (single file, works from file://).
# Needs Emscripten: git clone https://github.com/emscripten-core/emsdk ~/emsdk && cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
set -euo pipefail
source ~/emsdk/emsdk_env.sh >/dev/null
cd "$(dirname "$0")"
LVGL=$HOME/cardputerzero/src/Template/build/desktop/_deps/lvgl-src
emcmake cmake -S web -B build/web -G Ninja -DCMAKE_BUILD_TYPE=Release -DLVGL_DIR="$LVGL"
cmake --build build/web -j"$(nproc)"
cp build/web/velvet-web.js web/velvet-web.js
ls -la web/velvet-web.js

# one-file copy of the page to send to people
python3 -I tools/bundle-html.py
