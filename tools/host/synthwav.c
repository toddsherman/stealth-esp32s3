// Renders the real synth to a WAV so the audio can be checked without a
// speaker in hand. Sweeps tension 0 -> 1 so the heartbeat's ramp is audible,
// then plays each one-shot effect in turn.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "synth.h"

#define RATE   22050
#define FRAMES 256

static void put32(FILE *f, unsigned v) { fputc(v&255,f); fputc((v>>8)&255,f); fputc((v>>16)&255,f); fputc((v>>24)&255,f); }
static void put16(FILE *f, unsigned v) { fputc(v&255,f); fputc((v>>8)&255,f); }

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "synth.wav";
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }

    fwrite("RIFF", 1, 4, f); put32(f, 0); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, RATE); put32(f, RATE * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, 0);
    const long data_start = ftell(f);

    synth_init(RATE);
    int16_t buf[FRAMES];
    long total = 0;

    // 1) Heartbeat: tension ramps 0 -> 1 over 10 seconds.
    const int ramp_blocks = (10 * RATE) / FRAMES;
    for (int i = 0; i < ramp_blocks; i++) {
        synth_set_tension((float)i / (float)ramp_blocks);
        synth_render(buf, FRAMES);
        fwrite(buf, 2, FRAMES, f); total += FRAMES;
    }

    // 2) Silence, then each effect with a gap.
    synth_set_tension(0.0f);
    const sfx_t order[] = { SFX_SPOTTED, SFX_BOMB_THROW, SFX_BOMB_BURST,
                            SFX_RESCUE, SFX_CAUGHT, SFX_CLEAR, SFX_ARM };
    for (unsigned k = 0; k < sizeof(order)/sizeof(order[0]); k++) {
        synth_sfx(order[k]);
        const int blocks = (int)(1.1f * RATE) / FRAMES;
        for (int i = 0; i < blocks; i++) {
            synth_render(buf, FRAMES);
            fwrite(buf, 2, FRAMES, f); total += FRAMES;
        }
    }

    const long bytes = total * 2;
    fseek(f, 4, SEEK_SET);            put32(f, (unsigned)(36 + bytes));
    fseek(f, data_start - 4, SEEK_SET); put32(f, (unsigned)bytes);
    fclose(f);

    printf("wrote %s: %.1f s, %ld samples @ %d Hz\n",
           out, (double)total / RATE, total, RATE);
    return 0;
}
