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
    host_preset(&trk[0], ENGI_FM6, 0); host_preset(&trk[1], ENGI_FM6, 0);
    memset(smart_mpe_out, 0, sizeof smart_mpe_out);
    memset(smart_mpe_out_shadow, 0, sizeof smart_mpe_out_shadow);
    mo_w = mo_r = 0; usb.config = 1; usb.suspended = 0; mpe_out_configure(members);
    for (uint32_t i = 0; i < 3u + 4u * members; i++) mpe_out_service();
    assert(mpe_out_ready()); mo_r = mo_w;
}

static void macro_value(uint32_t track, int32_t value)
{ smart_macro[track][0].value = smart_q15(value); }

int main(void)
{
    uint16_t a, b, id[15];
    uint32_t base;

    reset_out(2);
    /* The phase-5A route is OFF by default: exact legacy one-packet note on. */
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100);
    assert(a && mo_w == mo_r + 1u && status(mo_r) == 0x91u);
    mpe_out_note_off(a); mo_r = mo_w;
    puts("Smart Macro MPE OUT: OFF keeps the legacy Note On path byte-for-byte OK");

    reset_out(2); mpe_out_macro_cc74_enable(&trk[0], 1); macro_value(0, 64);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100);
    assert(a && mo_w == mo_r + 4u && status(mo_r) == 0xb1u && data1(mo_r) == 74u && data2(mo_r) == 64u &&
           status(mo_r + 1u) == 0xd1u && data1(mo_r + 1u) == 0u &&
           status(mo_r + 2u) == 0xe1u && data1(mo_r + 2u) == 0u && data2(mo_r + 2u) == 64u &&
           status(mo_r + 3u) == 0x91u);
    assert(mpe_out_owner[0].track == 0u && mpe_out_owner[0].cc74_sent == 64u);
    puts("Smart Macro MPE OUT: initial positive Macro 1 CC74 precedes the matching Note On OK");

    /* Negative and zero are neutral, and the owner keeps its origin track. */
    mo_r = mo_w; song.sel = 1; macro_value(0, -127); mpe_out_macro_service();
    assert(mo_w == mo_r + 1u && status(mo_r) == 0xb1u && data2(mo_r) == 0u);
    mpe_out_macro_cc74_enable(&trk[1], 1); macro_value(1, 120);
    b = mpe_out_note_on_track(&trk[1], 1, 0, 64, 100); assert(b); mo_r = mo_w;
    macro_value(0, 20); macro_value(1, 100); mpe_out_macro_service();
    assert(mo_w == mo_r + 2u &&
           ((status(mo_r) == 0xb1u && data2(mo_r) == 20u && status(mo_r + 1u) == 0xb2u && data2(mo_r + 1u) == 100u) ||
            (status(mo_r) == 0xb2u && data2(mo_r) == 100u && status(mo_r + 1u) == 0xb1u && data2(mo_r + 1u) == 20u)));
    mo_r = mo_w; mpe_out_macro_service(); assert(mo_w == mo_r); /* deduplicated */
    puts("Smart Macro MPE OUT: per-owner track identity, neutral mapping and deduplication OK");

    mpe_out_macro_cc74_enable(&trk[0], 0); mo_r = mo_w; mpe_out_macro_service();
    assert(mo_w == mo_r + 1u && status(mo_r) == 0xb1u && data2(mo_r) == 0u);
    /* A held note muted by disabling its map remains muted: re-enabling
     * affects future notes only, never revives stale expression. */
    mpe_out_macro_cc74_enable(&trk[0], 1); mo_r = mo_w; macro_value(0, 70); mpe_out_macro_service();
    assert(mo_w == mo_r);
    host_preset(&trk[0], ENGI_CZ, 0); mo_r = mo_w; mpe_out_macro_service();
    assert(mo_w == mo_r);
    host_preset(&trk[0], ENGI_FM6, 0);
    puts("Smart Macro MPE OUT: disabling the assignment or leaving FM6 neutralizes held external notes OK");

    /* Latest value wins while USB is unavailable; no delayed update asks for a resync. */
    reset_out(1); mpe_out_macro_cc74_enable(&trk[0], 1); macro_value(0, 64);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); assert(a); mo_r = mo_w;
    usb.suspended = 1; macro_value(0, 90); mpe_out_macro_service();
    assert(mo_w == mo_r && mpe_out_owner[0].cc74_pending == 90u &&
           (mpe_out_owner[0].dirty & MPE_EXPR_DIRTY(MPE_EXPR_CC74)) && !mpe_out.resync);
    macro_value(0, 91); usb.suspended = 0; mpe_out_macro_service();
    assert(mo_w == mo_r + 1u && status(mo_r) == 0xb1u && data2(mo_r) == 91u && !mpe_out.resync);
    puts("Smart Macro MPE OUT: suspended USB defers only the latest CC74 without resync OK");

    mo_r = mo_w; mpe_timbre_update(1, 127); mpe_pressure_update(1, 96); mpe_bend_update(1);
    assert(mo_w == mo_r);
    puts("Smart Macro MPE OUT: received MPE expression is not bridged back to USB OK");

    reset_out(15); mpe_out_macro_cc74_enable(&trk[0], 1); macro_value(0, 127);
    for (uint32_t i = 0; i < 15u; i++) { id[i] = mpe_out_note_on_track(&trk[0], i, 0, 48u + i, 100); assert(id[i]); }
    mo_r = mo_w; macro_value(0, 40); mpe_out_macro_service();
    assert(mo_w == mo_r + 4u);
    for (uint32_t i = 0; i < 4u; i++) assert(status(mo_r + i) == 0xb1u + i && data1(mo_r + i) == 74u && data2(mo_r + i) == 40u);
    mo_r = mo_w; mpe_out_macro_service(); assert(mo_w == mo_r + 4u && status(mo_r) == 0xb5u);
    puts("Smart Macro MPE OUT: 15-member zone is round-robin and sends at most four CC74 events per block OK");

    /* Macro traffic stops at the low-priority threshold, leaving Note Off space. */
    reset_out(2); mpe_out_macro_cc74_enable(&trk[0], 1); macro_value(0, 64);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); b = mpe_out_note_on_track(&trk[0], 1, 0, 64, 100); assert(a && b); mo_r = mo_w;
    macro_value(0, 65); mo_w = mo_r + 31u; mpe_out_macro_service();
    assert(mo_w == mo_r + 32u && !mpe_out.resync); mpe_out_note_off(a); mpe_out_note_off(b);
    assert(mo_w == mo_r + 34u && !mpe_out.resync && !mpe_out_find(a) && !mpe_out_find(b));
    puts("Smart Macro MPE OUT: low-priority queue cap preserves multiple Note Off messages OK");

    reset_out(1); mpe_out_macro_cc74_enable(&trk[0], 1); macro_value(0, 10);
    mo_w = mo_r + MQ - 1u; base = mo_w;
    assert(!mpe_out_note_on_track(&trk[0], 0, 0, 60, 100) && mo_w == base && !mpe_out_owner[0].id);
    mo_r = mo_w; a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); assert(a);
    mpe_out_usb_reset(); assert(!mpe_out_find(a));
    puts("Smart Macro MPE OUT: initial CC74 plus Note On is atomic and USB reset retires ownership OK");

    reset_out(1); mpe_out_macro_cc74_enable(&trk[0], 1); macro_value(0, 90);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); assert(a); mpe_out_note_off(a); mo_r = mo_w;
    macro_value(0, 20); a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); assert(a);
    assert(status(mo_r) == 0xb1u && data1(mo_r) == 74u && data2(mo_r) == 20u &&
           status(mo_r + 1u) == 0xd1u && status(mo_r + 2u) == 0xe1u && status(mo_r + 3u) == 0x91u);
    puts("Smart Macro MPE OUT: a reused channel receives its current CC74 before a replacement Note On OK");

    reset_out(2); mpe_out_macro_cc74_enable(&trk[0], 1); macro_value(0, 32);
    a = mpe_out_note_on_track(&trk[0], 0, 0, 60, 100); assert(a); mo_r = mo_w;
    macro_value(0, 64); usb.suspended = 1; mpe_out_macro_service();
    assert(mpe_out_find(a)->dirty & MPE_EXPR_DIRTY(MPE_EXPR_CC74)); mpe_out_panic(); assert(!mpe_out_find(a));
    reset_out(2); mpe_out_macro_cc74_enable(&trk[0], 1); a = mpe_out_note_on_track(&trk[0], 0, 0, 61, 100); assert(a);
    mpe_out_request(1); mpe_out_service(); assert(!mpe_out_find(a));
    puts("Smart Macro MPE OUT: panic and zone changes cancel deferred owner state OK");
    return 0;
}
