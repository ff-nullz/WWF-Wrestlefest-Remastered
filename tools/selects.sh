#!/bin/sh
# Select-screen showcases: the extended grid picks, from SELECT PLAYERS to the match.
#   tools/selects.sh  -> docs/video/sel-<name>.mp4 + docs/img/game/sel-<name>.gif
# Each drive: clamp the cursor RIGHT (taps swallow during page scrolls - overshoot is
# deterministic), seat the pick(s), then play the opening of the match. The dead middle
# of the walkout (frames 720..929) is dropped - the black scene change hides the cut.
cd "$(dirname "$0")/.." || exit 2
OUT=docs/video; GIF=docs/img/game; mkdir -p "$OUT" "$GIF"; T=$(mktemp -d); FPS=57.44
R="50-51:1,70-71:1,90-91:1,110-111:1,130-131:1,150-151:1,170-171:1,190-191:1,210-211:1"
M="1000-1030:1,1040-1041:10,1060-1061:10,1100-1130:2,1140-1141:10,1180-1181:10,1220-1250:1,1260-1261:10,1300-1301:10,1340-1400:2,1420-1421:10,1460-1461:10,1500-1530:1,1540-1541:10"
title() { python3 - "$1" "$2" "$3" <<'PY'
import sys
from PIL import Image, ImageDraw, ImageFont
out, ti, sub = sys.argv[1:4]
im = Image.new('RGB', (640, 480), (12, 12, 20)); d = ImageDraw.Draw(im)
f1 = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 40)
f2 = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 22)
w = d.textlength(ti, font=f1); d.text(((640 - w) / 2, 170), ti, font=f1, fill=(255, 210, 60))
y = 235
for line in sub.split('|'):
    w = d.textlength(line, font=f2); d.text(((640 - w) / 2, y), line, font=f2, fill=(220, 220, 230)); y += 30
im.save(out)
PY
}
clip() { # name "title" "subtitle" profile "picks-script"
  n=$1; ti=$2; sub=$3; p=$4; pk=$5
  rm -rf "$T/$n"; mkdir -p "$T/$n"
  env WF_SEATED=1 WF_MODRULES=cpu_energy_meters=1 WF_P1="$R,$pk,$M" WF_SHOT_EVERY=1 WF_SHOT_DIR="$T/$n" \
    ./wfengine --headless --profile "$p" --frames 1700 --drive charselect >/dev/null 2>&1
  for f in "$T/$n"/f*.ppm; do k=$(basename "$f" .ppm | sed 's/^f0*//'); [ "${k:-0}" -ge 720 ] && [ "${k:-0}" -lt 930 ] && rm -f "$f"; done
  title "$T/$n/title.png" "$ti" "$sub"
  ffmpeg -hide_banner -loglevel error -y -loop 1 -framerate $FPS -t 2.5 -i "$T/$n/title.png" -c:v libx264 -crf 22 -pix_fmt yuv420p -r $FPS "$T/$n/title.mp4"
  ffmpeg -hide_banner -loglevel error -y -framerate $FPS -pattern_type glob -i "$T/$n/f*.ppm" -vf "scale=640:480:flags=neighbor" -c:v libx264 -crf 22 -pix_fmt yuv420p "$T/$n/game.mp4"
  printf "file '%s/title.mp4'\nfile '%s/game.mp4'\n" "$T/$n" "$T/$n" > "$T/$n/cat.txt"
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i "$T/$n/cat.txt" -c copy "$OUT/sel-$n.mp4"
  for fps in 15 12 10 8; do   # the GIF skips the title card; GitHub stops rendering over ~8 MB
    ffmpeg -hide_banner -loglevel error -y -i "$T/$n/game.mp4" -vf "fps=$fps,scale=320:-1:flags=neighbor,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=none" -loop 0 "$GIF/sel-$n.gif"
    [ "$(stat -c %s "$GIF/sel-$n.gif")" -lt 8000000 ] && break
  done
  rm -rf "$T/$n"; echo "select: $OUT/sel-$n.mp4 $GIF/sel-$n.gif $(du -h "$GIF/sel-$n.gif" | cut -f1)"
}
# legends: the LOD cells sit left of the col-5 clone - clamp right, one LEFT = Hawk, DOWN = Animal
clip legends    "Legion of Doom"    "select_extended: Hawk and Animal on the grid|picked as a team, played from the bell" legends    "260-261:2,330-331:10,380-381:8,430-431:10"
# superstars: clamp right = Duggan (pos 12), DOWN = Undertaker (pos 13)
clip undertaker "Undertaker"        "picked from the extended grid|an AI-drawn skin on a Warrior-class body" superstars "260-261:8,330-331:10,380-381:2,430-431:10"
# wardrobe: clamp right = Duggan (pos 12); partner LEFT = Hawk
clip duggan     "Duggan"            "picked from the extended grid|an AI-drawn skin on a Hogan-class body"   wardrobe   "300-302:10,360-361:2,420-422:10"
rm -rf "$T"
