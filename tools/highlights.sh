#!/bin/sh
# The mods built in this repo, each shown by a scripted drive that makes the feature obvious.
#   tools/highlights.sh  -> docs/video/hl-<name>.mp4 + docs/video/highlights.mp4
cd "$(dirname "$0")/.." || exit 2
OUT=docs/video; mkdir -p "$OUT"; T=$(mktemp -d); FPS=57.44; LIST="$T/list.txt"; : > "$LIST"
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
finish() { # name: title.png + game.mp4 in $T/$1 -> $OUT/hl-$1.mp4
  n=$1
  ffmpeg -hide_banner -loglevel error -y -loop 1 -framerate $FPS -t 2.5 -i "$T/$n/title.png" -c:v libx264 -crf 22 -pix_fmt yuv420p -r $FPS "$T/$n/title.mp4"
  printf "file '%s/title.mp4'\nfile '%s/game.mp4'\n" "$T/$n" "$T/$n" > "$T/$n/cat.txt"
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i "$T/$n/cat.txt" -c copy "$OUT/hl-$n.mp4"
  printf "file '%s'\n" "$(pwd)/$OUT/hl-$n.mp4" >> "$LIST"; rm -rf "$T/$n"; echo "highlight: $OUT/hl-$n.mp4"
}
clip() { # name "title" "subtitle" profile "env" frames [first-frame]
  n=$1; ti=$2; sub=$3; p=$4; e=$5; fr=$6; f0=${7:-0}
  rm -rf "$T/$n"; mkdir -p "$T/$n"
  env $e WF_SHOT_EVERY=1 WF_SHOT_DIR="$T/$n" ./wfengine --headless ${p:+--profile "$p"} --frames $fr --drive script >/dev/null 2>&1
  [ "$f0" -gt 0 ] && for f in "$T/$n"/f*.ppm; do k=$(basename "$f" .ppm | sed 's/^f0*//'); [ "${k:-0}" -lt "$f0" ] && rm -f "$f"; done
  title "$T/$n/title.png" "$ti" "$sub"
  ffmpeg -hide_banner -loglevel error -y -framerate $FPS -pattern_type glob -i "$T/$n/f*.ppm" -vf "scale=640:480:flags=neighbor" -c:v libx264 -crf 22 -pix_fmt yuv420p "$T/$n/game.mp4"
  finish "$n"
}
split() { # name "title" "subtitle" "envA" "envB" frames  (two runs side by side, the same script)
  n=$1; ti=$2; sub=$3; ea=$4; eb=$5; fr=$6
  rm -rf "$T/$n"; mkdir -p "$T/$n/a" "$T/$n/b"
  env $ea WF_SHOT_EVERY=1 WF_SHOT_DIR="$T/$n/a" ./wfengine --headless --frames $fr --drive script >/dev/null 2>&1
  env $eb WF_SHOT_EVERY=1 WF_SHOT_DIR="$T/$n/b" ./wfengine --headless --frames $fr --drive script >/dev/null 2>&1
  python3 - "$T/$n" <<'PY'
import sys, glob, os
from PIL import Image, ImageDraw, ImageFont
d = sys.argv[1]; f = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 14)
for a in sorted(glob.glob(d + '/a/f*.ppm')):
    b = d + '/b/' + os.path.basename(a)
    if not os.path.exists(b): continue
    A = Image.open(a).convert('RGB'); B = Image.open(b).convert('RGB')
    out = Image.new('RGB', (640, 240)); out.paste(A, (0, 0)); out.paste(B, (320, 0))
    dr = ImageDraw.Draw(out); dr.rectangle((0, 0, 100, 18), fill=(0, 0, 0)); dr.text((4, 2), "stock", font=f, fill=(255, 210, 60))
    dr.rectangle((320, 0, 440, 18), fill=(0, 0, 0)); dr.text((324, 2), "turbo 5", font=f, fill=(255, 210, 60))
    out.resize((1280, 480), Image.NEAREST).save(d + '/s' + os.path.basename(a)[1:7] + '.png')
PY
  python3 - "$T/$n/title.png" "$ti" "$sub" <<'PY'
import sys
from PIL import Image, ImageDraw, ImageFont
out, ti, sub = sys.argv[1:4]
im = Image.new('RGB', (1280, 480), (12, 12, 20)); d = ImageDraw.Draw(im)
f1 = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 40); f2 = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 22)
w = d.textlength(ti, font=f1); d.text(((1280 - w) / 2, 170), ti, font=f1, fill=(255, 210, 60))
y = 235
for line in sub.split('|'):
    w = d.textlength(line, font=f2); d.text(((1280 - w) / 2, y), line, font=f2, fill=(220, 220, 230)); y += 30
