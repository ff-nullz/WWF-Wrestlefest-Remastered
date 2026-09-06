#!/bin/sh
# Video of the mods in action, headless: every frame dumped by the engine (WF_SHOT_EVERY=1),
# a 2 s title card, H.264 via ffmpeg. No audio (the sound board is not captured headless).
#   tools/video.sh            -> docs/video/<name>.mp4 per mod + docs/video/mods-reel.mp4 (all, in order)
#   tools/video.sh NAME       -> just that clip
cd "$(dirname "$0")/.." || exit 2
OUT=docs/video; mkdir -p "$OUT"; T=$(mktemp -d); FPS=57.44; ONLY=$1; LIST="$T/list.txt"; : > "$LIST"
export WF_MODRULES=cpu_energy_meters=1   # every clip shows the energy gauges (README GIFs: "the HUD must be visible")
clip() { # name "title" "subtitle" profile "env" frames
  n=$1; ti=$2; sub=$3; p=$4; e=$5; fr=$6
  [ -n "$ONLY" ] && [ "$ONLY" != "$n" ] && return 0
  rm -rf "$T/$n"; mkdir -p "$T/$n"
  env $e WF_SHOT_EVERY=1 WF_SHOT_DIR="$T/$n" ./wfengine --headless ${p:+--profile "$p"} --frames $fr --drive script >/dev/null 2>&1
  python3 - "$T/$n/title.png" "$ti" "$sub" <<'PY'
import sys
from PIL import Image, ImageDraw, ImageFont
out, ti, sub = sys.argv[1:4]
im = Image.new('RGB', (640, 480), (12, 12, 20)); d = ImageDraw.Draw(im)
f1 = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 40)
f2 = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 22)
w = d.textlength(ti, font=f1); d.text(((640 - w) / 2, 180), ti, font=f1, fill=(255, 210, 60))
y = 240
for line in sub.split('|'):
    w = d.textlength(line, font=f2); d.text(((640 - w) / 2, y), line, font=f2, fill=(220, 220, 230)); y += 30
