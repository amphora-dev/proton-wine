#!/usr/bin/env bash
# Post-install packer for Amphora: strip symbols, drop unused host tools,
# emit a single Proton-type .wcp (zstd). Keeps i386-windows for WoW64.
set -euo pipefail

OUTPUT_DIR=""
PREFIX_PACK=""
OUT=""
SHA="unknown"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    --prefix-pack) PREFIX_PACK="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --sha) SHA="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$OUTPUT_DIR" && -d "$OUTPUT_DIR" ]] || { echo "missing --output-dir"; exit 1; }
[[ -n "$PREFIX_PACK" && -f "$PREFIX_PACK" ]] || { echo "missing --prefix-pack"; exit 1; }
[[ -n "$OUT" ]] || { echo "missing --out"; exit 1; }

STRIP_BIN="${STRIP:-strip}"
if command -v llvm-strip >/dev/null 2>&1; then
  STRIP_BIN=llvm-strip
fi

echo "== Amphora pack =="
echo "output_dir=$OUTPUT_DIR"
echo "strip=$STRIP_BIN"

before=$(du -sb "$OUTPUT_DIR" | awk '{print $1}')

# Keep all Wine-installed bin tools (notepad, winemine, winecfg, winedbg, …).
# Size wins come from strip only — do not delete Wine functionality.

# Strip ELF unix libs + PE dll/exe (biggest win on x86_64-windows / i386-windows).
echo "Stripping binaries..."
find "$OUTPUT_DIR/lib/wine" \( -name '*.so' -o -name '*.dll' -o -name '*.exe' -o -name 'wine' -o -name 'wineserver' -o -name 'wine-preloader' \) -type f -print0 \
  | while IFS= read -r -d '' f; do
      "$STRIP_BIN" -s "$f" 2>/dev/null || true
    done
# Also strip real ELF bins under bin/ (wineserver may live here on some layouts).
find "$OUTPUT_DIR/bin" -type f -print0 \
  | while IFS= read -r -d '' f; do
      if file -b "$f" | grep -q 'ELF'; then
        "$STRIP_BIN" -s "$f" 2>/dev/null || true
      fi
    done

# Ensure Amphora-friendly symlink launchers (never shell+box64 wrappers).
if [[ -e "$OUTPUT_DIR/lib/wine/x86_64-unix/wine" ]]; then
  ln -sfn ../lib/wine/x86_64-unix/wine "$OUTPUT_DIR/bin/wine"
fi
if [[ -e "$OUTPUT_DIR/lib/wine/x86_64-unix/wine-preloader" ]]; then
  ln -sfn ../lib/wine/x86_64-unix/wine-preloader "$OUTPUT_DIR/bin/wine-preloader"
fi
# If wineserver was wrapped as wineserver.bin + shell, prefer the ELF.
if [[ -f "$OUTPUT_DIR/bin/wineserver.bin" ]]; then
  mv -f "$OUTPUT_DIR/bin/wineserver.bin" "$OUTPUT_DIR/bin/wineserver"
fi
# Drop any leftover shell wrappers on wine only (keep real tools intact).
if [[ -f "$OUTPUT_DIR/bin/wine" ]] && head -c 2 "$OUTPUT_DIR/bin/wine" 2>/dev/null | grep -q '#!'; then
  ln -sfn ../lib/wine/x86_64-unix/wine "$OUTPUT_DIR/bin/wine"
fi

after=$(du -sb "$OUTPUT_DIR" | awk '{print $1}')
echo "tree bytes before_strip≈$before after=$after"

cp -f "$PREFIX_PACK" "$OUTPUT_DIR/prefixPack.txz"

cat > "$OUTPUT_DIR/profile.json" <<EOF
{
  "type": "Proton",
  "versionName": "11.0-amphora-x86_64",
  "versionCode": 1,
  "description": "Amphora Proton 11 x86_64 (stripped, symlink wine, sdk35/16kb). commit ${SHA}",
  "files": [],
  "wine": {
    "binPath": "bin",
    "libPath": "lib",
    "prefixPack": "prefixPack.txz"
  }
}
EOF

# Single Proton package; zstd matches WinNative-Components convention.
cd "$OUTPUT_DIR"
tar cf - bin lib share prefixPack.txz profile.json | zstd -T0 -19 -o "$OUT"
ls -lah "$OUT"
echo "OK wrote $OUT"
