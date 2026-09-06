#!/bin/sh
# Bootstrap the data tree from YOUR ROM set (see README "Clone and build"):
#   rom/  = the MAME wwfwfest set (not distributed)   ->   data/ (tables, gfx, wrestlers, sounds, stock skins)
# Run once after cloning, then ./build.sh. Safe to re-run (files are rewritten).
set -e
cd "$(dirname "$0")/.." || exit 2
[ -d rom ] || { echo "bootstrap: put the wwfwfest ROM set in ./rom first (see README)"; exit 1; }
for z in rom/*.zip; do   # a wwfwfest.zip dropped in whole is fine - unzipped in place
  [ -f "$z" ] || continue
  command -v unzip >/dev/null || { echo "bootstrap: $z found but no unzip - sudo apt install unzip"; exit 1; }
  unzip -o -q "$z" -d rom && echo "bootstrap: unzipped $(basename "$z")"
done
ls rom/*.ic* >/dev/null 2>&1 || { echo "bootstrap: no ROM files in ./rom - copy wwfwfest.zip (or its unzipped files) there first"; exit 1; }
[ -x ./wfengine ] || { echo "bootstrap: compiling (a minute) ..."; make -s wfengine 2>&1 | grep -E " error|Error " || true; }
echo "bootstrap: graphics from the ROM chips -> build/gfx.pak ..."; ./wfengine --export-gfx build/gfx.pak 2>&1 | grep -E 'export-gfx|FAILED' | tail -1
export WF_DATA=rom
echo "bootstrap: tables ..."; ./wfengine --export-all data/tables 2>&1 | grep -E 'export:|failed' | tail -2
echo "bootstrap: wrestlers 0..11 ..."; mkdir -p data/wrestlers; for i in 0 1 2 3 4 5 6 7 8 9 10 11; do ./wfengine --export $i data/wrestlers 2>&1 | grep -iE 'fail|error|out of range' || true; done
echo "bootstrap: sound ROMs ..."; ./wfengine --export-sound 2>&1 | grep -iE 'sound|fail' | tail -1
unset WF_DATA
echo "bootstrap: packing ..."; ./build.sh --no-bump 2>&1 | grep -E 'built|failed' | grep -v ' 0 failed' | tail -2
n=$(ls build/wrestlers/*.pak 2>/dev/null | wc -l); echo "bootstrap: $n wrestler paks in build/wrestlers"
if [ "$1" = "--skins" ]; then   # optional: the 12 stock men as editable skin sheets (data/stockskins, ~100 MB; the editor's Skins / Classes tabs)
    echo "bootstrap: stock skins (a minute) ..."; ./wfengine --stock-skins 2>&1 | grep -iE 'stock-skins' | tail -1
fi
echo "bootstrap: done - ./wfengine plays stock; ./wfengine --profiles lists the mods"
