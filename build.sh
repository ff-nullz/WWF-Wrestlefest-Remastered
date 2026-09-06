#!/bin/bash
# build.sh — bump the version in src/version.h, then build wfengine.
#
#   ./build.sh            bump the MINOR version + make   (V1.07 -> V1.08)
#   ./build.sh --major    bump the MAJOR version + make   (V1.xx -> V2.00)
#   ./build.sh --no-bump  make only (same as plain `make`)
#
# `make` on its own never bumps, so `make regress` / `make baseline` keep the
# number stable. The version shows up on the CLI at start-up, in the window
# title and in the on-screen "Vmajor.minor" label (video.c).
set -e
cd "$(dirname "$0")"
VH=src/version.h

if [ "$1" != "--no-bump" ]; then
    maj=$(sed -n 's/^#define WF_VERSION_MAJOR \([0-9]*\)$/\1/p' "$VH")
    min=$(sed -n 's/^#define WF_VERSION_MINOR \([0-9]*\)$/\1/p' "$VH")
    [ -n "$maj" ] && [ -n "$min" ] || { echo "build.sh: cannot read WF_VERSION_MAJOR/MINOR from $VH" >&2; exit 1; }
    if [ "$1" = "--major" ]; then maj=$((maj + 1)); min=0; else min=$((min + 1)); fi
    str=$(printf '%d.%02d' "$maj" "$min")
    cat > "$VH" <<EOF
#ifndef WF_VERSION_H
#define WF_VERSION_H
/* major.minor (user 2026-08-29): build.sh bumps MINOR on every build,
 * \`./build.sh --major\` bumps MAJOR (minor back to 0) when we decide to.
 * WF_VERSION stays a comparable number = major * 1000 + minor. */
#define WF_VERSION_MAJOR $maj
#define WF_VERSION_MINOR $min
#define WF_VERSION (WF_VERSION_MAJOR * 1000 + WF_VERSION_MINOR)
#define WF_VERSION_STRING "$str"
#endif
EOF
    echo "build.sh: version -> V$str"
fi

make
# Regenerate the runtime paks from the data tree (docs/adr-001-data-formats.md).
# They are a cache (build/ is git-ignored); the game falls back to the ROM
# copy while tables are still missing from build/base.pak.
make -s pack
echo "build.sh: built wfengine V$(sed -n 's/^#define WF_VERSION_STRING "\(.*\)"$/\1/p' "$VH")"
