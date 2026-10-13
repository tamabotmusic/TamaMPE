/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by midi_control.c. CPU0/event context; USB and TRS share this zone.
 * Zero-initialized/off by default. RPN 6 on channel 1 binds the selected FM6
 * POLY track until reconfiguration. No expression, recording or note transforms.
 * FIFO per (channel, original note), including stolen notes. Panic/reconfiguration
 * starts a new session: MIDI 1.0 cannot identify an old matching Note Off then. */
typedef struct {
    note_id_t id;
    uint8_t channel, note, track, released;
} mpe_owner_t;
static mpe_owner_t mpe_owner[NOTE_ID_CAP];
static struct {
    uint16_t pedals;             /* member pedal bits; master pedal is separate */
    uint8_t members, track, master_pedal, lgen, transpose;
} mpe_zone;

static int mpe_member(uint32_t ch) { return ch && ch <= mpe_zone.members; }
static int mpe_sustained(uint32_t ch)
{
    return mpe_zone.master_pedal || ((mpe_zone.pedals >> ch) & 1u);
}
static void mpe_end(mpe_owner_t *o, int silence)
{
    if (!o->id) return;
    if (silence) {
        track_t *t = &trk[o->track];
        for (uint32_t i = 0; i < NVOICE; i++)
            if (t->v[i].note_id == o->id && t->v[i].active) voice_kill(&t->v[i]);
    }
    note_identity_end(o->id);
    o->id = 0;
}
static void mpe_clear(int silence)
{
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) mpe_end(&mpe_owner[i], silence);
    mpe_zone.pedals = mpe_zone.master_pedal = 0;
}
static void mpe_forget_track(uint32_t track)
{
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++)
        if (mpe_owner[i].id && mpe_owner[i].track == track) mpe_end(&mpe_owner[i], 1);
    if (mpe_zone.members && mpe_zone.track == track)
        mpe_zone.pedals = mpe_zone.master_pedal = 0;
}
static int mpe_track_held(uint32_t track)
{
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++)
        if (mpe_owner[i].id && mpe_owner[i].track == track) return 1;
    return 0;
}
static int mpe_playable(const track_t *t)
{
    return t->engine == ENGI_FM6 && t->eng_req == ENGI_FM6 &&
           trk_vmode(t) == V_POLY && !t->xf_on && !t->p[P_AMODE] &&
           t->p[P_QUANT] == Q_OFF && !micro_active(t);
}
static void mpe_maintenance(void)
{
    if (!mpe_zone.members) return;
    track_t *t = &trk[mpe_zone.track];
    uint8_t tr = mpe_zone.track, transpose = fm6_patch[tr][FP_TRNSP];
    if (!mpe_playable(t) || mpe_zone.lgen != fm6_lgen[tr] ||
        mpe_zone.transpose != transpose) mpe_clear(1);
    mpe_zone.lgen = fm6_lgen[tr];
    mpe_zone.transpose = transpose;
}
static void mpe_configure(uint32_t members)
{
    if (members > 15u) return; /* reject invalid counts, never extend beyond CH16 */
    mpe_clear(1);
    mpe_zone.members = 0;
    if (!members || !mpe_playable(TSEL)) return;
    /* Channels newly reserved for MPE must not strand conventional owners.
     * Preserve physical keys and conventional notes on all other channels. */
    for (uint32_t ch = 0; ch <= members; ch++) {
        for (uint32_t note = 0; note < 128u; note++)
            if (mn_get(ch, note)) midi_release(ch, note);
        midi_channel(ch)->pedal = 0;
    }
    mpe_zone.track = (uint8_t)trk_index(TSEL);
    mpe_zone.lgen = fm6_lgen[mpe_zone.track];
    mpe_zone.transpose = fm6_patch[mpe_zone.track][FP_TRNSP];
    mpe_zone.members = (uint8_t)members;
}
static void mpe_note(uint32_t ch, uint32_t note, uint32_t vel)
{
    if (!vel) {
        mpe_owner_t *oldest = 0;
        for (uint32_t i = 0; i < NOTE_ID_CAP; i++) {
            mpe_owner_t *o = &mpe_owner[i];
            if (o->id && !o->released && o->channel == ch && o->note == note &&
                (!oldest || o->id < oldest->id)) oldest = o;
        }
        if (oldest) {
            oldest->released = 1;
            if (!mpe_sustained(ch)) mpe_end(oldest, 0);
        }
        return;
    }
    track_t *t = &trk[mpe_zone.track];
    if (!mpe_playable(t)) return;
    mpe_owner_t *o = 0;
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++)
        if (!mpe_owner[i].id) { o = &mpe_owner[i]; break; }
    if (!o) { mpe_clear(1); return; } /* bounded overflow: reset session, drop this note */
    note_id_t id = note_identity_new(ch);
    if (!id) { mpe_clear(1); return; }
    if (!trk_note_on_id(t, note, vel, id)) { note_identity_end(id); return; }
    o->channel = (uint8_t)ch;
    o->note = (uint8_t)note;
    o->track = mpe_zone.track;
    o->released = 0;
    o->id = id;
}
static void mpe_pedal_up(uint32_t ch)
{
    if (!ch) mpe_zone.master_pedal = 0;
    else mpe_zone.pedals &= (uint16_t)~(1u << ch);
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) {
        mpe_owner_t *o = &mpe_owner[i];
        if (o->id && o->released && !mpe_sustained(o->channel)) mpe_end(o, 0);
    }
}
static int mpe_event(uint32_t st, uint32_t ch, uint32_t d1, uint32_t d2)
{
    if (!mpe_zone.members || (ch && !mpe_member(ch))) return 0;
    if (st == 0x90u || st == 0x80u) {
        if (ch) mpe_note(ch, d1, st == 0x90u ? d2 : 0);
    } else if (st == 0xB0u) {
        midi_channel_t *c = midi_channel(ch);
        switch (d1) {
        case 101: c->rpn_msb = (uint8_t)d2; break;
        case 100: c->rpn_lsb = (uint8_t)d2; break;
        case 99: case 98: c->rpn_msb = c->rpn_lsb = 127; break;
        case 6:
            if (!ch && !c->rpn_msb && c->rpn_lsb == 6u) mpe_configure(d2);
            break;
        case 64:
            if (d2 < 64u) mpe_pedal_up(ch);
            else if (!ch) mpe_zone.master_pedal = 1;
            else mpe_zone.pedals |= (uint16_t)(1u << ch);
            break;
        case 120: case 123:
            for (uint32_t i = 0; i < NOTE_ID_CAP; i++) {
                mpe_owner_t *o = &mpe_owner[i];
                if (!o->id || (ch && o->channel != ch)) continue;
                if (d1 == 120u) mpe_end(o, 1);
                else {
                    o->released = 1;
                    if (!mpe_sustained(o->channel)) mpe_end(o, 0);
                }
            }
            if (d1 == 120u) {
                if (!ch) mpe_zone.pedals = mpe_zone.master_pedal = 0;
                else mpe_zone.pedals &= (uint16_t)~(1u << ch);
            }
            break;
        case 121:
            c->rpn_msb = c->rpn_lsb = 127;
            if (!ch) mpe_zone.pedals = 0;
            mpe_pedal_up(ch);
            break;
        default: break;
        }
    }
    /* Reserved zone channels cannot edit shared patches/controllers. Bend,
     * pressure, CC74 and all other unsupported controls are consumed here. */
    return 1;
}
