/* SPDX-License-Identifier: GPL-3.0-only */
/* Smart Macros phase 1: runtime-only FM6 MLVL routes. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>

#define NSIGNAL (CTL * 32u)

static void reset_fm6(void)
{
    memset(trk, 0, sizeof trk);
    memset(smart_macro, 0, sizeof smart_macro);
    eng_state_reset();
    memset(fm6_eff, 0, sizeof fm6_eff);
    fm6_fn_reset();
    host_tracks_init();
    host_preset(&trk[0], ENGI_FM6, 0);
    trk[0].p[P_VOICE] = V_POLY;
    trk_note_on(&trk[0], 60, 100);
}

static void render_track(int32_t *out, uint32_t blocks)
{
    for (uint32_t b = 0; b < blocks; b++) {
        mod_begin(&trk[0]);
        smart_begin(&trk[0]);
        track_render(&trk[0], out + b * CTL, CTL);
        smart_end(&trk[0]);
        mod_end(&trk[0]);
    }
}

static uint64_t energy(const int32_t *a, uint32_t n)
{
    uint64_t e = 0;
    for (uint32_t i = 0; i < n; i++) {
        int64_t x = a[i];
        e += (uint64_t)(x * x);
    }
    return e;
}

int main(void)
{
    int32_t a[NSIGNAL], b[NSIGNAL];
    int16_t p2;

    reset_fm6();
    p2 = trk[0].p[P_E2];
    smart_macro_route(&trk[0], 0, SMART_DST_FM6_MLVL, 48);
    smart_macro_value(&trk[0], 0, 0);
    render_track(a, NSIGNAL / CTL);
    assert(trk[0].p[P_E2] == p2);

    reset_fm6();
    render_track(b, NSIGNAL / CTL);
    assert(!memcmp(a, b, sizeof a)); /* neutral route and no route are bit-identical */
    assert(!memcmp(fm6_patch[0], fm6_eff[0].p, FP_SIZE));

    reset_fm6();
    smart_macro_route(&trk[0], 0, SMART_DST_FM6_MLVL, 48);
    smart_macro_value(&trk[0], 0, 127);
    int16_t old = 0;
    for (uint32_t i = 0; i < 24; i++) {
        int32_t one[CTL];
        mod_begin(&trk[0]); smart_begin(&trk[0]);
        assert(smart_macro[0][0].value >= old);
        old = smart_macro[0][0].value;
        track_render(&trk[0], one, CTL); smart_end(&trk[0]); mod_end(&trk[0]);
        assert(trk[0].p[P_E2] == p2);
    }
    assert(smart_macro[0][0].value > 32000);
    reset_fm6();
    smart_macro_route(&trk[0], 0, SMART_DST_FM6_MLVL, 48);
    smart_macro_value(&trk[0], 0, 127);
    render_track(a, NSIGNAL / CTL);
    reset_fm6();
    render_track(b, NSIGNAL / CTL);
    assert(memcmp(a, b, sizeof a));
    assert(energy(a, NSIGNAL) != energy(b, NSIGNAL));
    assert(!memcmp(fm6_patch[0], fm6_eff[0].p, FP_SIZE));
    puts("Smart Macro MLVL route changes FM6 while DX7 patch remains intact");

    reset_fm6();
    trk[0].p[P_M1SRC] = MS_MODW;
    trk[0].p[P_M1DST] = MD_E1 + 2u;
    trk[0].p[P_M1AMT] = 32;
    trk[0].mw = 127;
    smart_macro_route(&trk[0], 0, SMART_DST_FM6_MLVL, -32);
    smart_macro_value(&trk[0], 0, 127);
    for (uint32_t i = 0; i < 32; i++) {
        int32_t one[CTL];
        mod_begin(&trk[0]);
        int16_t matrix_value = trk[0].p[P_E2];
        smart_begin(&trk[0]);
        assert(trk[0].p[P_E2] != matrix_value);
        track_render(&trk[0], one, CTL);
        smart_end(&trk[0]);
        assert(trk[0].p[P_E2] == matrix_value);
        mod_end(&trk[0]);
        assert(trk[0].p[P_E2] == p2);
    }
    smart_macro_reset(&trk[0]);
    mod_begin(&trk[0]); smart_begin(&trk[0]); smart_end(&trk[0]); mod_end(&trk[0]);
    assert(!smart_macro[0][0].value && !smart_macro[0][0].target);
    puts("block smoothing, matrix coexistence and restoration OK");

    reset_fm6();
    int16_t base_ml = trk[0].p[P_E2], base_pan = trk[0].p[P_PAN];
    smart_macro_route_config(&trk[0], 0, 0, SMART_DST_FM6_MLVL, 40, -10, 10, SMART_CURVE_LINEAR);
    smart_macro_route_config(&trk[0], 0, 1, SMART_DST_FM6_MLVL, 30, -8, 8, SMART_CURVE_SMOOTH);
    smart_macro_route_config(&trk[0], 1, 0, SMART_DST_FM6_MLVL, -20, -64, 63, SMART_CURVE_EXP);
    smart_macro_route_config(&trk[0], 1, 1, SMART_DST_PAN, 30, -6, 6, SMART_CURVE_EXP);
    smart_macro_value(&trk[0], 0, 127); smart_macro_value(&trk[0], 1, 127);
    mod_begin(&trk[0]); smart_begin(&trk[0]);
    assert(trk[0].p[P_E2] <= base_ml + 18 && trk[0].p[P_E2] >= base_ml - 64);
    assert(trk[0].p[P_PAN] > base_pan && trk[0].p[P_PAN] <= base_pan + 6);
    smart_end(&trk[0]); mod_end(&trk[0]);
    assert(trk[0].p[P_E2] == base_ml && trk[0].p[P_PAN] == base_pan);
    smart_macro_value(&trk[0], 0, 0); smart_macro_value(&trk[0], 1, 0);
    for (uint32_t i = 0; i < 64; i++) {
        int32_t one[CTL]; mod_begin(&trk[0]); smart_begin(&trk[0]);
        track_render(&trk[0], one, CTL); smart_end(&trk[0]); mod_end(&trk[0]);
    }
    assert(trk[0].p[P_E2] == base_ml && trk[0].p[P_PAN] == base_pan);
    puts("two routes per macro, shared destinations, limits, curves and exact zero OK");
    return 0;
}
