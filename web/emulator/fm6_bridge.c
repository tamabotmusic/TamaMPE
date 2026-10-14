/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse the existing host harness and its firmware sources, not a DSP port.
 * Hardware section attributes are already removed by hostsim.c. */
#define main unused_hostsim_main
#include "../../tests/hostsim.c"
#undef main

static float emulator_pcm[CTL];

void emulator_init(void)
{
    dsp_tables_init();
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    eng_state_reset();
    fm6_fn_reset();
    host_tracks_init();
    host_preset(&trk[0], ENGI_FM6, 0); /* Existing TINE EP DX7 factory preset. */
    trk[0].p[P_VOICE] = V_POLY;
}

void emulator_note_on(int note)
{
    if (note >= 0 && note < 128) trk_note_on(&trk[0], (uint32_t)note, 100);
}

void emulator_note_off(int note)
{
    if (note >= 0 && note < 128) trk_note_off(&trk[0], (uint32_t)note);
}

float *emulator_render(void)
{
    int32_t pcm[CTL];
    mod_begin(&trk[0]);
    track_render(&trk[0], pcm, CTL);
    mod_end(&trk[0]);
    for (uint32_t i = 0; i < CTL; i++)
        /* Fixed audition headroom; no alteration of the engine or patch. */
        emulator_pcm[i] = (float)pcm[i] / 262144.0f;
    return emulator_pcm;
}

int emulator_rate(void) { return FS; }
int emulator_block(void) { return CTL; }

#ifdef EMULATOR_REFERENCE
int main(void)
{
    emulator_init();
    for (uint32_t b = 0; b < 4096; b++) {
        if (b == 4) emulator_note_on(60);
        if (b == 512) emulator_note_on(64);
        if (b == 1024) emulator_note_off(60);
        if (b == 1536) emulator_note_off(64);
        float *out = emulator_render();
        if (fwrite(out, sizeof *out, CTL, stdout) != CTL) return 1;
    }
    return 0;
}
#endif
