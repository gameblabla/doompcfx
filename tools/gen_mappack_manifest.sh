#!/bin/bash
# gen_mappack_manifest.sh -- regenerate generated/mappack_manifest.txt, the authoritative
# per-map precache set the pack builder (tools/gen_pcfx_packs.py) consumes.
#
# For each E1 map it builds the engine with -DGEN_MAPPACK_MANIFEST (which makes
# R_PrecacheLevel dump its reserved lump set, in reserve order, to the serial log),
# runs it headless warped to that map, and extracts the dump from the RAM image. Maps
# that fail to load (e.g. a pre-existing map-load OOM) are skipped with a warning.
#
# One build+run per map, only needed when doom1.wad or the precache logic changes;
# the result is checked in so normal builds never run the emulator.
#
# FRAMES only needs to cover boot + CD-load of the level far enough for
# R_PrecacheLevel/W_PrecacheEnd to fire and log the MAPPACK dump -- it does NOT
# need to simulate real play. That happens within the first ~150-450 engine
# frames after warp (measured: E1M1 lands its dump by frame ~150, E1M9 -- the
# heaviest map -- by ~450), so 8000 is a wide safety margin, not a tight bound.
# The previous default of 80000 was 10x oversized and cost ~5 min/map (~45 min
# total across BOOT + 9 maps) for no extra coverage; 8000 measures at ~30s/map
# (~5 min total) with the same result. If a future map's load genuinely needs
# more headroom, raise FRAMES for that run rather than the shared default.
set -u
cd "$(dirname "$0")/.."
WAD="${1:-../doom1.wad}"
OUT=generated/mappack_manifest.txt
BIOSDIR="$PWD/.."
FRAMES="${FRAMES:-8000}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

extract() { # $1 = ram dump -> stdout "MAP <name> <n>\n<num> <NAME>..." or nonzero
python3 - "$1" <<'PY'
import sys,struct
d=open(sys.argv[1],'rb').read(); i=d.find(b'PCFXLOG!')
if i<0: sys.exit(1)
ln=struct.unpack_from('<I',d,i+8)[0]; t=d[i+12:i+12+ln].decode('latin1','replace').splitlines()
h=next((l for l in t if l.startswith('MAPPACK ')),None)
if not h: sys.exit(1)
e=[(l.split()[1],l.split()[2]) for l in t if l.startswith('MP ') and len(l.split())>=3]
print(f'MAP {h.split()[1]} {len(e)}')
for n,nm in e: print(n,nm)
PY
}

mkdir -p generated
{
  echo "# PC-FX DOOM per-map asset manifest -- authoritative reserve set + order dumped by"
  echo "# the engine's R_PrecacheLevel (build -DGEN_MAPPACK_MANIFEST). Regenerate with"
  echo "# 'make mappack-manifest'. Lines: 'MAP <name> <n>' then '<lumpnum> <NAME>' x n."
} > "$OUT"

# BOOT pack: the whole-run resident UI set + TITLEPIC. Unlike the maps this run does
# NOT warp — it boots to the title so W_CacheLumpNum's ground-truth capture sees the
# real boot-resident set (dumped by W_BootManifestDump at the first title frame). Emit
# it FIRST so the BOOT pack sits at the front of pcfx_mappacks.bin.
echo "=== BOOT ==="
rm -rf build doom_pcfx.bin
if make -j"$JOBS" DOOM1WAD="$WAD" EXTRA="-DGEN_BOOTPACK_MANIFEST" >/dev/null 2>&1; then
  env PCFX_BIOS_DIR="$BIOSDIR" pcfx-headless --bios-dir "$BIOSDIR" --pcfx --auto-run \
    --frames "${BOOT_FRAMES:-8000}" --dump ram "$TMP/boot.bin" doom_pcfx.cue >/dev/null 2>&1
  if extract "$TMP/boot.bin" >> "$OUT" 2>/dev/null; then
    echo "  ok: $(extract "$TMP/boot.bin" | head -1)"
  else
    echo "  WARN: BOOT produced no manifest (title not reached?) -- no boot pack"
  fi
else
  echo "  build failed, skipping BOOT"
fi

for M in 1 2 3 4 5 6 7 8 9; do
  echo "=== E1M$M ==="
  rm -rf build doom_pcfx.bin
  make -j"$JOBS" DOOM1WAD="$WAD" EXTRA="-DDEV_WARP -DDEV_WARP_MAP=$M -DGEN_MAPPACK_MANIFEST" >/dev/null 2>&1 \
    || { echo "  build failed, skipping"; continue; }
  env PCFX_BIOS_DIR="$BIOSDIR" pcfx-headless --bios-dir "$BIOSDIR" --pcfx --auto-run \
    --frames "$FRAMES" --dump ram "$TMP/mm$M.bin" doom_pcfx.cue >/dev/null 2>&1
  if extract "$TMP/mm$M.bin" >> "$OUT" 2>/dev/null; then
    echo "  ok: $(grep -c '^[0-9]' <(extract "$TMP/mm$M.bin")) lumps"
  else
    echo "  WARN: E1M$M produced no manifest (map-load failure?) -- no pack for it"
  fi
done
echo "=== manifest maps ==="; grep '^MAP ' "$OUT"
