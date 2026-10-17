/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* Renders one model of the C port of the Braids macro oscillator (firmware/src/braids_osc.c) as tools/braids/ref_dump.cc
 * renders the original's: braids_osc_dump SHAPE PITCH SCENARIO OUT.bin (400 blocks of 24 samples, int16) */
#define BR_DIGMOD_SMOOTH 0
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../firmware/src/braids_tab.c"
#include "../firmware/src/braids_osc.c"
static br_lines_t lines;
static macosc_t osc;
int main(int argc, char **argv)
{
    int shape, pitch, sc, i;
    FILE *f;
    if (argc < 5)
        return 2;
    shape = atoi(argv[1]), pitch = atoi(argv[2]), sc = atoi(argv[3]);
    f = fopen(argv[4], "wb");
    osc.digital.rng = 0x21;
    osc.digital.dl = &lines;
    mac_init(&osc);
    mac_set_shape(&osc, shape);
    for (i = 0; i < 400; i++) {
        int16_t buf[24], a, b;
        uint8_t sync[24];
        memset(sync, 0, 24);
        if (sc == 0) { a = (i * 80) & 0x7fff; b = (i * 37 * 3) & 0x7fff; }
        else if (sc == 1) { a = 0; b = 16384; }
        else if (sc == 2) { a = 32767; b = (i * 131) & 0x7fff; }
        else { a = 16384 + ((i * 17) & 0x3fff); b = 32767 - ((i * 53) & 0x7fff); }
        osc.parameter[0] = a;
        osc.parameter[1] = b;
        if (sc == 3 && (i % 100) == 0)
            mac_strike(&osc);
        if (sc == 3 && (i % 7) == 0)
            sync[3] = 1;
        osc.pitch = pitch + (sc == 2 ? (i % 64) : 0);
        mac_render(&osc, sync, buf, 24);
        fwrite(buf, 2, 24, f);
    }
    fclose(f);
    return 0;
}
