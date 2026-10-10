#!/bin/bash
# Lock-step verification of recompiled code against the interpreter.
#   tools/verify.sh <disc.cue> <song> <frames> [extra headless args, e.g. --fuzz 3 --fuzz-from 150]
# Both backends run in "sync mode" (hardware events only at function entries / backward branches) and log
# (pc, cycle, register hash) at every safe point; the logs must be identical. VT_STRIDE=N keeps only every
# Nth record (rolling hash over the full history) so long runs stay small.
set -e
cd "$(dirname "$0")/.."
DISC=$1; SONG=$2; FRAMES=$3; shift 3
T=$(mktemp -d)
export VT_STRIDE=${VT_STRIDE:-8192}
build/vibrecomp-headless --disc "$DISC" --autoplay "$SONG" --frames "$FRAMES" --interp --sync --vtrace "$T/a.bin" 4000000000 "$@" 2>&1 | tail -1
build/vibrecomp-headless --disc "$DISC" --autoplay "$SONG" --frames "$FRAMES" --sync --vtrace "$T/b.bin" 4000000000 "$@" 2>&1 | tail -1
python3 tools/vtcmp.py "$T/a.bin" "$T/b.bin"
rm -rf "$T"
