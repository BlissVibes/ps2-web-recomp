#!/bin/bash
# Builds the web version of a recompiled game into build/web.
# usage: scripts/build_web.sh <game folder under games/> [emsdk dir]
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
game=${1:-gt4}
emsdk=${2:-${EMSDK:-$here/../webGame/tools/emsdk}}
source "$emsdk/emsdk_env.sh" > /dev/null 2>&1
emcmake cmake -S "$here" -B "$here/build/web" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2_GENERATED="$here/generated/$game" -DPS2_WEB_ELF="$here/games/$game/core.elf"
ninja -C "$here/build/web" ps2web
gzip -kf9 "$here/build/web/ps2web.wasm" "$here/build/web/ps2web.data" "$here/build/web/ps2web.js"
echo "built $here/build/web/ps2web.html; serve it with scripts/serve.py build/web games/$game/disc.iso"
