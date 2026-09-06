#!/bin/sh
# Headless check for the pin run-in (docs/engine-specs/pin-partner.md).
#   tools/pin_partner_check.sh [three|kick|stomp]
# Prints one line per interesting event so the whole pin can be eyeballed.
set -e
cd "$(dirname "$0")/.."
MODE=${1:-three}
echo "=== --drive pin  WF_PIN=$MODE ==="
WF_PIN=$MODE WF_DBGSEL=1 WF_TRACE=0xF ./wfengine --headless --frames 1400 --drive pin 2>&1 |
awk '
/^cover:|^tag:|^pin:|^ai: o[0-9] (rescue armed|run-in|fire)|^hit:|UNROUTED|stuck/ { print; next }
/^tr f/ {
    f=$2; o=$3;
    st=""; mv=""; ap=""; in_=""; rt=""; pin=""; f35=""; e6="";
    for (i=1;i<=NF;i++) {
        if ($i ~ /^st=/)  st=$i;
        if ($i ~ /^mv=/)  mv=$i;
        if ($i ~ /^ap=/)  ap=$i;
        if ($i ~ /^in=/)  in_=$i;
        if ($i ~ /^rt=/)  rt=$i;
        if ($i ~ /^pin=/) pin=$i;
        if ($i ~ /^f35=/) f35=$i;
        if ($i ~ /^e6=/)  e6=$i;
    }
    key = st" "mv" "ap" "in_" "pin;              # ignore the ticking timers
    if (key != prev[o]) { printf "%s %s %s %s %s %s %s %s %s\n", f,o,st,mv,ap,in_,pin,rt,f35; prev[o]=key }
}'
