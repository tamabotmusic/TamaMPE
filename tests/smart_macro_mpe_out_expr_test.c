/* SPDX-License-Identifier: GPL-3.0-only */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>

static uint32_t status(uint32_t n) { return (midi_out_q[n % MQ] >> 8) & 255u; }
static uint32_t data1(uint32_t n) { return (midi_out_q[n % MQ] >> 16) & 127u; }
static uint32_t data2(uint32_t n) { return (midi_out_q[n % MQ] >> 24) & 127u; }

static void reset_out(uint32_t members)
{
    host_tracks_init(); memset(&mpe_out, 0, sizeof mpe_out); mpe_out_forget();
    memset(smart_mpe_out, 0, sizeof smart_mpe_out);
    memset(smart_mpe_out_shadow, 0, sizeof smart_mpe_out_shadow);
    host_preset(&trk[0], ENGI_FM6, 0); host_preset(&trk[1], ENGI_FM6, 0);
    mo_w = mo_r = 0; usb.config = 1; usb.suspended = 0; mpe_out_configure(members);
    for (uint32_t i = 0; i < 3u + 4u * members; i++) mpe_out_service();
    assert(mpe_out_ready()); mo_r = mo_w;
}
static void value(uint32_t track, uint32_t macro, int32_t v)
{ smart_macro[track][macro].value = smart_q15(v); }
static void map(uint32_t macro, uint32_t destination, int32_t depth, int32_t lo, int32_t hi, uint32_t curve)
{ smart_mpe_out_map_test_config(&trk[0], macro, destination, depth, lo, hi, curve); }

