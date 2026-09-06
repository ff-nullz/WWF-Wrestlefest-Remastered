#!/bin/sh
# README media: a still + an animated GIF per profile / scene, headless.
#   tools/media.sh            -> docs/img/game/<name>.png + .gif
# Each entry: name | profile (or -) | extra env | frames | shot-every | still-frame
cd "$(dirname "$0")/.." || exit 2
OUT=docs/img/game; mkdir -p "$OUT"; T=$(mktemp -d)
one() { # name profile env frames every still
  n=$1; p=$2; e=$3; fr=$4; ev=$5; st=$6
  rm -rf "$T/$n"; mkdir -p "$T/$n"
  env $e WF_SHOT_EVERY=$ev WF_SHOT_DIR="$T/$n" ./wfengine --headless ${p:+--profile "$p"} --frames $fr --drive script >/dev/null 2>&1
  python3 - "$T/$n" "$OUT/$n" "$st" <<'PY'
import sys, glob, os
from PIL import Image
d, out, still = sys.argv[1], sys.argv[2], int(sys.argv[3])
fs = sorted(glob.glob(d + '/f*.ppm'))
if not fs: sys.exit("no frames for " + out)
ims = [Image.open(f).convert('RGB') for f in fs]
pick = min(range(len(fs)), key=lambda i: abs(int(os.path.basename(fs[i])[1:7]) - still))
ims[pick].resize((640, 480), Image.NEAREST).save(out + '.png')
seq = [im.resize((320, 240), Image.NEAREST).quantize(colors=128) for im in ims]
seq[0].save(out + '.gif', save_all=True, append_images=seq[1:], duration=100, loop=0, optimize=True)
print(out, len(seq), 'frames')
PY
}
one stock        ""              "WF_CPU2=1"                       900 6 600
one heavyhits    heavyhits       "WF_CPU2=1"                       900 6 600
one vampire-turbo vampire-turbo  "WF_CPU1=1 WF_CPU2=1"             900 6 600
one battle-royale-12 battle-royale-12 "WF_RUMBLE=1 WF_CPU2=1"      1200 6 700
one cagematch    cagematch       "WF_STAGE=2 WF_X1=0x280 WF_Y1=0x120 WF_P1=0-140:8" 420 4 200
one exit-ring    exit-ring       "WF_X1=0x2E0 WF_Y1=0x190 WF_P1=0-200:1" 320 4 120
one chaos        chaos           "WF_CPU2=1"                       1500 6 1000
one hardcore     hardcore        "WF_CPU1=1 WF_CPU2=1"             1200 6 800
one survivor     survivor        "WF_CPU1=1 WF_CPU2=1"             900 6 600
one widescreen   widescreen      "WF_CPU2=1"                       600 6 400
one fixedview    fixedview       "WF_CPU2=1"                       600 6 400
one ref-ko       ref-ko          "WF_X1=0x240 WF_Y1=0x198 WF_P1=2-3:10,14-15:10,26-27:10" 160 3 60
one skin-undertaker superstars   "WF_W1=13 WF_CPU2=1"              700 6 400
one skin-duggan  wardrobe        "WF_W1=12 WF_CPU2=1"              700 6 400
one legends      legends         "WF_W1=4 WF_CPU2=1"               700 6 400
rm -rf "$T"
