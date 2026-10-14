/* SPDX-License-Identifier: GPL-3.0-only */
/* USB MPE OUT for the FM-1 keyboard. It deliberately has no MIDI-IN bridge:
 * received MPE remains local. Channel numbers here are zero based. */
#define MPE_OUT_MAX 15u
typedef struct {
    uint16_t id;
    uint8_t channel, note, key, tone;
} mpe_out_owner_t;
static mpe_out_owner_t mpe_out_owner[MPE_OUT_MAX];
static struct {
    uint16_t serial;
    uint8_t members, setup, resync;
} mpe_out;

static uint32_t mpe_out_packet(uint32_t cin, uint32_t status, uint32_t d1, uint32_t d2)
{ return cin | status << 8 | d1 << 16 | d2 << 24; }
static mpe_out_owner_t *mpe_out_find(uint16_t id)
{
    for (uint32_t i = 0; i < MPE_OUT_MAX; i++) if (mpe_out_owner[i].id == id) return &mpe_out_owner[i];
    return 0;
}
static void mpe_out_forget(void) { memset(mpe_out_owner, 0, sizeof mpe_out_owner); }
static void mpe_out_configure(uint32_t members)
{
    if (members > MPE_OUT_MAX) return;
    mpe_out_forget();
    memset(kb_mpe_out, 0, sizeof kb_mpe_out);
    mpe_out.members = (uint8_t)members;                 /* zero is the default: conventional MIDI OUT */
    mpe_out.setup = 0;
    mpe_out.resync = 0;
}
static int mpe_out_enabled(void) { return mpe_out.members != 0; }
static void mpe_out_usb_reset(void)
{
    mpe_out_forget();
    memset(kb_mpe_out, 0, sizeof kb_mpe_out);
    if (mpe_out.members) { mpe_out.setup = 0; mpe_out.resync = 1; }
}
static int mpe_out_send(uint32_t status, uint32_t d1, uint32_t d2)
{ return midi_out_event(mpe_out_packet(status >> 4, status, d1, d2)); }
/* Send configuration one packet per audio block. This prevents a newly enabled
 * 15-member zone from monopolising the USB packet queue. */
static void mpe_out_service(void)
{
    if (!mpe_out.members || !usb.config) return;
    if (mpe_out.resync) {
        uint32_t ch = mpe_out.resync - 1u;
        if (!mpe_out_send(0xb0u | ch, 120, 0)) return;  /* all sound off is safe after a lost packet */
        if (++mpe_out.resync > mpe_out.members + 1u) mpe_out.resync = 0;
        return;
    }
    if (mpe_out.setup < 3u + 4u * mpe_out.members) {
        uint32_t p = mpe_out.setup++, ch, what;
        if (p == 0u) what = 101u << 8;                 /* master: RPN 6, lower-zone member count */
        else if (p == 1u) what = 100u << 8 | 6u;
        else if (p == 2u) what = 6u << 8 | mpe_out.members;
        else {
            p -= 3u; ch = p / 4u + 1u;
            switch (p & 3u) {
            case 0: what = 101u << 8; break;
            case 1: what = 100u << 8; break;
            case 2: what = 6u << 8 | 48u; break;       /* MPE default member bend: +/-48 semitones */
            default: what = 38u << 8; break;
            }
            if (!mpe_out_send(0xb0u | ch, what >> 8, what & 127u)) { mpe_out.setup--; return; }
            return;
        }
        if (!mpe_out_send(0xb0u, what >> 8, what & 127u)) mpe_out.setup--;
    }
}
static int mpe_out_ready(void) { return mpe_out_enabled() && !mpe_out.resync && mpe_out.setup >= 3u + 4u * mpe_out.members; }
static uint16_t mpe_out_note_on(uint32_t key, uint32_t tone, uint32_t note, uint32_t velocity)
{
    if (!mpe_out_ready()) return 0;
    mpe_out_owner_t *o = 0;
    for (uint32_t i = 0; i < mpe_out.members; i++) if (!mpe_out_owner[i].id) { o = &mpe_out_owner[i]; break; }
    if (!o || !mpe_out_send(0x90u | (uint32_t)(o - mpe_out_owner + 1u), note, velocity)) return 0;
    if (!++mpe_out.serial) ++mpe_out.serial;
    o->id = mpe_out.serial; o->channel = (uint8_t)(o - mpe_out_owner + 1u); o->note = (uint8_t)note;
    o->key = (uint8_t)key; o->tone = (uint8_t)tone;
    return o->id;
}
static void mpe_out_note_off(uint16_t id)
{
    mpe_out_owner_t *o = mpe_out_find(id);
    if (!o) return;
    if (!mpe_out_send(0x80u | o->channel, o->note, 0)) mpe_out.resync = 1;
    o->id = 0;
}
static void mpe_out_bend(uint16_t id, uint32_t value)
{
    mpe_out_owner_t *o = mpe_out_find(id); if (!o) return;
    value = value > 16383u ? 16383u : value;
    if (!mpe_out_send(0xe0u | o->channel, value & 127u, value >> 7)) mpe_out.resync = 1;
}
static void mpe_out_pressure(uint16_t id, uint32_t value)
{
    mpe_out_owner_t *o = mpe_out_find(id); if (!o) return;
    if (!mpe_out_send(0xd0u | o->channel, value > 127u ? 127u : value, 0)) mpe_out.resync = 1;
}
static void mpe_out_timbre(uint16_t id, uint32_t value)
{
    mpe_out_owner_t *o = mpe_out_find(id); if (!o) return;
    if (!mpe_out_send(0xb0u | o->channel, 74, value > 127u ? 127u : value)) mpe_out.resync = 1;
}
static void mpe_out_panic(void)
{
    if (!mpe_out_enabled()) return;
    mpe_out_forget(); mpe_out.resync = 1;
}
