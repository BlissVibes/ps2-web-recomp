#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
here="$ROOT/scripts"

usage() {
  cat <<'EOF'
usage: scripts/port.sh <name> <game.iso | game.zip | game.7z> [options]

Turns a PS2 disc image you own into a browser build in build/web-<name>. It
reads the boot program named in SYSTEM.CNF, translates its machine code to
C++, runs it headless to find code the static pass missed (adding those entry
points and translating again), compiles the vector unit programs those runs
recorded and builds the page.

options:
  --frames N         frames per exploring run (default 3600)
  --press F:BUTTON   input for the exploring runs, repeatable; the default
                     presses start at frame 300 and cross every 300 frames
  --rounds N         exploring runs at most (default 8)
  --no-explore       translate and build without running the game
EOF
}

[ $# -ge 2 ] || { usage; exit 2; }
name="$1"; source_path="$2"; shift 2
frames=3600; rounds=8; explore=1; presses=()
while [ $# -gt 0 ]; do
  case "$1" in
    --frames) frames="$2"; shift ;;
    --press) presses+=(--press "$2"); shift ;;
    --rounds) rounds="$2"; shift ;;
    --no-explore) explore=0 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
if [ ${#presses[@]} -eq 0 ]; then
  presses=(--press 300:start)
  for ((f = 600; f < frames; f += 300)); do presses+=(--press "$f:cross"); done
fi

game="$ROOT/games/$name"
generated="$ROOT/generated/$name"
native="$ROOT/build/native-$name"
tools="$ROOT/build/tools"
export EMSDK="${EMSDK:-$ROOT/tools/emsdk}"
if [ ! -f "$EMSDK/emsdk_env.sh" ] || [ ! -x "$tools/eerecomp" ] || [ ! -x "$tools/vurecomp" ]; then "$here/setup.sh"; fi
mkdir -p "$game/disc" "$game/vu1" "$game/shots" "$generated"

echo "== [1/6] disc"
iso="$game/disc.iso"
if [ -e "$iso" ]; then
  echo "already in place"
else
  case "${source_path,,}" in
    *.zip)
      python3 -I - "$source_path" "$iso" <<'EOF'
import shutil, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    images = [m for m in archive.infolist() if m.filename.lower().endswith('.iso')]
    if not images:
        sys.exit('no .iso inside the archive')
    with archive.open(max(images, key=lambda m: m.file_size)) as src, open(sys.argv[2], 'wb') as dst:
        shutil.copyfileobj(src, dst, 1 << 24)
EOF
      ;;
    *.7z) 7z e -so -r "$source_path" '*.iso' > "$iso" ;;
    *) ln -s "$(realpath "$source_path")" "$iso" ;;
  esac
fi
python3 -I "$here/disc.py" extract "$iso" "$game/disc" SYSTEM.CNF > /dev/null
boot=$(sed -n 's/^BOOT2 *= *//p' "$game/disc/SYSTEM.CNF" | tr -d '\r')
program=${boot#*\\}
program=${program%;*}
[ -n "$program" ] || { echo "SYSTEM.CNF names no BOOT2 program" >&2; exit 1; }
python3 -I "$here/disc.py" extract "$iso" "$game/disc" "$program" > /dev/null
echo "boots $boot"

echo "== [2/6] executable"
elf="$game/disc/$program"
if [ -f "$game/core.elf" ]; then
  elf="$game/core.elf"
elif python3 -I "$here/disc.py" list "$iso" | awk '{print $4}' | grep -qx CORE.GT4; then
  python3 -I "$here/disc.py" extract "$iso" "$game/disc" CORE.GT4 > /dev/null
  python3 -I "$here/gt4_core.py" "$game/disc/CORE.GT4" "$game/core.elf" > /dev/null
  elf="$game/core.elf"
fi
echo "${elf#$ROOT/}"

translate() { "$tools/eerecomp" "$elf" --entries "$game/entries.txt" --out "$generated" | tail -1; }

echo "== [3/6] machine code to C++"
touch "$game/entries.txt"
translate

echo "== [4/6] exploring"
if [ "$explore" = 1 ]; then
  if [ ! -f "$native/build.ninja" ]; then
    compilers=()
    if command -v clang++ > /dev/null; then compilers=(-DCMAKE_CXX_COMPILER=clang++); fi
    cmake -S "$ROOT" -B "$native" -G Ninja -DCMAKE_BUILD_TYPE=Release "${compilers[@]}" -DPS2_GENERATED="$generated" > /dev/null
  fi
  for ((round = 1; round <= rounds; round++)); do
    ninja -C "$native" ps2run > "$native/ninja.log" 2>&1 || { tail -30 "$native/ninja.log"; exit 1; }
    rm -f "$game/misses.txt"
    PS2_GPU_LOW_POWER=1 PS2_VU_RECORD="$game/vu1" "$native/ps2run" "$elf" --disc "$iso" --misses "$game/misses.txt" \
      --frames "$frames" "${presses[@]}" --frames-dir "$game/shots" --frame-every 300 > "$game/explore.log" 2>&1 || true
    reason=$(sed -n 's/^\[ee\] halted: //p' "$game/explore.log" | head -1)
    added=$(python3 -I - "$game/entries.txt" "$game/misses.txt" <<'EOF'
import os, sys
entries, misses = sys.argv[1], sys.argv[2]
known = {int(t, 16) for t in open(entries).read().split()}
found = [int(t, 16) for t in open(misses).read().split()] if os.path.exists(misses) else []
new = sorted({a for a in found if a not in known})
with open(entries, 'a') as out:
    for a in new:
        out.write('%x\n' % a)
print(len(new))
EOF
)
    echo "round $round: ${reason:-ran to the end}, $added new entry points"
    [ "$added" -gt 0 ] || break
    translate
  done
  echo "frames of the last run are in games/$name/shots, its log in games/$name/explore.log"
else
  echo "skipped"
fi

echo "== [5/6] vector unit programs"
if compgen -G "$game/vu1/*.bin" > /dev/null; then
  "$tools/vurecomp" "$game/vu1" "$generated" | tail -1
else
  echo "none recorded, the page compiles them on the fly"
fi

echo "== [6/6] web build"
"$here/build_web.sh" "$name" "$EMSDK"
echo
echo "Done. Serve it with:  python3 scripts/serve.py build/web-$name games/$name/disc.iso"
echo "and open http://localhost:8620/"
