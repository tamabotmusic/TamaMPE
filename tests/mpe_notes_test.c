/* SPDX-License-Identifier: GPL-3.0-only */
/* Real USB/TRS queue -> events_block -> MPE owners -> FM6 voices. */
#define MIDI_CONTROL_NO_MAIN 1
#include "midi_control_test.c"
#include <assert.h>

static uint32_t owners(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) n += mpe_owner[i].id != 0;
    return n;
}
static uint32_t identities(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) n += note_ids[i] != 0;
    return n;
}
static note_id_t owner(uint32_t ch, uint32_t note)
{
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++)
        if (mpe_owner[i].id && mpe_owner[i].channel == ch && mpe_owner[i].note == note)
            return mpe_owner[i].id;
    return 0;
}
static voice_t *sounding(note_id_t id)
{
    if (!id) return NULL;
    for (uint32_t t = 0; t < NTRK; t++)
        for (uint32_t i = 0; i < NVOICE; i++)
            if (trk[t].v[i].note_id == id && trk[t].v[i].gate) return &trk[t].v[i];
    return NULL;
}
static int audible(void)
{
    int32_t pcm[CTL];
    int heard = 0;
    for (uint32_t block = 0; block < 32; block++) {
        mod_begin(&trk[0]);
        track_render(&trk[0], pcm, CTL);
        mod_end(&trk[0]);
        for (uint32_t i = 0; i < CTL; i++) heard |= pcm[i] != 0;
    }
    return heard;
}
static void configure(uint32_t n)
{
    queued(0xB0, 101, 0, 1); queued(0xB0, 100, 6, 1); queued(0xB0, 6, n, 1);
}
static void setup(void)
{
    mpe_clear(1);
    memset(&mpe_zone, 0, sizeof mpe_zone);
    midi_test_reset();
    set_engine_of(&trk[0], ENGI_FM6);
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_AMODE] = 0;
    trk[0].p[P_QUANT] = Q_OFF;
    events_block(CTL);
    assert(!owners() && !identities() && !mpe_zone.members);
    configure(3);
    assert(mpe_zone.members == 3 && mpe_zone.track == 0);
}
int main(void)
{
    setup();
    queued(0x91, 60, 100, 1); queued(0x92, 60, 110, 1);
    note_id_t a = owner(1, 60), b = owner(2, 60);
    assert(a && b && a != b && sounding(a) && sounding(b) && sounding(a) != sounding(b));
    assert(audible());
    song.sel = 1;
    queued(0x81, 60, 0, 1);
    assert(!owner(1, 60) && sounding(b) && owners() == 1);
    assert(audible());
    queued(0x81, 60, 0, 1); assert(sounding(b));
    queued(0x92, 62, 100, 1); note_id_t c = owner(2, 62);
    queued(0x82, 60, 0, 1); assert(sounding(c));
    queued(0x82, 62, 0, 1); assert(!owners() && !identities());
    puts("equal notes, independent/tardy release and fixed track ownership OK");

    setup();
    queued(0x91, 60, 100, 1); a = owner(1, 60);
    queued(0x91, 60, 100, 2); /* USB/TRS share channel state */
    b = 0;
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) if (mpe_owner[i].id != a && mpe_owner[i].id) b = mpe_owner[i].id;
    assert(b && sounding(a) && sounding(b));
    queued(0x81, 60, 0, 2); assert(!sounding(a) && sounding(b));
    queued(0x81, 60, 0, 1); assert(!owners() && !identities());
    puts("same-channel repeats use FIFO; USB/TRS ownership intentionally shared OK");

    setup();
    queued(0x91, 60, 100, 1); a = owner(1, 60);
    voice_kill(sounding(a));
    queued(0x91, 60, 100, 1); b = 0;
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) if (mpe_owner[i].id && mpe_owner[i].id != a) b = mpe_owner[i].id;
    queued(0x81, 60, 0, 1); assert(sounding(b) && !note_identity_valid(a));
    queued(0x81, 60, 0, 1); assert(!owners() && !identities());
    puts("stolen identities retain FIFO bookkeeping without releasing replacements OK");

    setup();
    queued(0xB0, 64, 127, 1);
    queued(0x91, 60, 100, 1); a = owner(1, 60);
    queued(0x81, 60, 0, 1); assert(sounding(a) && owners() == 1);
    queued(0xB1, 64, 127, 1); queued(0xB0, 64, 0, 1); assert(sounding(a));
    queued(0xB1, 121, 0, 1); assert(!owners() && !identities());
    queued(0x91, 60, 100, 1); queued(0x92, 61, 100, 1); b = owner(2, 61);
    queued(0xB1, 120, 0, 1); assert(owners() == 1 && sounding(b));
    queued(0xB0, 64, 127, 1); queued(0xB0, 123, 0, 1); assert(sounding(b));
    queued(0xB0, 121, 0, 1); assert(!owners() && !identities());
    puts("master/member sustain and scoped CC120/121/123 cleanup OK");

    setup();
    queued(0x91, 60, 100, 1);
    panic_req |= 1; events_block(CTL); assert(!owners() && !identities());
    queued(0x91, 60, 100, 1); queued(0x81, 60, 0, 1);
    assert(!owners()); /* accepted post-panic matching old/new Note Off ambiguity */
    queued(0x91, 60, 100, 1); configure(0); assert(!mpe_zone.members && !owners() && !identities());
    song.g[G_ROUTE] = 1; queued(0x91, 60, 100, 1); assert(gate_note(&trk[0], 60) && !owners());
    queued(0x81, 60, 0, 1);
    setup(); queued(0x91, 60, 100, 1); midi_in_overflow = 1; events_block(CTL);
    assert(!owners() && !identities());
    puts("panic, disable, overflow and return to conventional routing OK");

    setup();
    fm1_in.notes = 1u; events_block(CTL); uint32_t local = kb_note[0];
    queued(0x91, local, 100, 1); a = owner(1, local); assert(sounding(a));
    fm1_in.notes = 0; events_block(CTL); assert(sounding(a));
    queued(0x94, 65, 100, 1); queued(0xB4, 123, 0, 1); assert(sounding(a));
    queued(0xB4, 120, 0, 1); assert(!owners() && !identities());
    puts("physical keyboard release and conventional CC123 preserve MPE; track CC120 cleans it OK");

    setup(); queued(0x91, 60, 100, 1);
    fm6_put_patch(0, fm6_patch[0], 1); events_block(CTL); assert(!owners() && !identities());
    queued(0x91, 60, 100, 1); set_engine_of(&trk[0], HOST_SYNTH); events_block(CTL);
    assert(!owners() && !identities());
    queued(0x91, 62, 100, 1); assert(!owners());
    puts("preset reload and engine changes retire all affected identities OK");

    setup();
    int16_t params[NTRK][P_COUNT];
    uint8_t patch[sizeof fm6_patch]; memcpy(patch, fm6_patch, sizeof patch);
    for (uint32_t tr = 0; tr < NTRK; tr++) memcpy(params[tr], trk[tr].p, sizeof params[tr]);
    song.sel = 1; /* legacy channel routing would reach another track */
    queued(0xE1, 127, 127, 1); queued(0xD1, 127, 0, 1);
    queued(0xB1, 74, 127, 1); queued(0xB1, 1, 127, 1); queued(0xB1, 73, 127, 1);
    assert(!memcmp(patch, fm6_patch, sizeof patch));
    for (uint32_t tr = 0; tr < NTRK; tr++) {
        assert(!memcmp(params[tr], trk[tr].p, sizeof params[tr]));
        assert(!trk[tr].bend_raw && !trk[tr].at && !trk[tr].mw && !midi_bend_target[tr]);
    }
    song.sel = 0;
    queued(0x94, 64, 100, 1); assert(gate_note(&trk[0], 64) && !owners());
    queued(0x84, 64, 0, 1);
    configure(16); assert(mpe_zone.members == 3);
    queued(0xB1, 101, 0, 1); queued(0xB1, 100, 6, 1); queued(0xB1, 6, 15, 1);
    assert(mpe_zone.members == 3); configure(15); assert(mpe_zone.members == 15);
    queued(0x9F, 65, 100, 1); assert(owner(15, 65));
    configure(1); assert(!owners() && !identities() && mpe_zone.members == 1);
    puts("expression suppression, channel boundaries and master-only RPN6 OK");

    setup();
    trk[0].p[P_AMODE] = 1; queued(0x91, 60, 100, 1); assert(!owners());
    trk[0].p[P_AMODE] = 0; trk[0].p[P_QUANT] = Q_SNAP;
    queued(0x91, 60, 100, 1); assert(!owners());
    trk[0].p[P_QUANT] = Q_OFF; trk[0].p[P_CHRD] = 1;
    song.rec = 1; queued(0x91, 60, 100, 1); assert(owners() == 1 && !live_held[0][60 >> 5]);
    assert(mpe_owner[0].note == 60); mpe_clear(1);
    setup();
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) queued(0x91, 60, 100, 1);
    assert(owners() == NOTE_ID_CAP && identities() == NOTE_ID_CAP);
    queued(0x91, 60, 100, 1); assert(!owners() && !identities());
    queued(0x91, 61, 100, 1); assert(owners() == 1); mpe_clear(1);
    puts("safe transform/recording restrictions and bounded saturation cleanup OK");
    printf("MPE ownership RAM: %zu + zone %zu bytes; identity registry %zu + counter %zu\n",
           sizeof mpe_owner, sizeof mpe_zone, sizeof note_ids, sizeof note_serial);
    return 0;
}
