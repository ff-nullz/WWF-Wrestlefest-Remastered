#!/bin/sh
# codex = the ONLY art backend (tools/art_run.c art_providers[]; qwen-edit
# and grok were tried and dropped 2026-08-29). This checks the machine can
# serve it: the codex CLI on PATH, a login, and one real image round trip
# through the exact call art_run.c makes (codex exec -i A -i B -- prompt,
# result = the newest file under ~/.codex/generated_images).
#
#   tools/codex_setup.sh          check + install/login what is missing
#   tools/codex_setup.sh --smoke  ... and run one image generation (~$0.05)
#
# Exit 0 = ready. Nothing here touches the repo's data.
set -u
SMOKE=0; [ "${1-}" = "--smoke" ] && SMOKE=1
ok()   { printf '  ok   %s\n' "$*"; }
miss() { printf '  MISS %s\n' "$*"; }

echo "codex setup:"
if ! command -v node >/dev/null 2>&1; then
    miss "node not on PATH - install Node.js 18+ (apt install nodejs npm, or nvm) and re-run"
    exit 1
fi
ok "node $(node --version)"

if ! command -v codex >/dev/null 2>&1; then
    miss "codex CLI not on PATH - installing @openai/codex with npm (user-local)"
    npm install -g @openai/codex || { miss "npm install failed"; exit 1; }
    command -v codex >/dev/null 2>&1 || { miss "codex still not on PATH - add npm's global bin dir (npm prefix -g)/bin to PATH"; exit 1; }
fi
ok "$(codex --version 2>/dev/null || echo codex) at $(command -v codex)"

AUTH="${CODEX_HOME:-$HOME/.codex}/auth.json"
if [ ! -s "$AUTH" ]; then
    miss "no login ($AUTH) - running 'codex login' (a browser window opens)"
    codex login || { miss "login failed"; exit 1; }
    [ -s "$AUTH" ] || { miss "login left no $AUTH"; exit 1; }
fi
ok "login present ($AUTH)"

GEN="${CODEX_HOME:-$HOME/.codex}/generated_images"
mkdir -p "$GEN" 2>/dev/null
ok "generated_images dir $GEN"

# text round trip: proves the account answers at all (no image cost)
if ! codex exec --skip-git-repo-check -- "Reply with the single word READY and nothing else." 2>/dev/null | grep -q READY; then
    miss "'codex exec' gave no answer - expired login? try 'codex logout && codex login'"
    exit 1
fi
ok "codex exec answers"

[ $SMOKE = 1 ] || { echo "ready (add --smoke for a real image generation)"; exit 0; }

# image round trip with the pipeline's own shape: two input images, one PNG out
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
IMG="$TMP/in.png"
# 16x16 magenta PNG, written by the engine's own PNG path when available
if [ -x ./wfengine ] && [ -f data/generics/0/base_ref.png ]; then
    IMG=data/generics/0/base_ref.png
else
    printf 'P6 16 16 255 ' > "$TMP/in.ppm"; i=0
    while [ $i -lt 256 ]; do printf '\377\000\377' >> "$TMP/in.ppm"; i=$((i+1)); done
    if command -v convert >/dev/null 2>&1; then convert "$TMP/in.ppm" "$IMG"; else IMG="$TMP/in.ppm"; fi
fi
NEWEST_BEFORE=$(find "$GEN" -type f -name 'exec-*' -printf '%T@\n' 2>/dev/null | sort -n | tail -1)
echo "  ...  generating one test image (this is billed like a pipeline call)"
codex exec --skip-git-repo-check -i "$IMG" -i "$IMG" -- \
  "Generate an image: redraw the figure in the first image as a plain pixel-art sprite on a solid magenta (#FF00FF) background, same pose, same size. Save it as a PNG." >/dev/null 2>&1
NEWEST_AFTER=$(find "$GEN" -type f -name 'exec-*' -printf '%T@\n' 2>/dev/null | sort -n | tail -1)
if [ -n "$NEWEST_AFTER" ] && [ "${NEWEST_AFTER%%.*}" -gt "${NEWEST_BEFORE%%.*}" ] 2>/dev/null; then
    F=$(find "$GEN" -type f -name 'exec-*' -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2-)
    ok "image came back: $F"
    echo "ready"
else
    miss "no new file under $GEN - codex ran but produced no image (model/plan without image generation?)"
    exit 1
fi
