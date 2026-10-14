/* SPDX-License-Identifier: GPL-3.0-only */
#define MPE_NOTES_NO_MAIN 1
#include "mpe_notes_test.c"
#include <math.h>

#define NSIGNAL 4096
static int32_t signal0[NSIGNAL], signal1[NSIGNAL];

static void timbre(uint32_t ch, uint32_t value) { queued(0xB0u | ch, 74, value, 1); }
static void pressure(uint32_t ch, uint32_t value) { queued(0xD0u | ch, value, 0, 1); }
static void settle(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        int32_t out[CTL]; mod_begin(&trk[0]); track_render(&trk[0], out, CTL); mod_end(&trk[0]);
    }
}
static void capture(voice_t *v, const fm6_voice_t *saved, uint32_t cc74, int32_t *out)
{
    *fm6_state(&trk[0], v) = *saved;
    timbre(1, cc74); v->mpe_timbre_level = v->mpe_timbre_target;
    vmod_t m = {.amp0 = 32767, .amp1 = 32767, .shape = 64 << 8};
    for (uint32_t at = 0; at < NSIGNAL; at += CTL) {
        memset(out + at, 0, CTL * sizeof *out); fm6_render(&trk[0], v, out + at, CTL, &m); fm6_join();
        for (uint32_t i = 0; i < CTL; i++) assert(isfinite(fm6_sum[i]) && abs(out[at + i]) < 32768);
    }
}
static double difference(const int32_t *a, const int32_t *b)
{
    double d = 0, e = 0;
    for (uint32_t i = 0; i < NSIGNAL; i++) { double x = a[i] - b[i]; d += x * x; e += (double)a[i] * a[i]; }
    return e ? sqrt(d / e) : 0;
}

