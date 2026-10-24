# tests/tr808_test.c: where the reference numbers come from

`TR8_REF` in `tr808_test.c` holds what the 8W8 module (<https://github.com/athousanddetails/schwung-8W8>, GPL-3.0,
commit `6fa3f70d52c76f17d816581c668e3ab65b2cd395`, 2026-10-11) renders for each of its 16 voices at its default pots and
velocity 100: the peak in dBFS, the level (rms of 5 ms) at 2.5, 7.5, 17.5, 37.5, 77.5, 157.5, 317.5 and 637.5 ms, and the
strongest frequency from 20 to 250 ms. The module's volume (100 / 127) is in them: this engine's master scale is the same.

To make them again:

```sh
git clone https://github.com/athousanddetails/schwung-8W8 && cd schwung-8W8/src
# tools/refdump.cpp (below) next to tools/render.cpp
g++ -O2 -std=c++17 -Idsp -o refdump tools/refdump.cpp dsp/w8_engine.cpp
./refdump bd 100 1.3 > bd.f32          # raw float32, mono, 44.1 kHz; key=value pots after the seconds (bd_decay=40 ...)
```

```python
import numpy as np
x = np.fromfile("bd.f32", dtype=np.float32).astype(float)
peak_db = 20 * np.log10(abs(x).max())
def win(t):                                   # rms of the 5 ms around t (s), dB
    n = int(44100 * 0.005); a = int(t * 44100) - n // 2
    return 20 * np.log10(np.sqrt(np.mean(x[a:a + n] ** 2)) + 1e-9)
seg = x[int(0.02 * 44100):int(0.25 * 44100)]
f = np.fft.rfftfreq(32768, 1 / 44100)[np.argmax(abs(np.fft.rfft(seg * np.hanning(len(seg)), 32768)))]
```

The noise voices (SD, MA, CP, RS, the hats, the cymbal, the cowbell's bleed) draw other noise than 8W8's generator, so only their
levels are compared, in windows of 5 ms; the tuned voices (BD, SD, the toms and congas, CL) are compared in pitch too. The same
script over other pots (TUNE, DECY, velocity) agreed to the same extent when the engine was written (mean level difference
under 2 dB, peaks within 2.7 dB); `tests/tr808_test.c` keeps the default settings.

`tools/refdump.cpp` (8W8's tree):

```cpp
// refdump <voice> <vel> <seconds> [key=val ...] > raw float32 mono
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w8_engine.h"
int main(int argc, char **argv)
{
    int vi = -1; const char *name = argv[1];
    for (int v = 0; v < W8_NUM_VOICES; ++v) if (!strcmp(w8_voice_id(v), name)) vi = v;
    if (vi < 0) return 1;
    int vel = atoi(argv[2]); double sec = atof(argv[3]);
    w8_engine_t *e = w8_create(44100.0f);
    for (int i = 4; i < argc; ++i) { char k[64], val[64]; if (sscanf(argv[i], "%63[^=]=%63s", k, val) == 2) w8_set_param(e, k, val); }
    int n = (int)(sec * 44100.0);
    float *buf = (float *)calloc(n, sizeof(float));
    w8_trigger(e, vi, vel);
    w8_render(e, buf, n);
    fwrite(buf, sizeof(float), n, stdout);
    return 0;
}
```
