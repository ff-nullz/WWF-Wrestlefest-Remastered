#!/bin/sh
# Animated GIFs for the README (GitHub plays GIFs inline; it will not play a big mp4).
#   tools/gifs.sh  -> docs/img/game/hl-<mod>.gif       (the highlight clips, title card kept)
#                     docs/img/game/<profile>.gif      (the mods-table cells, title card skipped)
# All native 320 wide, palette per clip; each GIF held under 8 MB (GitHub stops rendering
# big images) by stepping the frame rate down. The sel-*.gif set is made by tools/selects.sh.
cd "$(dirname "$0")/.." || exit 2
gif() { # src dst seek
  m=$1; o=$2; ss=$3
  [ -f "$m" ] || { echo "gif: no $m"; return 0; }
  w=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$m")
  [ "$w" -gt 640 ] && sc="scale=640:-1:flags=neighbor" || sc="scale=320:-1:flags=neighbor"
  for fps in 15 12 10 8 6; do
    ffmpeg -hide_banner -loglevel error -y ${ss:+-ss $ss} -i "$m" -vf "fps=$fps,$sc,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=none" -loop 0 "$o"
    [ "$(stat -c %s "$o")" -lt 8000000 ] && break
  done
  echo "gif: $o $(du -h "$o" | cut -f1)"
}
for m in docs/video/hl-*.mp4; do
  gif "$m" "docs/img/game/$(basename "$m" .mp4).gif" ""
done
# the mods table + the hero (stock): one cell per profile clip, card skipped
for n in stock heavyhits parasitic turbo vampire-turbo battle-royale-12 battle-royale \
         cagematch exit-ring chaos run-ins ref-ko hardcore survivor handicap one-on-one \
         mirror precision meters gauge widescreen fixedview trainer legends clones \
         skin-undertaker skin-duggan; do
  gif "docs/video/$n.mp4" "docs/img/game/$n.gif" 2
done