im.save(out)
PY
  ffmpeg -hide_banner -loglevel error -y -loop 1 -framerate $FPS -t 2 -i "$T/$n/title.png" -c:v libx264 -crf 22 -pix_fmt yuv420p -r $FPS "$T/$n/title.mp4"
  ffmpeg -hide_banner -loglevel error -y -framerate $FPS -i "$T/$n/f%06d.ppm" -vf "scale=640:480:flags=neighbor" -c:v libx264 -crf 22 -pix_fmt yuv420p "$T/$n/game.mp4"
  printf "file '%s/title.mp4'\nfile '%s/game.mp4'\n" "$T/$n" "$T/$n" > "$T/$n/cat.txt"
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i "$T/$n/cat.txt" -c copy "$OUT/$n.mp4"
  printf "file '%s'\n" "$(pwd)/$OUT/$n.mp4" >> "$LIST"
  rm -rf "$T/$n"; echo "video: $OUT/$n.mp4 ($fr frames)"
}
S="0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10,88-89:10,94-95:10,100-101:10,110-111:10"
clip stock         "WWF WrestleFest Remastered" "the stock game, CPU vs CPU|native engine, no ROM code running" ""             "WF_CPU1=1 WF_CPU2=1"  1200
clip heavyhits     "heavyhits"      "a human's hits, knees and throws do 3x damage"                heavyhits     "WF_THROW=0x24 WF_P1=$S WF_CPU2=1" 900
clip parasitic     "parasitic"      "every hit feeds the hitter what it takes|energy bars: watch the attacker refill" parasitic "WF_HP1=50 WF_CPU1=1 WF_CPU2=1" 1200
clip turbo         "turbo"          "wrestlers at 160%, clock and referee at real time"            turbo         "WF_CPU1=1 WF_CPU2=1"  900
clip vampire-turbo "vampire-turbo"  "parasitic + turbo stacked in one profile"                     vampire-turbo "WF_HP1=50 WF_CPU1=1 WF_CPU2=1" 1200
clip battle-royale-12 "battle-royale-12" "all twelve in the ring at the bell|no walk-ins, last man standing" battle-royale-12 "WF_RUMBLE=1 WF_CPU2=1" 1800
clip battle-royale "battle-royale"  "8-man bell, fast entrances (rumble)"                          battle-royale "WF_RUMBLE=1 WF_CPU2=1" 1500
clip cagematch     "cagematch"      "hold DOWN into the bottom wall ~2 s|climb hand over hand, over the top, drop, WIN" cagematch "WF_STAGE=2 WF_X1=0x280 WF_Y1=0x120 WF_P1=0-140:8" 520
clip exit-ring     "exit-ring"      "hold into a rope for half a second to hop out"                exit-ring     "WF_X1=0x2E0 WF_Y1=0x190 WF_P1=0-200:1" 400
clip chaos         "chaos"          "random run-ins every few seconds + exit at will|two mods stacked" chaos     "WF_CPU2=1"            1800
clip run-ins       "run-ins"        "a wrestler who is not in the match runs in and brawls"        run-ins       "WF_CPU2=1"            1800
clip ref-ko        "ref-ko"         "strikes floor the referee, no counts until he is up"          ref-ko        "WF_X1=0x240 WF_Y1=0x198 WF_P1=2-3:10,14-15:10,26-27:10" 400
clip hardcore      "hardcore"       "weapons come into the ring, no count-outs"                    hardcore      "WF_CPU1=1 WF_CPU2=1"  1500
clip survivor      "survivor"       "3-on-3 with elimination rules"                                survivor      "WF_CPU1=1 WF_CPU2=1"  1200
clip handicap      "handicap"       "you alone against a tag team"                                 handicap      "WF_CPU1=1 WF_CPU2=1"  1000
clip one-on-one    "one-on-one"     "singles: no partners"                                         one-on-one    "WF_CPU1=1 WF_CPU2=1"  1000
clip mirror        "mirror"         "the same wrestler twice, the second in an alternate outfit"   mirror        "WF_W1=0 WF_CPU1=1 WF_CPU2=1" 900
clip precision     "precision"      "deterministic throws: direction + button = the same move"     precision     "WF_THROW=0x24 WF_P1=$S WF_CPU2=1" 700
clip meters        "meters"         "CPU energy meters on the HUD"                                 meters        "WF_CPU1=1 WF_CPU2=1"  700
clip gauge         "gauge"          "the grapple gauge over a grappling pair"                      gauge         "WF_CPU1=1 WF_CPU2=1"  900
clip widescreen    "widescreen"     "zoomed-out panning camera, a third more ring"                 widescreen    "WF_CPU1=1 WF_CPU2=1"  900
clip fixedview     "fixedview"      "a locked full-ring view, the camera never pans"               fixedview     "WF_CPU1=1 WF_CPU2=1"  900
clip trainer       "trainer"        "unlimited energy for everyone, no clock"                      trainer       "WF_CPU1=1 WF_CPU2=1"  700
clip legends       "legends"        "the Legion of Doom join the select grid"                      legends       "WF_W1=4 WF_CPU1=1 WF_CPU2=1" 900
clip clones        "clones"         "a clone slot: HOLLYWOOD, a re-palette Hogan"                  clones        "WF_CLONE=12 WF_CPU1=1 WF_CPU2=1" 900
clip skin-undertaker "skin: Undertaker" "an AI-drawn skin on a Warrior-class body"                superstars    "WF_W1=13 WF_CPU1=1 WF_CPU2=1" 900
clip skin-duggan   "skin: Duggan"   "an AI-drawn skin on a Hogan-class body"                       wardrobe      "WF_W1=12 WF_CPU1=1 WF_CPU2=1" 900
if [ -z "$ONLY" ]; then ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i "$LIST" -c copy "$OUT/mods-reel.mp4" && echo "video: $OUT/mods-reel.mp4"; fi
rm -rf "$T"
