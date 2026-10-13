/* SPDX-License-Identifier: GPL-3.0-only */
#define MPE_NOTES_NO_MAIN 1
#include "mpe_notes_test.c"
#include <math.h>

static void bend(uint32_t ch, int32_t raw)
{
    uint32_t wire = (uint32_t)(raw + 8192);
    queued(0xE0u | ch, wire & 127u, wire >> 7, 1);
}
static void sensitivity(uint32_t ch, uint32_t semis, uint32_t cents)
{
    queued(0xB0u | ch, 101, 0, 1); queued(0xB0u | ch, 100, 0, 1);
    queued(0xB0u | ch, 6, semis, 1); queued(0xB0u | ch, 38, cents, 1);
}
static fm6_voice_t *dsp(voice_t *v) { return fm6_state(&trk[0], v); }
static void settle(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        int32_t out[CTL]; mod_begin(&trk[0]); track_render(&trk[0], out, CTL); mod_end(&trk[0]);
    }
}
/* Render one already-planned voice through the real kernel, without other
 * voices/release tails. INIT is a single-carrier sine, measured by crossings. */
static double measured_hz(voice_t *v)
{
    fm6_voice_t saved = *dsp(v);
    fm6_voice_t *s = dsp(v);
    int crossings = 0, samples = 44100, previous = 0;
    for (int at = 0; at < samples; at += CTL) {
        uint32_t n = (uint32_t)(samples - at); if (n > CTL) n = CTL;
        fm6_run(s, n);
        for (uint32_t i = 0; i < n; i++) {
            int positive = fm6_sum[i] > 0;
            crossings += !previous && positive;
            previous = positive;
        }
    }
    *s = saved;
    return crossings; /* exactly one second */
}
int main(void)
{
    setup();
    /* Preserve a known conventional RPN and the DX7 patch/function settings. */
    uint8_t patch[sizeof fm6_patch], functions[sizeof fm6_fn];
    memcpy(patch, fm6_patch, sizeof patch); memcpy(functions, fm6_fn, sizeof functions);
    assert(mpe_bend_ch[0].semis == 2 && mpe_bend_ch[1].semis == 48);
    queued(0x91, 60, 100, 1); queued(0x92, 60, 100, 1);
    voice_t *a = sounding(owner(1, 60)), *b = sounding(owner(2, 60));
    settle(); int32_t centre = dsp(a)->fq[0];
    bend(1, 8191); settle();
    assert(a->mpe_bend_q24 == 4 * (1 << 24) && b->mpe_bend_q24 == 0);
    assert(abs(dsp(a)->fq[0] / 16 - centre) < 8 && dsp(b)->fq[0] == centre);
    bend(2, -8192); settle();
    assert(b->mpe_bend_q24 == -4 * (1 << 24));
    assert(abs(dsp(b)->fq[0] * 16 - centre) < 32);
    double hz_a = measured_hz(a), hz_b = measured_hz(b);
    printf("isolated C4 +/-48: %.0f / %.0f Hz (expected ~4186 / ~16)\n", hz_a, hz_b);
    assert(fabs(hz_a - 4186.01) < 3 && fabs(hz_b - 16.35) < 2);
    bend(1, 0); bend(2, 0); settle();
    assert(!a->mpe_bend_q24 && !b->mpe_bend_q24);
    assert(dsp(a)->fq[0] == centre && dsp(b)->fq[0] == centre);
    puts("member +/-48 endpoints, independent audio/frequency and exact centre OK");

    sensitivity(1, 12, 50); bend(1, 8191); settle();
    int32_t expected = (int32_t)((int64_t)1250 * (1 << 24) / 1200);
    assert(a->mpe_bend_q24 == expected && !b->mpe_bend_q24);
    assert(mpe_bend_ch[2].semis == 48 && mpe_bend_ch[0].semis == 2);
    sensitivity(0, 3, 0); bend(0, -8192); settle();
    assert(a->mpe_bend_q24 == expected - (1 << 22) && b->mpe_bend_q24 == -(1 << 22));
    assert(!trk[0].bend_raw && !midi_bend_target[0]);
    queued(0xB0, 121, 0, 1); settle(); assert(a->mpe_bend_q24 == expected && !b->mpe_bend_q24);
    queued(0xB1, 121, 0, 1); settle(); assert(!a->mpe_bend_q24 && mpe_bend_ch[1].semis == 12);
    assert(!memcmp(patch, fm6_patch, sizeof patch) && !memcmp(functions, fm6_fn, sizeof functions));
    puts("independent member/master RPN0, additive master bend, CC121 and intact DX7 data OK");

    queued(0x93, 127, 100, 1); voice_t *edge = sounding(owner(3, 127));
    settle(); int32_t edge_frequency = dsp(edge)->fq[0];
    /* A nonmember's conventional bend on this track must not leak through
     * generic pitch clamping or FM6's shared control state. */
    sensitivity(4, 24, 0); bend(4, 8191); settle();
    assert(dsp(edge)->fq[0] == edge_frequency && !edge->mpe_bend_q24);
    assert(dsp(b)->fq[0] == centre);
    bend(4, 0); queued(0x83, 127, 0, 1);
    puts("conventional bend cannot leak into MPE through shared state or pitch clamps OK");

    /* No overshoot, exact landing and every conversion at maximum sensitivity
     * remains representable. This tests conversion independently of aliasing. */
    sensitivity(1, 127, 127); assert(mpe_bend_ch[1].cents == 99);
    for (int raw = -8192; raw <= 8191; raw++) {
        mpe_bend_ch[1].raw = (int16_t)raw;
        int32_t value = mpe_bend_value(1);
        int32_t limit = (int32_t)((int64_t)12799 * (1 << 24) / 1200);
        assert(value >= -limit && value <= limit);
        assert(value == (int32_t)((int64_t)raw * 12799 * (1 << 24) /
                                  ((int64_t)(raw < 0 ? 8192 : 8191) * 1200)));
    }
    sensitivity(1, 48, 0); bend(1, 0); settle(); bend(1, 8191);
    int32_t previous = a->mpe_bend_q24;
    for (uint32_t i = 0; i < 128; i++) {
        fm6_mpe_bend_tick(a);
        assert(a->mpe_bend_q24 >= previous && a->mpe_bend_q24 <= a->mpe_bend_target);
        previous = a->mpe_bend_q24;
    }
    assert(a->mpe_bend_q24 == a->mpe_bend_target);
    puts("all 14-bit values at maximum RPN range convert safely; monotonic smoothing OK");

    setup(); sensitivity(1, 12, 0);
    bend(1, 8191); queued(0x91, 60, 100, 1); a = sounding(owner(1, 60));
    queued(0xB0, 64, 127, 1); queued(0x81, 60, 0, 1);
    int32_t frozen = a->mpe_bend_target;
    bend(1, -8192); assert(a->mpe_bend_target == frozen);
    queued(0x91, 64, 100, 1); b = sounding(owner(1, 64));
    assert(b && b != a && b->mpe_bend_target == -(1 << 24));
    bend(1, 0); assert(a->mpe_bend_target == frozen && !b->mpe_bend_target);
    queued(0xB0, 64, 0, 1); assert(a->active && !a->gate);
    bend(1, 8191); assert(a->mpe_bend_target == frozen && b->mpe_bend_target == (1 << 24));
    queued(0x81, 64, 0, 1); bend(1, -8192); assert(b->mpe_bend_target == (1 << 24));
    /* Old still-held note is not selected again when latest instance ends. */
    queued(0x92, 60, 100, 1); voice_t *older = sounding(owner(2, 60));
    queued(0x92, 61, 100, 1); queued(0x82, 61, 0, 1);
    bend(2, 8191); assert(older->mpe_bend_target == 0);
    configure(0); assert(a->mpe_bend == 1 && b->mpe_bend == 1);
    int32_t tail = a->mpe_bend_target;
    configure(3); bend(0, 8191); assert(a->mpe_bend_target == tail);
    puts("pre-note expression, sustain/release snapshots, no fallback and session isolation OK");

    /* Ghost cannot reuse shared track bend or skip a pending per-voice ramp. */
    fm6_part_t *p = FM6P(&trk[0]); fm6_voice_t *s = dsp(a);
    a->active = a->gate = 0; a->mpe_bend_q24 = 0; a->mpe_bend_target = 1 << 24;
    s->still = 1; s->spb = song.g[G_TUNE] * 13981 + tuning_log();
    p->pt.pb = -(1 << 24); fm6_ghost(&trk[0], a, s);
    assert(a->mpe_bend_q24 > 0 && a->mpe_bend_q24 < a->mpe_bend_target);
    assert(s->spb == a->mpe_bend_q24 + song.g[G_TUNE] * 13981 + tuning_log());
    puts("ghost pitch cache respects per-voice bend and pending smoothing OK");
    mpe_clear(1);
    printf("bend fields %zu bytes/voice; channels %zu + master %zu bytes\n",
           sizeof a->mpe_bend_q24 + sizeof a->mpe_bend_target, sizeof mpe_bend_ch, sizeof mpe_master_bend);
    return 0;
}
