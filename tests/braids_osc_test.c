/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* The C port of the Braids macro oscillator (firmware/src/braids_osc.c) against Mutable Instruments' original, sample
 * for sample: all 48 models, two pitches, four parameter / strike / sync scenarios (384 renders of 400 blocks of 24
 * samples). The golden is a hash of the ORIGINAL's output (tests/braids_osc_golden.txt, written by
 * tools/braids/verify.sh from a clone of the original: it also compares the two byte for byte). The port is built with
 * the original's delay line sizes (BR_LINES_SHIFT 0) and no pitch offset: at 96 kHz it is the original.
 *   build/host/braids_osc_test tests/braids_osc_golden.txt */
#define BR_LINES_SHIFT 0
#define BR_DIGMOD_SMOOTH 0
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../firmware/src/braids_tab.c"
#include "../firmware/src/braids_osc.c"
static br_lines_t lines;
static macosc_t osc;
static uint64_t render_hash(int shape, int pitch, int sc)
{
    uint64_t h = 0xcbf29ce484222325ull;
    int i;
    uint32_t k;
    memset(&osc, 0, sizeof osc);
    memset(&lines, 0, sizeof lines);
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
        for (k = 0; k < 48u; k++) {
            h ^= ((const uint8_t *)buf)[k];
            h *= 0x100000001b3ull;
        }
    }
    return h;
}
int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "tests/braids_osc_golden.txt", "r");
    int shape, pitch, sc, n = 0, bad = 0;
    unsigned long long want;
    if (!f) {
        printf("braids_osc: no golden file\n");
        return 2;
    }
    while (fscanf(f, "%d %d %d %llx", &shape, &pitch, &sc, &want) == 4) {
        uint64_t got = render_hash(shape, pitch, sc);
        n++;
        if (got != want) {
            printf("braids_osc: model %d pitch %d scenario %d differs from the original\n", shape, pitch, sc);
            bad++;
        }
    }
    fclose(f);
    printf("braids_osc: %d renders against the original's hashes, %d differ: %s\n", n, bad, n == 384 && !bad ? "ok" : "FAIL");
    return !(n == 384 && !bad);
}