im.save(out)
PY
  ffmpeg -hide_banner -loglevel error -y -framerate $FPS -pattern_type glob -i "$T/$n/s*.png" -c:v libx264 -crf 22 -pix_fmt yuv420p "$T/$n/game.mp4"
  ffmpeg -hide_banner -loglevel error -y -loop 1 -framerate $FPS -t 2.5 -i "$T/$n/title.png" -c:v libx264 -crf 22 -pix_fmt yuv420p -r $FPS "$T/$n/title.mp4"
  printf "file '%s/title.mp4'\nfile '%s/game.mp4'\n" "$T/$n" "$T/$n" > "$T/$n/cat.txt"
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i "$T/$n/cat.txt" -c copy "$OUT/hl-$n.mp4"
  ffmpeg -hide_banner -loglevel error -y -i "$OUT/hl-$n.mp4" -vf "scale=640:240,pad=640:480:0:120" -c:v libx264 -crf 22 -pix_fmt yuv420p "$T/$n-reel.mp4"
  printf "file '%s'\n" "$T/$n-reel.mp4" >> "$LIST"; rm -rf "$T/$n"; echo "highlight: $OUT/hl-$n.mp4"
}
S="0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10,88-89:10,94-95:10,100-101:10,110-111:10"
P=""; for f in $(seq 90 12 400); do P="$P,$f-$((f+1)):10"; done
clip skins-undertaker-vs-duggan "Undertaker vs Duggan" "two AI-drawn skins, CPU vs CPU|Undertaker on a Warrior body, Duggan on a Hogan body" superstars "WF_W1=13 WF_W2=12 WF_CPU1=1 WF_CPU2=1" 1700
clip toprope-knockoff "extended_moves" "a strike knocks the perched man off the buckle|he lands on the mat and gets up" "" "WF_MODRULES=extended_moves=1 WF_X1=0x300 WF_Y1=0x197 WF_P1=0-120:4 WF_P2=0-36:5$P" 420
clip toprope-out "toprope_out" "the same knock-off, out of the ring" "" "WF_MODRULES=extended_moves=1,toprope_out=1 WF_X1=0x300 WF_Y1=0x197 WF_P1=0-120:4 WF_P2=0-36:5$P" 520
clip double-pin "the rumble double pin" "B2 at a pile joins the cover (stock behaviour restored)" battle-royale-12 "WF_RUMBLE=1 WF_CPU2=1 WF_X1=0x300 WF_Y1=0x190 WF_PINAT=3 WF_P1=9-10:20" 200
clip cage-climb "cagematch: the escape" "hold DOWN into the bottom wall ~2 s|climb hand over hand, over the top, drop, WIN" cagematch "WF_STAGE=2 WF_X1=0x280 WF_Y1=0x120 WF_P1=0-140:8" 620
clip exit-ring "exit_ring" "hold into a rope for half a second: the hop out" exit-ring "WF_X1=0x2E0 WF_Y1=0x190 WF_P1=0-200:1" 400
clip exit-ring-climb "exit_ring_climb" "...or climb out through the ropes" "" "WF_MODRULES=exit_ring=1,exit_ring_climb=1 WF_X1=0x2E0 WF_Y1=0x190 WF_P1=0-200:1" 500
# hardcore carry-in: needs a singles ring (the CPU partners hijack the choreography) - temp profile
cat > profiles/_wpndemo.json <<'J'
{ "name": "_wpndemo", "description": "demo: hardcore weapons in a singles match", "mods": ["hardcore", "one-on-one", "exitring"], "category": "demo", "mode": "tag" }
J
clip weapons "hardcore" "climb out, take the chair, carry it INTO the ring|one chair shot" _wpndemo "WF_MODRULES=exit_ring_climb=1,cpu_energy_meters=1 WF_X1=0x310 WF_Y1=0x122 WF_P1=0-50:1,160-230:2,360-435:5,450-452:10,520-720:2,730-820:4,855-902:1,915-917:10,960-1000:1" 1080
rm -f profiles/_wpndemo.json
clip throw-out "throw_out" "a bodyslam near the ropes throws the victim OVER the top" "" "WF_W1=2 WF_THROW=0x24 WF_MODRULES=throw_out=1 WF_P1=$S" 400
clip ref-ko "ref_knockdown" "a strike floors the referee: no counts until he is up" ref-ko "WF_X1=0x240 WF_Y1=0x198 WF_P1=2-3:10,14-15:10,26-27:10" 420
clip parasitic "parasitic_pct" "P1 starts at half energy - watch his bar after the slam" "" "WF_HP1=50 WF_THROW=0x24 WF_MODRULES=parasitic_pct=100,cpu_energy_meters=1 WF_P1=$S" 400
clip heavyhits "human_hit_mult" "a human's slam does 3x - the CPU's bar after one throw" heavyhits "WF_THROW=0x24 WF_MODRULES=cpu_energy_meters=1 WF_P1=$S" 400
clip ko-dq "ko_dq" "energy 0 = OUT: x9 damage, one slam, his team loses" "" "WF_THROW=0x24 WF_MODRULES=human_hit_mult=9,ko_dq=1,cpu_energy_meters=1 WF_P1=$S" 500
split turbo "turbo" "the same inputs, stock (left) vs turbo 5 (right)" "WF_X1=0x1C0 WF_Y1=0x150 WF_P1=0-40:1,44-45:10,70-71:10,100-101:10,130-131:10" "WF_MODRULES=turbo=5 WF_X1=0x1C0 WF_Y1=0x150 WF_P1=0-40:1,44-45:10,70-71:10,100-101:10,130-131:10" 260
clip battle-royale-12 "battle-royale-12" "all twelve at the bell - nobody walks in" battle-royale-12 "WF_RUMBLE=1 WF_CPU2=1" 1800
clip walkin-palette "8-man rumble entrants" "a man walks in after an elimination|(his palette bank reclaimed - the corrupt-walk-in fix)" battle-royale "WF_RUMBLE=1 WF_CPU2=1" 2400 1500
ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i "$LIST" -c copy "$OUT/highlights.mp4" && echo "highlight: $OUT/highlights.mp4"
rm -rf "$T"
