/* SPDX-License-Identifier: GPL-3.0-only */
/* USB MPE OUT for the FM-1 keyboard. It deliberately has no MIDI-IN bridge:
 * received MPE remains local. Channel numbers here are zero based. */
#define MPE_OUT_MAX 15u
typedef struct {
    uint16_t id;
    uint8_t channel, note, key, tone, track;
    uint8_t cc74_sent, cc74_pending, cc74_dirty;
} mpe_out_owner_t;
static mpe_out_owner_t mpe_out_owner[MPE_OUT_MAX];
static struct {
    uint16_t serial;
    /* All fields below cross the UI/main loop and the audio ISR. They are
     * byte-sized on AC79, so each published state transition is atomic. */
    volatile uint8_t request, members, setup, resync, retire, retire_members;
    uint8_t cc74_rr;
} mpe_out;
/* Experimental, runtime-only MPE OUT route.  It is deliberately private:
 * phase 5A has no UI, SMR1 extension or MPE-IN bridge.  Tests can enable it
 * directly; production starts with every route off. */
static uint8_t mpe_out_macro_cc74[NTRK];

static uint32_t mpe_out_packet(uint32_t cin, uint32_t status, uint32_t d1, uint32_t d2)
{ return cin | status << 8 | d1 << 16 | d2 << 24; }
static mpe_out_owner_t *mpe_out_find(uint16_t id)
{
    for (uint32_t i = 0; i < MPE_OUT_MAX; i++) if (mpe_out_owner[i].id == id) return &mpe_out_owner[i];
    return 0;
}
static void mpe_out_forget(void)
{
    memset(mpe_out_owner, 0, sizeof mpe_out_owner);
    mpe_out.cc74_rr = 0;
}
static void mpe_out_drop_keys(void) { memset(kb_mpe_out, 0, sizeof kb_mpe_out); }
static void mpe_out_configure(uint32_t members)
{
    if (members > MPE_OUT_MAX) return;
    mpe_out_forget();
    mpe_out_drop_keys();
    mpe_out.members = (uint8_t)members;                 /* zero is the default: conventional MIDI OUT */
    mpe_out.request = (uint8_t)members;
    mpe_out.setup = 0;
    mpe_out.resync = mpe_out.retire = mpe_out.retire_members = 0;
}
static int mpe_out_enabled(void) { return mpe_out.members != 0; }
/* While a reconfiguration is queued, suppress all new external notes.  The
 * local FM6 path remains independent and continues to sound normally. */
static int mpe_out_routing(void) { return mpe_out.members || mpe_out.request || mpe_out.retire; }
void mpe_out_request(uint32_t members)
{
    if (members <= MPE_OUT_MAX) {
        mpe_out.request = (uint8_t)members;
        RING_PUBLISH();
    }
}
uint32_t mpe_out_ui_members(void) { return mpe_out.request; }
int mpe_out_ui_pending(void)
{
    return mpe_out.request != mpe_out.members || mpe_out.retire || mpe_out.resync ||
           (mpe_out.request && mpe_out.setup < 3u + 4u * mpe_out.members);
}
static void mpe_out_retire(uint32_t old)
{
    mpe_out_forget();
    mpe_out_drop_keys();
    mpe_out.resync = 0;
    mpe_out.setup = 0;
    mpe_out.retire_members = (uint8_t)old;
    mpe_out.retire = old ? 1u : 0u;
    if (!old) mpe_out.members = mpe_out.request;
}
static void mpe_out_usb_reset(void)
{
    mpe_out_forget();
    mpe_out_drop_keys();
    if (mpe_out.members) { mpe_out.setup = 0; mpe_out.resync = 1; }
}
static int mpe_out_send(uint32_t status, uint32_t d1, uint32_t d2)
{ return midi_out_event(mpe_out_packet(status >> 4, status, d1, d2)); }

static uint32_t mpe_out_queue_used(void) { return mo_w - mo_r; }
static int mpe_out_room(uint32_t packets)
{ return usb.config && mpe_out_queue_used() <= MQ - packets; }

static int mpe_out_macro_cc74_active(const track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    return tr < NTRK && mpe_out_macro_cc74[tr] &&
           t->engine == ENGI_FM6 && t->eng_req == ENGI_FM6;
}

/* Test-only hook for phase 5A.  The complete user-facing assignment belongs
 * to a later phase, so this never persists and defaults to OFF. */
static void mpe_out_macro_cc74_enable(track_t *t, uint32_t enable)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr < NTRK) mpe_out_macro_cc74[tr] = enable != 0;
}

/* This reads the ISR-owned, already smoothed Smart Macro 1 value.  seq.c
 * calls the service in events_block(), before mix_part() calls smart_begin(),
 * so it sees the complete value from the preceding block on both serial and
 * CPU1 builds.  It does not read the UI shadow or introduce another slew. */
static uint32_t mpe_out_macro_cc74_value(uint32_t tr)
{
    int32_t value = tr < NTRK ? smart_value127(smart_macro[tr][0].value) : 0;
    /* TamaMPE transmits CC74=0 as neutral; external MPE instruments may map
     * that controller differently, so this is intentionally not a claim
     * about their audible default. */
    return value > 0 ? (uint32_t)value : 0u;
}
/* Send configuration one packet per audio block. This prevents a newly enabled
 * 15-member zone from monopolising the USB packet queue. */
