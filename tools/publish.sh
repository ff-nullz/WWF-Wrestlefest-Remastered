#!/bin/sh
# Build the PUBLIC tree for GitHub - a fresh single-commit repository with no history and
# none of the ROM-derived content (the engine regenerates it from the player's own ROM:
# tools/bootstrap.sh). Nothing from the private history, HANDOFF or the working notes.
#
#   tools/publish.sh OUTDIR [--skins] [--push]
#     --skins   also ship the two AI skins (Undertaker, Duggan) with the superstars + wardrobe mods
#               and profiles that seat them: drawings of real wrestlers, your call
#     --push    push OUTDIR to the GitHub remote as main (force: the public repo is a mirror)
#
# Author on the public commit: the noreply identity below (the private history carries a
# personal address).
set -e
cd "$(dirname "$0")/.." || exit 2
OUT=$1; shift; [ -n "$OUT" ] || { echo "usage: tools/publish.sh OUTDIR [--skins] [--push]"; exit 2; }
SKINS=0; PUSH=0
for a in "$@"; do case $a in --skins) SKINS=1;; --push) PUSH=1;; esac; done
REMOTE="git@github.com:ff-nullz/WWF-Wrestlefest-Remastered.git"
AUTHOR="ff-nullz <ff-nullz@users.noreply.github.com>"

rm -rf "$OUT"; mkdir -p "$OUT"
# --- the allowlist (rsync from the committed tree, never the working files) ---
T=$(mktemp -d); git archive HEAD | tar -x -C "$T"
copy() { # relative path under the archive
  [ -e "$T/$1" ] || return 0; mkdir -p "$OUT/$(dirname "$1")"; cp -r "$T/$1" "$OUT/$1"; }
for f in README.md LICENSE Makefile build.sh baseline.txt src tools docs profiles; do copy "$f"; done
# mods: the JSON only (rules layers, wrestler/palette/stat json), plus the project's own weapon art
( cd "$T" && find mods -type f \( -name '*.json' -o -path 'mods/hardcore/*' \) ) | while read -r f; do copy "$f"; done
# data: the project's own files - weapon and badge art, the derived indexes; NOT the ROM exports
for f in data/weapons data/badges data/classes.json data/move-names.json data/movecatalog.json data/moveposes.json data/poseindex.json data/framecoverage.json data/sound-names.json data/sound-commands.txt data/sstar-posemap.txt; do copy "$f"; done
# the body-class calibration (grapple alignment, victim maps): the project's own tuning, JSON only - the
# class frame art (data/classes/N/generic, AI drawings) stays out
( cd "$T" && find data/classes \( -name 'calib.json' -o -name 'victmap.json' -o -name 'victoffs.json' -o -name 'depth.json' -o -name 'wrestler.json' -o -name 'stats.json' -o -name 'skin.json' \) ) | while read -r f; do copy "$f"; done
# arenas: the layered arena files (the three stock arenas as PNG layers - 5 MB of the game's own
# backdrops, needed for the cage to draw right - and the codex-made templates)
copy arenas
if [ $SKINS = 1 ]; then   # the two AI skins and the mods that seat them (Undertaker = superstars slot 13, Duggan = wardrobe slot 12)
  for f in skins/duggan skins/undertaker skins/defaults.json mods/superstars mods/wardrobe data/select; do copy "$f"; done
  rm -rf "$OUT/mods/newart" "$OUT/profiles/newart.json" "$OUT/profiles/generics.json" "$OUT/mods/generics"
else
  rm -rf "$OUT/mods/superstars" "$OUT/mods/wardrobe" "$OUT/mods/newart" "$OUT/mods/generics" \
         "$OUT/profiles/superstars.json" "$OUT/profiles/wardrobe.json" "$OUT/profiles/newart.json" "$OUT/profiles/generics.json"
fi
# video: every clip ships; only the stitched 100 MB per-profile reel is dropped (over GitHub's file limit) - its clips are there one per profile
rm -f "$OUT/docs/video/mods-reel.mp4"
rm -rf "$OUT/mods/superstars/wrestlers/14.broken~" "$OUT/docs/rom-unmapped-2026-08-23.txt" "$OUT/docs/rom-coverage-2026-08-23.txt"
# never
for f in "$OUT"/*.md; do [ "$(basename "$f")" = README.md ] || rm -f "$f"; done   # only the README at the top level
rm -rf "$OUT/reference" "$OUT/rom" "$OUT/build" "$OUT/jobs" "$OUT/exports"
mkdir -p "$OUT/rom"; printf "Put the MAME wwfwfest set here - wwfwfest.zip as-is (bootstrap unzips it), or its unzipped files.\n" > "$OUT/rom/README.md"   # ships empty: the clone builds without a mkdir step
rm -rf "$T"
# a public .gitignore: the bootstrap's outputs stay out of the repo
cat > "$OUT/.gitignore" <<'IG'
wfengine
*.ppm
build/
rom/*
!rom/README.md
jobs/
exports/
data/tables/
data/wrestlers/
data/gfx-edit/
data/sound/
data/sounds/
data/stockskins/
data/music/
data/generics/
data/classes/
data/stock.json
IG
# leak check on the tree we are about to publish
HITS=$(grep -rIiE 'sk-[a-z]+-|[A-Z_]+_API_KEY=|/home/[a-z]+/|richarpad|rah2402' "$OUT" --exclude-dir=.git | grep -v 'tools/publish.sh' | head -5)
if [ -n "$HITS" ]; then echo "$HITS"; echo "publish: LEAK CHECK HIT (above) - not published"; exit 1; fi
( cd "$OUT" && git init -q -b main && git add -A && git -c user.name="${AUTHOR%% <*}" -c user.email="$(echo "$AUTHOR" | sed 's/.*<//; s/>//')" commit -q -m "WWF WrestleFest Remastered - public tree" --author="$AUTHOR" && git remote add origin "$REMOTE" )
echo "publish: $OUT ready - $(cd "$OUT" && git ls-files | wc -l) files, $(du -sh "$OUT" --exclude=.git | cut -f1); remote $REMOTE"
if [ $PUSH = 1 ]; then ( cd "$OUT" && git push --force origin main ); fi
