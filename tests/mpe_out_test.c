/* SPDX-License-Identifier: GPL-3.0-only */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>

static void reset_out(uint32_t members)
{
    host_tracks_init(); memset(&mpe_out, 0, sizeof mpe_out); mpe_out_forget();
    mo_w = mo_r = 0; usb.config = 1; mpe_out_configure(members);
    for (uint32_t i = 0; i < 3u + 4u * members; i++) mpe_out_service();
    assert(mpe_out_ready());
}
static uint32_t status(uint32_t n) { return (midi_out_q[n % MQ] >> 8) & 255u; }
static uint32_t data1(uint32_t n) { return (midi_out_q[n % MQ] >> 16) & 127u; }
static uint32_t data2(uint32_t n) { return (midi_out_q[n % MQ] >> 24) & 127u; }
static void drain_transition(void)
{
    uint32_t guard = 256;
    while (mpe_out_ui_pending() && guard--) { mpe_out_service(); mo_r = mo_w; }
    assert(!mpe_out_ui_pending());
}
int main(void)
{
    host_tracks_init(); memset(&mpe_out, 0, sizeof mpe_out); mpe_out_forget(); mo_w = mo_r = 0; usb.config = 1;
    mpe_out_configure(15);
    for (uint32_t i = 0; i < 10; i++) mpe_out_service();
    assert(mpe_out.setup == 10u && !mpe_out_ready() && !mpe_out_note_on(0, 0, 60, 100));
    mo_r = mo_w; /* host consumed the partial setup */
    while (!mpe_out_ready()) { mpe_out_service(); mo_r = mo_w; }
    assert(mpe_out.setup == 63u);
    puts("MPE OUT: partial configuration pauses notes and resumes without losing RPN state OK");

    reset_out(15);
    assert(mo_w == 63u && status(0) == 0xb0u && status(62) == 0xbfu &&
           ((midi_out_q[62] >> 16) & 127u) == 38u);
    assert(mpe_out_note_on(0, 0, 60, 100) && status(63) == 0x91u);
    mo_r = mo_w;
    puts("MPE OUT: 15-member RPN6/RPN0 setup uses 63 packets and precedes notes OK");

    reset_out(3);
    assert(mo_w == 15u && status(0) == 0xb0u && ((midi_out_q[2] >> 24) & 127u) == 3u);
    mo_r = mo_w; /* setup was transmitted */
    uint16_t a = mpe_out_note_on(1, 0, 60, 100), b = mpe_out_note_on(2, 0, 60, 110), c = mpe_out_note_on(3, 0, 64, 90);
    assert(a && b && c && a != b && b != c && !mpe_out_note_on(4, 0, 67, 100));
    assert(status(mo_r) == 0x91u && status(mo_r + 1u) == 0x92u && status(mo_r + 2u) == 0x93u);
    mpe_out_bend(a, 16383); mpe_out_pressure(b, 64); mpe_out_timbre(c, 127);
    assert(status(mo_r + 3u) == 0xe1u && status(mo_r + 4u) == 0xd2u && status(mo_r + 5u) == 0xb3u);
    mpe_out_note_off(b); mpe_out_note_off(a); mpe_out_note_off(c);
    assert(status(mo_r + 6u) == 0x82u && status(mo_r + 7u) == 0x81u && status(mo_r + 8u) == 0x83u);
    assert(!mpe_out_find(a) && !mpe_out_find(b) && !mpe_out_find(c));
    puts("MPE OUT: lower-zone setup, equal notes, independent expression and releases OK");

    reset_out(2); mo_r = mo_w; kb_prev = 0; fm1_in.notes = 1u << 7; keyboard_block();
    assert(mo_w == mo_r + 1u && status(mo_r) == 0x91u && kb_mpe_out[7][0]);
    fm1_in.notes = 0; keyboard_block(); assert(mo_w == mo_r + 2u && status(mo_r + 1u) == 0x81u);
    puts("MPE OUT: physical keyboard uses member channels while the internal synth remains local OK");

    reset_out(2); mo_r = mo_w; uint16_t id = mpe_out_note_on(0, 0, 60, 100); assert(id);
    mo_w = mo_r + MQ; mpe_out_note_off(id); assert(mpe_out.resync && !mpe_out_find(id));
    mo_r = mo_w; mpe_out_service(); assert(status(mo_r) == 0xb0u && ((midi_out_q[mo_r % MQ] >> 16) & 127u) == 120u);
    kb_mpe_out[0][0] = id; mpe_out_usb_reset();
    assert(!mpe_out_find(id) && mpe_out.resync && !kb_mpe_out[0][0]);
    puts("MPE OUT: saturated queue, lost Note Off and USB reset schedule safe All Sound Off recovery OK");

    mpe_out_configure(0); mo_w = mo_r = 0; assert(!mpe_out_enabled());
    midi_out_event(mpe_out_packet(9, 0x90, 60, 100)); assert(mo_w == 1u && status(0) == 0x90u);
    puts("MPE OUT: disabled mode leaves conventional MIDI OUT unchanged OK");

    reset_out(3); mo_r = mo_w;
    uint16_t held = mpe_out_note_on(0, 0, 60, 100); assert(held);
    mo_r = mo_w; uint32_t base = mo_r; mpe_out_request(0);
    assert(mpe_out_ui_pending() && !mpe_out_note_on(1, 0, 61, 100));
    /* One master + three member All Sound Off messages, then master RPN 6 = 0. */
    for (uint32_t i = 0; i < 8u; i++) { mpe_out_service(); mo_r = mo_w; }
    assert(!mpe_out_ui_pending() && !mpe_out_enabled() && !mpe_out_find(held));
    assert(status(base) == 0xb0u && data1(base) == 120u && status(base + 3u) == 0xb3u && data1(base + 4u) == 101u && data1(base + 5u) == 100u && data2(base + 5u) == 6u && data1(base + 6u) == 6u && !data2(base + 6u));
    puts("MPE OUT: disabling a live zone sends All Sound Off and RPN 6 = 0 before conventional output OK");

    reset_out(15); mo_r = mo_w; mpe_out_request(1);
    /* A full USB queue pauses the transition; no owner or configuration is lost. */
    mo_w = mo_r + MQ; mpe_out_service(); assert(mpe_out_ui_pending() && mpe_out.retire == 1u);
    mo_r = mo_w; drain_transition(); assert(mpe_out_ready() && mpe_out.members == 1u);
    puts("MPE OUT: member changes survive a full queue and reconfigure only after safely retiring the old zone OK");

    reset_out(2); mo_r = mo_w; usb.config = 0; mpe_out_request(1); mpe_out_service();
    assert(mpe_out_ui_pending() && !mpe_out_note_on(0, 0, 60, 100));
    usb.config = 1; drain_transition(); assert(mpe_out_ready() && mpe_out.members == 1u);
    puts("MPE OUT: disconnect pauses a requested change and reconnect completes it without interleaved notes OK");
    return 0;
}