static void mpe_out_service(void)
{
    if (mpe_out.request != mpe_out.members && !mpe_out.retire)
        mpe_out_retire(mpe_out.members);
    if (!usb.config) return;
    if (mpe_out.retire) {
        uint32_t p = mpe_out.retire++ - 1u, what;
        if (p <= mpe_out.retire_members) {             /* master and every old member: no stuck remote note */
            if (!mpe_out_send(0xb0u | p, 120, 0)) { mpe_out.retire--; return; }
            return;
        }
        p -= mpe_out.retire_members + 1u;
        if (p < 3u) {                                  /* master RPN 6 = zero: leave the old lower zone */
            what = p == 0u ? 101u << 8 : p == 1u ? 100u << 8 | 6u : 6u << 8;
            if (!mpe_out_send(0xb0u, what >> 8, what & 127u)) { mpe_out.retire--; return; }
            return;
        }
        mpe_out.retire = 0;
        mpe_out.members = mpe_out.request;             /* a later knob turn is handled by another safe transition */
        mpe_out.setup = mpe_out.resync = 0;
        return;
    }
    if (!mpe_out.members) return;
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
static uint16_t mpe_out_note_on_track(track_t *t, uint32_t key, uint32_t tone, uint32_t note, uint32_t velocity)
{
    if (mpe_out.request != mpe_out.members || !mpe_out_ready()) return 0;
    mpe_out_owner_t *o = 0;
    for (uint32_t i = 0; i < mpe_out.members; i++) if (!mpe_out_owner[i].id) { o = &mpe_out_owner[i]; break; }
    uint32_t tr = (uint32_t)(t - trk);
    int macro = mpe_out_macro_cc74_active(t);
    uint32_t cc74 = macro ? mpe_out_macro_cc74_value(tr) : 0u;
    /* A macro-routed note is all-or-nothing: CC74 must precede Note On and
     * both packets must fit before the owner becomes visible. */
    if (!o || (macro && (!mpe_out_room(2u) || usb.suspended))) return 0;
    if (macro && !mpe_out_send(0xb0u | (uint32_t)(o - mpe_out_owner + 1u), 74, cc74)) return 0;
    if (!mpe_out_send(0x90u | (uint32_t)(o - mpe_out_owner + 1u), note, velocity)) return 0;
    if (!++mpe_out.serial) ++mpe_out.serial;
    o->id = mpe_out.serial; o->channel = (uint8_t)(o - mpe_out_owner + 1u); o->note = (uint8_t)note;
    o->key = (uint8_t)key; o->tone = (uint8_t)tone; o->track = (uint8_t)tr;
    o->cc74_sent = (uint8_t)cc74; o->cc74_pending = (uint8_t)cc74; o->cc74_dirty = 0;
    return o->id;
}
/* Compatibility route for conventional callers and the pre-phase-5 tests. */
static uint16_t mpe_out_note_on(uint32_t key, uint32_t tone, uint32_t note, uint32_t velocity)
{ return mpe_out_note_on_track(&trk[song.sel % NTRK], key, tone, note, velocity); }
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

/* Low-priority phase 5A traffic.  It never resynchronises a zone after a
 * deferred value: the owner retains only the latest value and releases remain
 * free to use the rest of the 64-packet queue. */
static void mpe_out_macro_service(void)
{
    uint32_t sent = 0, n;
    if (!mpe_out_ready())
        return;
    /* Keep only the newest requested value while USB is busy, suspended or
     * disconnected.  This is bounded (15 owners), has no side effects on the
     * MIDI queue, and makes a resumed channel converge without a resync. */
    n = mpe_out.members;
    for (uint32_t at = 0; at < n; at++) {
        mpe_out_owner_t *o = &mpe_out_owner[at];
        uint32_t want;
        if (!o->id) continue;
        want = o->track < NTRK && mpe_out_macro_cc74[o->track] &&
               trk[o->track].engine == ENGI_FM6 && trk[o->track].eng_req == ENGI_FM6
             ? mpe_out_macro_cc74_value(o->track) : 0u;
        o->cc74_pending = (uint8_t)want;
        o->cc74_dirty = want != o->cc74_sent;
    }
    if (!usb.config || usb.suspended || mpe_out_queue_used() >= 32u)
        return;
    for (uint32_t step = 0; step < n && sent < 4u && mpe_out_queue_used() < 32u; step++) {
        uint32_t at = (mpe_out.cc74_rr + step) % n;
        mpe_out_owner_t *o = &mpe_out_owner[at];
        if (!o->id || !o->cc74_dirty) continue;
        if (!mpe_out_send(0xb0u | o->channel, 74, o->cc74_pending)) break;
        o->cc74_sent = o->cc74_pending;
        o->cc74_dirty = 0;
        sent++;
    }
    mpe_out.cc74_rr = (uint8_t)((mpe_out.cc74_rr + 1u) % n);
}
static void mpe_out_panic(void)
{
    if (!mpe_out_enabled()) return;
    mpe_out_forget(); mpe_out_drop_keys(); mpe_out.resync = 1;
}
