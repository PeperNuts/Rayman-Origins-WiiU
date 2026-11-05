#!/bin/bash
# Assemble an installable Wii U title folder (code/, meta/, content/) for NUSPacker.
#
#   make_title.sh OUT RPX GLSLCOMPILER DISC_FILES MODS ICON SPLASH [LOGO] [SOUND]
#
#   RPX           the built rayman.rpx
#   GLSLCOMPILER  glslcompiler.rpl (CafeGLSL) - goes next to the RPX in code/
#   DISC_FILES    the disc's files/ folder (your own copy of the game)
#   MODS          the mods folder (localisation/, icons/) - see the README
#   ICON SPLASH   your square and wide pictures; LOGO and SOUND are optional
#
# Then pack it with your own Wii U common key (not included):
#   java -jar NUSPacker.jar -in OUT -out "Rayman Origins" -encryptKeyWith <common key>
set -e
[ $# -ge 7 ] || { sed -n '3,14p' "$0"; exit 1; }
OUT=$1 RPX=$2 GLSL=$3 FILES=$4 MODS=$5 ICON=$6 SPLASH=$7 LOGO=${8:-} SOUND=${9:-}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/code" "$OUT/meta" "$OUT/content"
cp "$HERE/../title/app.xml" "$HERE/../title/cos.xml" "$OUT/code/"
cp "$RPX" "$OUT/code/rayman.rpx"
cp "$GLSL" "$OUT/code/glslcompiler.rpl"
cp "$HERE/../title/meta.xml" "$OUT/meta/"
args=("$OUT/meta" --icon "$ICON" --splash "$SPLASH")
[ -n "$LOGO" ] && args+=(--logo "$LOGO")
[ -n "$SOUND" ] && args+=(--sound "$SOUND")
python3 "$HERE/make_title_meta.py" "${args[@]}"
mkdir -p "$OUT/content/files" "$OUT/content/mods"
cp -r "$FILES/." "$OUT/content/files/"
find "$OUT/content/files" -name '*.ok' -delete   # extraction markers
cp -r "$MODS/." "$OUT/content/mods/"
echo "title folder ready: $OUT ($(du -sh "$OUT" | cut -f1))"
