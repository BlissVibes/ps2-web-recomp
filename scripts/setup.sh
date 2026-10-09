#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMSDK="${EMSDK:-$ROOT/tools/emsdk}"
EMSDK_VERSION="6.0.11"

if [ ! -f "$EMSDK/emsdk_env.sh" ]; then
  echo "== installing the Emscripten SDK $EMSDK_VERSION into $EMSDK"
  mkdir -p "$(dirname "$EMSDK")"
  git clone https://github.com/emscripten-core/emsdk.git "$EMSDK"
  "$EMSDK/emsdk" install "$EMSDK_VERSION"
  "$EMSDK/emsdk" activate "$EMSDK_VERSION"
fi

if [ ! -f "$ROOT/third_party/wgpu-native/lib/libwgpu_native.a" ]; then
  echo "== fetching wgpu-native for the native test runner"
  "$ROOT/scripts/fetch_wgpu.sh"
fi

compilers=()
if command -v clang++ > /dev/null; then compilers=(-DCMAKE_CXX_COMPILER=clang++); fi
cmake -S "$ROOT" -B "$ROOT/build/tools" -G Ninja -DCMAKE_BUILD_TYPE=Release "${compilers[@]}" > /dev/null
ninja -C "$ROOT/build/tools" eerecomp vurecomp
echo "== ready"
