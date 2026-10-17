#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Stephane Schmitz
# Checks firmware/src/braids_osc.c against Mutable Instruments' original Braids, sample for sample, and rewrites
# tests/braids_osc_golden.txt (the hashes tests/braids_osc_test.c compares with) from the original's output.
#   tools/braids/verify.sh [DIR]     DIR: a clone of https://github.com/pichenettes/eurorack and of
#                                    https://github.com/pichenettes/stmlib (as DIR/stmlib); default: cloned into a temp dir
# Needs g++ and network access (once).
set -e
cd "$(dirname "$0")/../.."
W=$(mktemp -d)
D=${1:-$W/eurorack}
if [ ! -d "$D/braids" ]; then
    git clone -q --depth 1 https://github.com/pichenettes/eurorack.git "$D"
    rm -rf "$D/stmlib"
    git clone -q --depth 1 https://github.com/pichenettes/stmlib.git "$D/stmlib"
fi
g++ -O1 -w -I"$D" -o "$W/ref" tools/braids/ref_dump.cc "$D"/braids/macro_oscillator.cc "$D"/braids/analog_oscillator.cc \
    "$D"/braids/digital_oscillator.cc "$D"/braids/resources.cc "$D"/stmlib/utils/random.cc
cc -O1 -w -DBR_LINES_SHIFT=0 -DBR_DIGMOD_SMOOTH=0 -Ifirmware/src -o "$W/port" tests/braids_osc_dump.c
bad=0
: > "$W/golden.txt"
for sh in $(seq 0 47); do for p in 6400 10000; do for sc in 0 1 2 3; do
    "$W/ref" $sh $p $sc "$W/r.bin"
    "$W/port" $sh $p $sc "$W/c.bin"
    cmp -s "$W/r.bin" "$W/c.bin" || { echo "DIFFERENT: model $sh pitch $p scenario $sc"; bad=1; }
    echo "$sh $p $sc $(python3 -I -c "
import sys
h=0xcbf29ce484222325
for b in open('$W/r.bin','rb').read():
    h=((h^b)*0x100000001b3)&0xffffffffffffffff
print('%016x'%h)")" >> "$W/golden.txt"
done; done; done
[ $bad = 0 ] && cp "$W/golden.txt" tests/braids_osc_golden.txt && echo "braids_osc.c == the original (384 renders); golden rewritten"
rm -rf "$W"
exit $bad