int main(void)
{
    uint32_t changed = 0;
    for (uint32_t alg = 0; alg < 32; alg++) {
        setup(); uint8_t raw[FP_SIZE]; fm6_unpack(FM6_INIT, raw); raw[FP_ALG] = (uint8_t)alg; raw[FP_FB] = 0;
        for (uint32_t k = 0; k < 6; k++) {
            uint8_t *op = raw + k * FP_OP; op[FP_OL] = fm6_carrier(alg, 6 - k) ? 80 : 60;
            op[FP_FC] = 1; op[FP_AMS] = 3; for (uint32_t j = 0; j < 3; j++) op[FP_L1 + j] = 99;
        }
        fm6_put_patch(0, raw, 1); events_block(CTL); FM6F(&trk[0])[FN_ENGINE] = FM6_MODERN;
        queued(0x91, 60, 100, 1); queued(0x92, 60, 110, 1); settle();
        voice_t *a = sounding(owner(1, 60)), *b = sounding(owner(2, 60));
        fm6_voice_t saved = *fm6_state(&trk[0], a); fm6_ctl_t cache = FM6P(&trk[0])->pt;
        capture(a, &saved, 0, signal0); capture(a, &saved, 127, signal1);
        double d = difference(signal0, signal1); if (d > .01) changed++;
        if (alg == 31) assert(!memcmp(signal0, signal1, sizeof signal0));
        assert(!b->mpe_timbre_target && !memcmp(&cache, &FM6P(&trk[0])->pt, sizeof cache));
        *fm6_state(&trk[0], a) = saved; a->mpe_timbre_level = a->mpe_timbre_target = 0;
        vmod_t m = {.shape = 64 << 8}; fm6_control(&trk[0], a, fm6_state(&trk[0], a), &m);
        float gains[6]; memcpy(gains, fm6_state(&trk[0], a)->gout, sizeof gains);
        *fm6_state(&trk[0], a) = saved; a->mpe_timbre_level = a->mpe_timbre_target = 32768;
        fm6_control(&trk[0], a, fm6_state(&trk[0], a), &m);
        for (uint32_t k = 0; k < 6; k++) {
            double expected = fm6_carrier(alg, 6 - k) ? 1 : pow(2., .25);
            assert(fabs(fm6_state(&trk[0], a)->gout[k] / gains[k] - expected) < .0001);
        }
    }
    assert(changed > 20); printf("CC74 changed %u/32 controlled algorithms; carrier-only case unchanged\n", changed);

    setup(); timbre(1, 64); queued(0x91, 60, 100, 1); voice_t *a = sounding(owner(1, 60));
    assert(a->mpe_timbre_target == mpe_pressure_level(64));
    queued(0x92, 60, 100, 1); voice_t *b = sounding(owner(2, 60)); timbre(2, 127);
    assert(a->mpe_timbre_target == mpe_pressure_level(64) && b->mpe_timbre_target == 32768);
    int32_t prior = 0;
    for (uint32_t i = 0; i < 128; i++) {
        vmod_t m = {.shape = 64 << 8}; fm6_control(&trk[0], b, fm6_state(&trk[0], b), &m);
        assert(b->mpe_timbre_level >= prior && b->mpe_timbre_level <= 32768); prior = b->mpe_timbre_level;
    }
    assert(b->mpe_timbre_level == 32768);
    timbre(2, 0); prior = b->mpe_timbre_level;
    for (uint32_t i = 0; i < 128; i++) {
        vmod_t m = {.shape = 64 << 8}; fm6_control(&trk[0], b, fm6_state(&trk[0], b), &m);
        assert(b->mpe_timbre_level <= prior); prior = b->mpe_timbre_level;
    }
    assert(!b->mpe_timbre_level); /* a rapid reversal still lands exactly and monotonically */
    timbre(2, 127);
    timbre(0, 127); assert(b->mpe_timbre_target == 32768 && !trk[0].at); /* master is isolated */
    queued(0xB0, 64, 127, 1); queued(0x81, 60, 0, 1); uint16_t frozen = a->mpe_timbre_target;
    timbre(1, 0); queued(0x91, 64, 100, 1); voice_t *c = sounding(owner(1, 64));
    assert(a->mpe_timbre_target == frozen && !c->mpe_timbre_target);
    pressure(1, 127); timbre(1, 127); queued(0xE1, 127, 127, 1);
    assert(c->mpe_pressure_target == 32768 && c->mpe_timbre_target == 32768 && c->mpe_bend_target);
    fm6_voice_t both = *fm6_state(&trk[0], c); vmod_t m = {.shape = 64 << 8}; float capped[6];
    c->mpe_pressure_level = c->mpe_pressure_target; c->mpe_timbre_level = c->mpe_timbre_target;
    fm6_control(&trk[0], c, fm6_state(&trk[0], c), &m);
    memcpy(capped, fm6_state(&trk[0], c)->gout, sizeof capped);
    *fm6_state(&trk[0], c) = both; c->mpe_timbre_level = 0;
    fm6_control(&trk[0], c, fm6_state(&trk[0], c), &m);
    for (uint32_t k = 0; k < 6; k++)
        assert(fabs(capped[k] - fm6_state(&trk[0], c)->gout[k]) < .0001f);
    queued(0xB0, 64, 0, 1); timbre(1, 0); assert(a->mpe_timbre_target == frozen);
    note_id_t stolen = owner(1, 64); voice_kill(c); queued(0x91, 67, 100, 1); c = sounding(owner(1, 67));
    timbre(1, 64); queued(0x81, 64, 0, 1); assert(c->gate && c->mpe_timbre_target == mpe_pressure_level(64));
    assert(!note_identity_valid(stolen));
    queued(0xB1, 121, 0, 1); assert(!c->mpe_timbre_target); configure(0); assert(!owners() && !identities());
    puts("independence, smoothing, bend/pressure combination, sustain, reuse, stolen identity and reset OK");

    setup(); queued(0x91, 60, 100, 1); a = sounding(owner(1, 60)); fm6_voice_t saved = *fm6_state(&trk[0], a);
    capture(a, &saved, 0, signal0); *fm6_state(&trk[0], a) = saved; capture(a, &saved, 0, signal1);
    assert(!memcmp(signal0, signal1, sizeof signal0)); /* no CC74 and CC74=0 are PCM-identical */
    mpe_clear(1); configure(0);
    int16_t params[NTRK][P_COUNT];
    for (uint32_t tr = 0; tr < NTRK; tr++) memcpy(params[tr], trk[tr].p, sizeof params[tr]);
    queued(0xB1, 74, 127, 1);
    uint32_t conventional_change = 0;
    for (uint32_t tr = 0; tr < NTRK; tr++) conventional_change |= memcmp(params[tr], trk[tr].p, sizeof params[tr]) != 0;
    assert(conventional_change && !mpe_zone.members && !owners());
    puts("neutral CC74 is PCM-identical; conventional CC74 remains outside MPE handling OK");
    return 0;
}
