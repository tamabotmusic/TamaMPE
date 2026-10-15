/* SPDX-License-Identifier: GPL-3.0-only */
/* USB MPE OUT for the FM-1 keyboard. It deliberately has no MIDI-IN bridge:
 * received MPE remains local. Channel numbers here are zero based. */
#define MPE_OUT_MAX 15u
typedef struct {
    uint16_t id;
    uint16_t bend_sent, bend_pending;
    uint8_t channel, note, key, tone, track;
    uint8_t cc74_sent, cc74_pending, pressure_sent, pressure_pending, dirty;
} mpe_out_owner_t;
_Static_assert(sizeof(mpe_out_owner_t) == 16u, "compact MPE OUT owner");
static mpe_out_owner_t mpe_out_owner[MPE_OUT_MAX];
static uint8_t mpe_out_channel_dirty[MPE_OUT_MAX];
static struct {
    uint16_t serial;
    /* All fields below cross the UI/main loop and the audio ISR. They are
     * byte-sized on AC79, so each published state transition is atomic. */
    volatile uint8_t request, members, setup, resync, retire, retire_members;
    uint8_t expr_rr;
} mpe_out;

#define MPE_EXPR_CC74 0u
#define MPE_EXPR_PRESSURE 1u
#define MPE_EXPR_BEND 2u
#define MPE_EXPR_N 3u
#define MPE_EXPR_DIRTY(n) (1u << (n))
#define MPE_EXPR_USED 0x40u
#define MPE_EXPR_MUTED 0x80u

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
    memset(mpe_out_channel_dirty, 0, sizeof mpe_out_channel_dirty);
    mpe_out.expr_rr = 0;
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

/* Compatibility hook for phase-5A tests.  It remains runtime-only and maps
 * Macro 1 to the phase-5B external assignment format. */
