/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by midi_control.c. CPU0/event context; USB and TRS share this zone.
 * Zero-initialized/off by default. RPN 6 on channel 1 binds the selected FM6
 * POLY track until reconfiguration. Bend/local pressure; no recording or transforms.
 * FIFO per (channel, original note), including stolen notes. Panic/reconfiguration
 * starts a new session: MIDI 1.0 cannot identify an old matching Note Off then. */
typedef struct {
    note_id_t id;
    uint8_t channel, note, track, released;
} mpe_owner_t;
static mpe_owner_t mpe_owner[NOTE_ID_CAP];
typedef struct {
    note_id_t latest;            /* never fall back to an older instance on channel reuse */
    int16_t raw;
    uint8_t semis, cents;
} mpe_bend_channel_t;
static mpe_bend_channel_t mpe_bend_ch[16];
static uint8_t mpe_pressure_ch[16]; /* initialized to neutral at session start */
static int32_t mpe_master_bend;  /* resolved master contribution to this session */
static struct {
    uint16_t pedals;             /* member pedal bits; master pedal is separate */
    uint8_t members, track, master_pedal, lgen, transpose;
} mpe_zone;

static int mpe_member(uint32_t ch) { return ch && ch <= mpe_zone.members; }
static uint16_t mpe_pressure_level(uint32_t value)
{
    return (uint16_t)((value * 32768u + 63u) / 127u);
}
static void mpe_pressure_update(uint32_t ch, uint32_t value)
{
    if (!ch) return; /* no master pressure policy yet; never feed shared DX7 AT */
    mpe_pressure_ch[ch] = (uint8_t)value;
    note_id_t id = mpe_bend_ch[ch].latest;
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) {
        mpe_owner_t *o = &mpe_owner[i];
        if (!id || o->id != id || o->released) continue;
        track_t *t = &trk[o->track];
        for (uint32_t k = 0; k < NVOICE; k++) {
            voice_t *v = &t->v[k];
            if (v->note_id == id && v->active && v->gate) {
                v->mpe_pressure = (uint8_t)value;
                v->mpe_pressure_target = mpe_pressure_level(value);
            }
        }
        break;
    }
}
static int32_t mpe_bend_value(uint32_t ch)
{
    const mpe_bend_channel_t *c = &mpe_bend_ch[ch];
    if (!c->raw) return 0;
    uint32_t cents = (uint32_t)c->semis * 100u + c->cents;
    uint32_t raw = (uint32_t)(c->raw < 0 ? -(int32_t)c->raw : c->raw);
    uint32_t denominator = (c->raw < 0 ? 8192u : 8191u) * 1200u;
    uint32_t product = raw * cents; /* <= 104849408, safely fits 32 bits */
    uint32_t value = product / denominator, rem = product % denominator;
    /* Exact Q24 quotient without a 64-bit division runtime (absent on pi32v2).
     * rem*2 < 19660800. Bounded work only when a MIDI bend/RPN arrives. */
    for (uint32_t bit = 0; bit < 24u; bit++) {
        value <<= 1;
        rem <<= 1;
        if (rem >= denominator) { rem -= denominator; value |= 1u; }
    }
    return c->raw < 0 ? -(int32_t)value : (int32_t)value;
}
static void mpe_bend_update(uint32_t ch)
{
    int32_t bend = mpe_bend_value(ch);
    track_t *t = &trk[mpe_zone.track];
    if (!ch) {
        int32_t delta = bend - mpe_master_bend;
        mpe_master_bend = bend;
        for (uint32_t i = 0; i < NVOICE; i++)
            if (t->v[i].mpe_bend == 2 && t->v[i].active && t->v[i].stage != 4u)
                t->v[i].mpe_bend_target += delta;
        return;
    }
    /* Only the latest still-keyed instance receives member expression. Earlier
     * held notes and sustain/release tails keep their own resolved snapshot. */
    note_id_t id = mpe_bend_ch[ch].latest;
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) {
        mpe_owner_t *o = &mpe_owner[i];
        if (o->id != id || !id || o->released) continue;
        for (uint32_t v = 0; v < NVOICE; v++)
            if (t->v[v].note_id == id && t->v[v].active && t->v[v].gate)
                t->v[v].mpe_bend_target = bend + mpe_master_bend;
        break;
    }
}
static void mpe_freeze_track(uint32_t track)
{
    for (uint32_t i = 0; i < NVOICE; i++)
        if (trk[track].v[i].mpe_bend == 2) trk[track].v[i].mpe_bend = 1;
}
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
    if (mpe_zone.members) mpe_freeze_track(mpe_zone.track);
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) mpe_end(&mpe_owner[i], silence);
    mpe_zone.pedals = mpe_zone.master_pedal = 0;
    mpe_master_bend = 0;
    for (uint32_t ch = 0; ch < 16u; ch++) {
        mpe_bend_ch[ch].latest = mpe_bend_ch[ch].raw = 0;
        mpe_pressure_ch[ch] = 0;
    }
}
static void mpe_forget_track(uint32_t track)
{
    mpe_freeze_track(track);
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++)
        if (mpe_owner[i].id && mpe_owner[i].track == track) mpe_end(&mpe_owner[i], 1);
    if (mpe_zone.members && mpe_zone.track == track) mpe_clear(1);
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
    for (uint32_t ch = 0; ch < 16u; ch++) {
        mpe_bend_ch[ch].semis = ch ? 48 : 2; /* MPE initial member/master ranges */
        mpe_bend_ch[ch].cents = 0;
    }
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
    /* A full bookkeeping table must never interrupt the established session.
     * Drop only this untracked Note On; its later Note Off is harmless. */
    if (!o) return;
    note_id_t id = note_identity_new(ch);
    if (!id) return; /* registry exhaustion follows the same safe drop policy */
    if (!trk_note_on_id(t, note, vel, id)) { note_identity_end(id); return; }
    o->channel = (uint8_t)ch;
    o->note = (uint8_t)note;
    o->track = mpe_zone.track;
    o->released = 0;
    o->id = id;
    mpe_bend_ch[ch].latest = id;
    int32_t bend = mpe_bend_value(ch) + mpe_master_bend;
    for (uint32_t i = 0; i < NVOICE; i++)
        if (t->v[i].note_id == id) {
            t->v[i].mpe_bend = 2;
            t->v[i].mpe_bend_q24 = t->v[i].mpe_bend_target = bend;
            t->v[i].mpe_pressure = mpe_pressure_ch[ch];
            t->v[i].mpe_pressure_level = t->v[i].mpe_pressure_target = mpe_pressure_level(mpe_pressure_ch[ch]);
        }
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
    } else if (st == 0xE0u) {
        mpe_bend_ch[ch].raw = (int16_t)((int32_t)(d1 | (d2 << 7)) - 8192);
        mpe_bend_update(ch);
    } else if (st == 0xD0u) {
        mpe_pressure_update(ch, d1);
    } else if (st == 0xB0u) {
        midi_channel_t *c = midi_channel(ch);
        switch (d1) {
        case 101: c->rpn_msb = (uint8_t)d2; break;
        case 100: c->rpn_lsb = (uint8_t)d2; break;
        case 99: case 98: c->rpn_msb = c->rpn_lsb = 127; break;
        case 6: case 38:
            if (!ch && d1 == 6u && !c->rpn_msb && c->rpn_lsb == 6u) mpe_configure(d2);
            else if (!c->rpn_msb && !c->rpn_lsb) {
                if (d1 == 6u) mpe_bend_ch[ch].semis = (uint8_t)d2;
                else mpe_bend_ch[ch].cents = (uint8_t)(d2 > 99u ? 99u : d2);
                mpe_bend_update(ch);
            }
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
            mpe_pressure_update(ch, 0); /* member neutral; released snapshots remain frozen */
            mpe_bend_ch[ch].raw = 0;
            mpe_bend_update(ch); /* centre applicable bend, retain sensitivity */
            c->rpn_msb = c->rpn_lsb = 127;
            if (!ch) mpe_zone.pedals = 0;
            mpe_pedal_up(ch);
            break;
        default: break;
        }
    }
    /* Reserved zone channels cannot edit shared patches/controllers. CC74 and
     * all other unsupported controls are consumed here; master pressure ignored. */
    return 1;
}
