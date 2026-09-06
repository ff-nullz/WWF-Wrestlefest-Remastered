#!/bin/sh
# AI bench: the same CPU-vs-CPU drives with a rule setting, WF_AISTATS totals only.
#   tools/ai_bench.sh                 -> stock vs ai_commit=32
#   tools/ai_bench.sh "ai_commit=48"  -> stock vs that
cd "$(dirname "$0")/.." || exit 2
R=${1:-ai_commit=32}
one() { # label rules
  printf "%-16s tag    " "$1"; env WF_MODRULES="$2" WF_AISTATS=1 WF_CPU1=1 WF_CPU2=1 ./wfengine --headless --frames 6000 --drive script 2>&1 | grep '^ai-stats: ALL' | sed 's/.*per 1000/per 1000/'
  printf "%-16s rumble " "$1"; env WF_MODRULES="$2" WF_AISTATS=1 WF_RUMBLE=1 WF_CPU2=1 ./wfengine --headless --frames 6000 --drive script 2>&1 | grep '^ai-stats: ALL' | sed 's/.*per 1000/per 1000/'
}
one stock ""
one "$R" "$R"
