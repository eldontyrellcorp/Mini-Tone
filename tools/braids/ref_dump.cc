// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Stephane Schmitz
// The reference renderer for tests/braids_osc_test.c: renders a model of the ORIGINAL Braids (Mutable Instruments, MIT)
// the way the test renders the C port. tools/braids/verify.sh builds it against a clone of github.com/pichenettes/eurorack.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "braids/macro_oscillator.h"
using namespace braids;
// usage: dump shape pitch scenario out.bin
int main(int argc, char** argv) {
  int shape = atoi(argv[1]); int pitch = atoi(argv[2]); int sc = atoi(argv[3]);
  static MacroOscillator osc; osc.Init(); osc.set_shape((MacroOscillatorShape)shape);
  osc.set_pitch(pitch);
  FILE* f = fopen(argv[4], "wb");
  int blocks = 400;
  for (int i = 0; i < blocks; ++i) {
    int16_t buf[24]; uint8_t sync[24]; memset(sync, 0, 24);
    int16_t a, b;
    if (sc == 0) { a = (i * 80) & 0x7fff; b = (i * 37 * 3) & 0x7fff; }
    else if (sc == 1) { a = 0; b = 16384; }
    else if (sc == 2) { a = 32767; b = (i * 131) & 0x7fff; }
    else { a = 16384 + ((i * 17) & 0x3fff); b = 32767 - ((i * 53) & 0x7fff); }
    osc.set_parameters(a, b);
    if (sc == 3 && (i % 100) == 0) osc.Strike();
    if (sc == 3 && (i % 7) == 0) sync[3] = 1;
    osc.set_pitch(pitch + (sc == 2 ? (i % 64) : 0));
    osc.Render(sync, buf, 24);
    fwrite(buf, 2, 24, f);
  }
  fclose(f);
}