static void mpe_out_macro_cc74_enable(track_t *t, uint32_t enable)
{
    smart_mpe_out_map_test_config(t, 0, enable ? SMART_MPE_OUT_CC74 : SMART_MPE_OUT_OFF,
                                  127, 0, 127, SMART_CURVE_LINEAR);
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
static uint16_t mpe_out_expr_sent(const mpe_out_owner_t *o, uint32_t which)
{
    if (which == MPE_EXPR_CC74) return o->cc74_sent;
    if (which == MPE_EXPR_PRESSURE) return o->pressure_sent;
    return o->bend_sent;
}
static uint16_t mpe_out_expr_pending(const mpe_out_owner_t *o, uint32_t which)
{
    if (which == MPE_EXPR_CC74) return o->cc74_pending;
    if (which == MPE_EXPR_PRESSURE) return o->pressure_pending;
    return o->bend_pending;
}
static void mpe_out_expr_set_sent(mpe_out_owner_t *o, uint32_t which, uint16_t value)
{
    if (which == MPE_EXPR_CC74) o->cc74_sent = (uint8_t)value;
    else if (which == MPE_EXPR_PRESSURE) o->pressure_sent = (uint8_t)value;
    else o->bend_sent = value;
}
static void mpe_out_expr_set_pending(mpe_out_owner_t *o, uint32_t which, uint16_t value)
{
    if (which == MPE_EXPR_CC74) o->cc74_pending = (uint8_t)value;
    else if (which == MPE_EXPR_PRESSURE) o->pressure_pending = (uint8_t)value;
    else o->bend_pending = value;
}
static uint16_t mpe_out_expr_neutral(uint32_t which)
{ return which == MPE_EXPR_BEND ? 8192u : 0u; }
static int mpe_out_expr_send(const mpe_out_owner_t *o, uint32_t which, uint16_t value)
{
    if (which == MPE_EXPR_CC74) return mpe_out_send(0xb0u | o->channel, 74, value);
    if (which == MPE_EXPR_PRESSURE) return mpe_out_send(0xd0u | o->channel, value, 0);
    return mpe_out_send(0xe0u | o->channel, value & 127u, value >> 7);
}
static int mpe_out_expr_non_neutral(const mpe_out_owner_t *o)
{
    for (uint32_t i = 0; i < MPE_EXPR_N; i++)
        if (mpe_out_expr_sent(o, i) != mpe_out_expr_neutral(i) ||
            mpe_out_expr_pending(o, i) != mpe_out_expr_neutral(i)) return 1;
    return 0;
}
static uint16_t mpe_out_note_on_track(track_t *t, uint32_t key, uint32_t tone, uint32_t note, uint32_t velocity)
{
    if (mpe_out.request != mpe_out.members || !mpe_out_ready()) return 0;
    mpe_out_owner_t *o = 0;
    for (uint32_t i = 0; i < mpe_out.members; i++) if (!mpe_out_owner[i].id) { o = &mpe_out_owner[i]; break; }
    uint32_t tr = (uint32_t)(t - trk);
    uint16_t expr[MPE_EXPR_N];
    uint32_t oi, prepared = smart_mpe_out_values(t, expr);
    if (!o) return 0;
    oi = (uint32_t)(o - mpe_out_owner);
    /* A programmed note, or a channel left non-neutral by a released note,
     * is prepared atomically: CC74, pressure, bend, then Note On. */
    if (prepared || mpe_out_channel_dirty[oi]) {
        if (!mpe_out_room(4u) || usb.suspended) return 0;
        /* If USB is torn down between packets, do not publish an owner and
         * retain the cleanup obligation for the next use of this channel. */
        mpe_out_channel_dirty[oi] = 1u;
        if (!prepared) { expr[0] = expr[1] = 0; expr[2] = 8192; }
        for (uint32_t i = 0; i < MPE_EXPR_N; i++)
            if (!mpe_out_send(i == MPE_EXPR_CC74 ? 0xb0u | (oi + 1u) :
                              i == MPE_EXPR_PRESSURE ? 0xd0u | (oi + 1u) : 0xe0u | (oi + 1u),
                              i == MPE_EXPR_CC74 ? 74u : i == MPE_EXPR_BEND ? expr[i] & 127u : expr[i],
                              i == MPE_EXPR_CC74 ? expr[i] : i == MPE_EXPR_BEND ? expr[i] >> 7 : 0u)) return 0;
    }
    if (!mpe_out_send(0x90u | (uint32_t)(o - mpe_out_owner + 1u), note, velocity)) return 0;
    if (!++mpe_out.serial) ++mpe_out.serial;
    o->id = mpe_out.serial; o->channel = (uint8_t)(o - mpe_out_owner + 1u); o->note = (uint8_t)note;
    o->key = (uint8_t)key; o->tone = (uint8_t)tone; o->track = (uint8_t)tr;
    o->cc74_sent = (uint8_t)expr[0]; o->cc74_pending = (uint8_t)expr[0];
    o->pressure_sent = (uint8_t)expr[1]; o->pressure_pending = (uint8_t)expr[1];
    o->bend_sent = expr[2]; o->bend_pending = expr[2];
    o->dirty = prepared ? MPE_EXPR_USED : 0u;
    mpe_out_channel_dirty[oi] = 0;
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
    if (mpe_out_expr_non_neutral(o)) mpe_out_channel_dirty[o - mpe_out_owner] = 1u;
    o->id = 0;
}
static void mpe_out_bend(uint16_t id, uint32_t value)
{
    mpe_out_owner_t *o = mpe_out_find(id); if (!o) return;
    value = value > 16383u ? 16383u : value;
    if (!mpe_out_expr_send(o, MPE_EXPR_BEND, value)) mpe_out.resync = 1;
    else { o->bend_sent = o->bend_pending = value; }
}
static void mpe_out_pressure(uint16_t id, uint32_t value)
{
    mpe_out_owner_t *o = mpe_out_find(id); if (!o) return;
    value = value > 127u ? 127u : value;
    if (!mpe_out_expr_send(o, MPE_EXPR_PRESSURE, value)) mpe_out.resync = 1;
    else { o->pressure_sent = o->pressure_pending = (uint8_t)value; }
}
static void mpe_out_timbre(uint16_t id, uint32_t value)
{
    mpe_out_owner_t *o = mpe_out_find(id); if (!o) return;
    value = value > 127u ? 127u : value;
    if (!mpe_out_expr_send(o, MPE_EXPR_CC74, value)) mpe_out.resync = 1;
    else { o->cc74_sent = o->cc74_pending = (uint8_t)value; }
}

/* Low-priority expression traffic.  It is one shared, bounded scheduler: a
 * deferred value never asks for a zone resync, and Note Off / panic retain
 * priority because this runs after the normal keyboard and MPE services. */
static void mpe_out_macro_service(void)
{
    uint32_t sent = 0, n, slots;
    if (!mpe_out_ready())
        return;
    n = mpe_out.members;
    for (uint32_t at = 0; at < n; at++) {
        mpe_out_owner_t *o = &mpe_out_owner[at];
        uint16_t want[MPE_EXPR_N];
        if (!o->id) continue;
        if ((o->dirty & MPE_EXPR_MUTED) || o->track >= NTRK ||
            trk[o->track].engine != ENGI_FM6 || trk[o->track].eng_req != ENGI_FM6 ||
            !smart_mpe_out_values(&trk[o->track], want)) {
            want[0] = want[1] = 0; want[2] = 8192;
            o->dirty |= MPE_EXPR_MUTED;
        }
        for (uint32_t i = 0; i < MPE_EXPR_N; i++) {
            mpe_out_expr_set_pending(o, i, want[i]);
            if (mpe_out_expr_sent(o, i) != want[i]) o->dirty |= MPE_EXPR_DIRTY(i);
            else o->dirty &= ~MPE_EXPR_DIRTY(i);
        }
    }
    if (!usb.config || usb.suspended || mpe_out_queue_used() >= 32u)
        return;
    slots = n * MPE_EXPR_N;
    for (uint32_t step = 0; step < slots && sent < 4u && mpe_out_queue_used() < 32u; step++) {
        uint32_t at = (mpe_out.expr_rr + step) % slots, oi = at / MPE_EXPR_N, which = at % MPE_EXPR_N;
        mpe_out_owner_t *o = &mpe_out_owner[oi];
        if (!o->id || !(o->dirty & MPE_EXPR_DIRTY(which))) continue;
        if (!mpe_out_expr_send(o, which, mpe_out_expr_pending(o, which))) break;
        mpe_out_expr_set_sent(o, which, mpe_out_expr_pending(o, which));
        o->dirty &= ~MPE_EXPR_DIRTY(which);
        sent++;
    }
    mpe_out.expr_rr = (uint8_t)((mpe_out.expr_rr + 1u) % slots);
}
static void mpe_out_panic(void)
{
    if (!mpe_out_enabled()) return;
    mpe_out_forget(); mpe_out_drop_keys(); mpe_out.resync = 1;
}
