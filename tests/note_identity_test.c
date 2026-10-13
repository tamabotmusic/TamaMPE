/* SPDX-License-Identifier: GPL-3.0-only */
/* Opt-in instance ownership through the real allocator, without MPE controls. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>

static voice_t *owned(track_t *t, note_id_t id)
{
    for (uint32_t i = 0; i < NVOICE; i++)
        if (t->v[i].note_id == id && t->v[i].active && t->v[i].gate) return &t->v[i];
    return NULL;
}
static void reset_part(track_t *t, uint32_t engine)
{
    trk_all_off(t);
    memset(t->v, 0, sizeof t->v);
    host_preset(t, engine, 0);
    t->p[P_VOICE] = V_POLY;
    t->p[P_MUTE] = 0;
    t->rr = 0;
}
int main(void)
{
    host_tracks_init();
    track_t *t = &trk[0];
    const uint32_t engines[] = {HOST_SYNTH, ENGI_FM6};
    for (uint32_t e = 0; e < 2; e++) {
        reset_part(t, engines[e]);
        note_id_t a = note_identity_new(1), b = note_identity_new(2);
        assert(a && b && a != b && (a & 15u) == 1 && (b & 15u) == 2);
        assert(trk_note_on_id(t, 60, 100, a));
        assert(trk_note_on_id(t, 60, 100, b));
        assert(owned(t, a) && owned(t, b) && owned(t, a) != owned(t, b));
        assert(!trk_note_on_id(t, 60, 100, a));
        trk_note_off(t, 60); /* pitch-only release cannot release identified notes */
        assert(owned(t, a) && owned(t, b));
        note_identity_end(a);
        assert(!owned(t, a) && owned(t, b));
        assert(!trk_note_on_id(t, 60, 100, a)); /* retired handle cannot attach */
        note_identity_end(b);
        printf("engine %u: equal pitches coexist and release independently OK\n", engines[e]);
    }
    reset_part(t, ENGI_FM6);
    note_id_t old = note_identity_new(3);
    assert(trk_note_on_id(t, 60, 100, old));
    voice_t *slot = owned(t, old);
    voice_kill(slot);
    assert(!slot->note_id);
    mod_note(t, 60, 100);
    voice_start(t, slot, 60, 100, 0); /* explicit restart of the slot that was killed */
    assert(slot->gate && !slot->note_id);
    note_identity_end(old);
    assert(slot->gate);
    puts("budget steal: old handle cannot release replacement OK");

    reset_part(t, ENGI_FM6);
    note_id_t ids[NVOICE + 1];
    for (uint32_t i = 0; i <= NVOICE; i++) {
        ids[i] = note_identity_new(4);
        assert(ids[i] && trk_note_on_id(t, 60 + i, 100, ids[i]));
    }
    note_id_t stolen = 0;
    for (uint32_t i = 0; i < NVOICE; i++) if (!owned(t, ids[i])) { stolen = ids[i]; break; }
    assert(stolen && owned(t, ids[NVOICE]));
    note_identity_end(stolen);
    assert(owned(t, ids[NVOICE]));
    for (uint32_t i = 0; i <= NVOICE; i++) note_identity_end(ids[i]);
    puts("allocator steal: stale release preserves substitute OK");

    reset_part(t, ENGI_FM6);
    reset_part(&trk[1], HOST_SYNTH);
    note_id_t layer = note_identity_new(5);
    assert(trk_note_on_id(t, 64, 100, layer));
    assert(trk_note_on_id(&trk[1], 64, 100, layer));
    trk_note_off_id(t, layer);
    assert(!owned(t, layer) && owned(&trk[1], layer));
    note_identity_end(layer);
    assert(!owned(&trk[1], layer));
    puts("one logical identity can own voices on two tracks OK");

    reset_part(t, HOST_SYNTH);
    note_id_t legato = note_identity_new(7);
    assert(trk_note_on_id(t, 60, 100, legato));
    voice_t *moving = owned(t, legato);
    t->p[P_VOICE] = V_LEGATO;
    mono_play(t, 62, 100, 0, 0);
    note_identity_end(legato);
    assert(moving->gate && moving->note == 62 && !moving->note_id);
    puts("conventional legato invalidates the former slot association OK");

    reset_part(t, HOST_SYNTH);
    note_id_t rejected = note_identity_new(8);
    t->p[P_VOICE] = V_MONO;
    assert(!trk_note_on_id(t, 60, 100, rejected));
    t->p[P_VOICE] = V_POLY;
    t->xf_on = 1;
    assert(!trk_note_on_id(t, 60, 100, rejected) && !t->xp_n);
    t->xf_on = 0;
    assert(!trk_note_on_id(t, 128, 100, rejected));
    assert(!trk_note_on_id(t, 60, 0, rejected));
    note_identity_end(rejected);
    puts("unsupported modes/transitions and malformed notes fail without queuing OK");

    reset_part(t, HOST_SYNTH);
    trk_note_on(t, 60, 100);
    trk_note_on(t, 60, 100);
    uint32_t gates = 0;
    for (uint32_t i = 0; i < NVOICE; i++) gates += t->v[i].gate != 0;
    assert(gates == 1);
    trk_note_off(t, 60);
    for (uint32_t i = 0; i < NVOICE; i++) assert(!t->v[i].gate);
    puts("conventional pitch allocation/release unchanged OK");

    note_id_t pool[NOTE_ID_CAP];
    for (uint32_t i = 0; i < NOTE_ID_CAP; i++) { pool[i] = note_identity_new(6); assert(pool[i]); }
    assert(!note_identity_new(6));
    note_id_t retired = pool[0];
    note_identity_end(retired);
    note_id_t renewed = note_identity_new(6);
    assert(renewed && renewed != retired && !note_identity_valid(retired));
    note_identity_end(renewed);
    for (uint32_t i = 1; i < NOTE_ID_CAP; i++) note_identity_end(pool[i]);
    assert(!note_identity_new(16));
    note_serial = 0x0fffffffu; /* terminal guard, never production reset */
    assert(!note_identity_new(0));
    puts("capacity/reuse/exhaustion cannot resurrect stale handles OK");
    printf("identity RAM: voices %zu + registry %zu + serial %zu bytes\n",
           sizeof(note_id_t) * NPART * NVOICE, sizeof note_ids, sizeof note_serial);
    return 0;
}
