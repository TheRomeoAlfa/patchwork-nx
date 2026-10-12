#!/bin/bash
# Run the desktop build with a scripted button sequence and collect screenshots + log.
#   tools/qa-run.sh <name> <fake-sd-dir> "<script>" [ENV=value ...]
# Script steps (see source/app/debug_script.hpp): w<ms> wait, t<x>,<y> tap, k<button> press, s<name> screenshot, q quit.
# Output goes to qa-out/<name>/; the summary line flags crashes and logged errors.
name=$1; sd=$2; script=$3; shift 3
repo=$(cd "$(dirname "$0")/.." && pwd)
out=$repo/qa-out/$name; rm -rf "$out"; mkdir -p "$out" "$sd"
cd "$repo/build-desktop" || { echo "build the desktop app first (see README)"; exit 1; }
env MM_SHOT_DIR="$out" MM_DEBUG_SCRIPT="$script" MM_SD_ROOT="$sd" MM_TOTK_VERSION=1.2.1 MM_BOTW_VERSION=1.6.0 "$@" \
    timeout 300 ./patchwork > "$out/log.txt" 2>&1
code=$?
sed -i '' 's/\x1b\[[0-9;]*m//g' "$out/log.txt" 2>/dev/null || sed -i 's/\x1b\[[0-9;]*m//g' "$out/log.txt"
echo "== $name: exit=$code screenshots=$(ls "$out"/*.png 2>/dev/null | wc -l | tr -d ' ')"
grep -E "terminat|Assertion|Segmentation|AddressSanitizer" "$out/log.txt" | head -3 | sed 's/^/   CRASH: /'