int main(void)
{
    uint16_t a, b, c, d;
    uint16_t expr[MPE_EXPR_N];

    reset_out(2);
    map(0, SMART_MPE_OUT_CC74, 127, -127, 127, SMART_CURVE_LINEAR);
    map(1, SMART_MPE_OUT_PRESSURE, 127, -127, 127, SMART_CURVE_SMOOTH);
    map(2, SMART_MPE_OUT_BEND, 127, -127, 127, SMART_CURVE_EXP);
    value(0, 0, 50); value(0, 1, 64); value(0, 2, -127);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100);
    assert(a && mo_w == mo_r + 4u);
    assert(status(mo_r) == 0xb1u && data1(mo_r) == 74u && data2(mo_r) == 50u);
    assert(status(mo_r + 1u) == 0xd1u && data1(mo_r + 1u) > 0u);
    assert(status(mo_r + 2u) == 0xe1u && data1(mo_r + 2u) == 0u && data2(mo_r + 2u) == 0u);
    assert(status(mo_r + 3u) == 0x91u);
    puts("Smart Macro MPE OUT expression: CC74, pressure and signed 14-bit bend initialise before Note On OK");

    /* Every curve retains an exact zero and reaches the full 14-bit bend
     * domain.  Positive and negative depths invert the direction without
     * overflowing the compact signed route representation. */
    for (uint32_t curve = SMART_CURVE_LINEAR; curve <= SMART_CURVE_EXP; curve++) {
        reset_out(1); map(0, SMART_MPE_OUT_BEND, 127, -127, 127, curve);
        value(0, 0, -127); assert(smart_mpe_out_values(&trk[0], expr) && expr[2] == 0u);
        value(0, 0, 0); assert(smart_mpe_out_values(&trk[0], expr) && expr[2] == 8192u);
        value(0, 0, 127); assert(smart_mpe_out_values(&trk[0], expr) && expr[2] == 16383u);
        map(0, SMART_MPE_OUT_BEND, -127, -127, 127, curve);
        value(0, 0, 127); assert(smart_mpe_out_values(&trk[0], expr) && expr[2] == 0u);
        value(0, 0, -127); assert(smart_mpe_out_values(&trk[0], expr) && expr[2] == 16383u);
    }
    reset_out(1); map(0, SMART_MPE_OUT_CC74, 127, -127, 127, SMART_CURVE_LINEAR);
    map(1, SMART_MPE_OUT_CC74, 127, -127, 127, SMART_CURVE_LINEAR);
    map(2, SMART_MPE_OUT_CC74, 127, -127, 127, SMART_CURVE_LINEAR);
    map(3, SMART_MPE_OUT_CC74, 127, -127, 127, SMART_CURVE_LINEAR);
    for (uint32_t k = 0; k < 4u; k++) value(0, k, 127);
    assert(smart_mpe_out_values(&trk[0], expr) && expr[0] == 127u && expr[1] == 0u && expr[2] == 8192u);
    puts("Smart Macro MPE OUT expression: all curves, signed depths and 14-bit endpoints are exact OK");

    /* Two routes sharing a destination compose then saturate, while zero is
     * exact neutral even when a route's requested bounds excluded zero. */
    reset_out(1);
    map(0, SMART_MPE_OUT_CC74, 127, 10, 30, SMART_CURVE_LINEAR);
    map(1, SMART_MPE_OUT_CC74, 127, 0, 127, SMART_CURVE_LINEAR);
    value(0, 0, 0); value(0, 1, 0);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); assert(a && data2(mo_r) == 0u);
    mpe_out_note_off(a); mo_r = mo_w;
    value(0, 0, 100); value(0, 1, 100);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 61, 100); assert(a && data2(mo_r) == 127u);
    puts("Smart Macro MPE OUT expression: zero neutral and additive saturation OK");

    /* Four programmed notes reserve all three expressions plus Note On. */
    reset_out(4);
    map(0, SMART_MPE_OUT_CC74, 127, -127, 127, SMART_CURVE_LINEAR);
    map(1, SMART_MPE_OUT_PRESSURE, 127, -127, 127, SMART_CURVE_LINEAR);
    map(2, SMART_MPE_OUT_BEND, 127, -127, 127, SMART_CURVE_LINEAR);
    value(0, 0, 20); value(0, 1, 30); value(0, 2, 40);
    a=mpe_out_note_on_track(&trk[0],0,0,60,100); b=mpe_out_note_on_track(&trk[0],1,0,61,100);
    c=mpe_out_note_on_track(&trk[0],2,0,62,100); d=mpe_out_note_on_track(&trk[0],3,0,63,100);
    assert(a && b && c && d && mo_w == mo_r + 16u);
    mo_r = mo_w; value(0, 0, 21); value(0, 1, 31); value(0, 2, 41); mpe_out_macro_service();
    assert(mo_w == mo_r + 4u); /* global cap, shared by all three dimensions */
    puts("Smart Macro MPE OUT expression: four-note atomic setup and shared four-event budget OK");

    /* With all 15 owners changing CC74 every block, the rotating slot cursor
     * must still reach pressure and bend.  Draining the simulated USB queue
     * after every block models normal endpoint service. */
    reset_out(15);
    map(0, SMART_MPE_OUT_CC74, 127, -127, 127, SMART_CURVE_LINEAR);
    map(1, SMART_MPE_OUT_PRESSURE, 127, -127, 127, SMART_CURVE_LINEAR);
    map(2, SMART_MPE_OUT_BEND, 127, -127, 127, SMART_CURVE_LINEAR);
    value(0, 0, 1); value(0, 1, 1); value(0, 2, 1);
    for (uint32_t i = 0; i < 15u; i++) assert(mpe_out_note_on_track(&trk[0], i, 0, 48u + i, 100));
    mo_r = mo_w; value(0, 1, 80); value(0, 2, -80); smart_mpe_out_values(&trk[0], expr);
    for (uint32_t block = 0; block < 45u; block++) {
        value(0, 0, (int32_t)(block % 127u) + 1); mpe_out_macro_service(); mo_r = mo_w;
    }
    for (uint32_t i = 0; i < 15u; i++)
        assert(mpe_out_owner[i].pressure_sent == expr[1] && mpe_out_owner[i].bend_sent == expr[2]);
    puts("Smart Macro MPE OUT expression: continuous CC74 cannot starve pressure or bend in a 15-member zone OK");

    /* A note released with non-neutral expression leaves a free-channel
     * cleanup obligation.  Turning routes off cannot leak that expression to
     * a conventional replacement note. */
    reset_out(1); map(0, SMART_MPE_OUT_CC74, 127, -127, 127, SMART_CURVE_LINEAR); value(0, 0, 70);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); assert(a); mpe_out_note_off(a);
    assert(mpe_out_channel_dirty[0]); mo_r = mo_w;
    map(0, SMART_MPE_OUT_OFF, 0, 0, 0, SMART_CURVE_LINEAR);
    b = mpe_out_note_on_track(&trk[0], 0, 0, 61, 100);
    assert(b && mo_w == mo_r + 4u && status(mo_r) == 0xb1u && data2(mo_r) == 0u &&
           status(mo_r + 1u) == 0xd1u && status(mo_r + 2u) == 0xe1u && data1(mo_r + 2u) == 0u && data2(mo_r + 2u) == 64u &&
           status(mo_r + 3u) == 0x91u && !mpe_out_channel_dirty[0]);
    mpe_out_note_off(b); mo_r = mo_w;
    puts("Smart Macro MPE OUT expression: released non-neutral channels are neutralised before reuse OK");

    /* Destination changes neutralise the old controller.  Once disabled, an
     * owner is muted permanently and cannot receive a later reactivation. */
    reset_out(1); map(0, SMART_MPE_OUT_CC74, 127, -127, 127, SMART_CURVE_LINEAR); value(0, 0, 50);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); assert(a); mo_r = mo_w;
    map(0, SMART_MPE_OUT_PRESSURE, 127, -127, 127, SMART_CURVE_LINEAR); value(0, 0, 60);
    mpe_out_macro_service(); assert(mo_w == mo_r + 2u);
    assert((status(mo_r) == 0xb1u && data2(mo_r) == 0u) || (status(mo_r + 1u) == 0xb1u && data2(mo_r + 1u) == 0u));
    mo_r = mo_w; map(0, SMART_MPE_OUT_OFF, 0, 0, 0, SMART_CURVE_LINEAR); mpe_out_macro_service();
    assert(mo_w == mo_r + 1u && status(mo_r) == 0xd1u && data1(mo_r) == 0u);
    mo_r = mo_w; map(0, SMART_MPE_OUT_PRESSURE, 127, -127, 127, SMART_CURVE_LINEAR); value(0, 0, 80);
    mpe_out_macro_service(); assert(mo_w == mo_r);
    puts("Smart Macro MPE OUT expression: destination changes and muted owners cannot revive stale expression OK");

    /* No owner means no deferred traffic.  USB reset clears both ownership
     * and free-channel bookkeeping, matching the zone's remote reset path. */
    reset_out(1); map(0, SMART_MPE_OUT_PRESSURE, 127, -127, 127, SMART_CURVE_LINEAR); value(0, 0, 80);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); assert(a); mo_r = mo_w;
    value(0, 0, 81); mpe_out_note_off(a); mpe_out_macro_service();
    assert(mo_w == mo_r + 1u && !mpe_out_find(a));
    mpe_out_usb_reset(); assert(!mpe_out_channel_dirty[0] && !mpe_out_find(a));
    puts("Smart Macro MPE OUT expression: released and USB-reset owners cannot emit stale expression OK");
    return 0;
}
