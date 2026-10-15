/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Smart Macros, phases 1 and 2.
 *
 * These are runtime performance controls, deliberately outside track_t and
 * the project / DX7 patch formats.  A macro has a bipolar Q15 target and
 * current value; zero is exact neutral.  The audio ISR moves current once
 * per control block.  Each of the four slots has two independently signed
 * routes to FM6 MLVL or track PAN.  The temporary edit is made
 * after mod_begin() and undone before mod_end(), so neither the modulation
 * matrix nor any persistent parameter ever observes it.
 */
#define SMART_MACRO_N 4u
#define SMART_ROUTE_N 2u
#define SMART_DST_OFF 0u
#define SMART_DST_FM6_MLVL 1u
#define SMART_DST_PAN 2u
#define SMART_CURVE_LINEAR 0u
#define SMART_CURVE_SMOOTH 1u
#define SMART_CURVE_EXP 2u
#define SMART_MPE_OUT_OFF 0u
#define SMART_MPE_OUT_CC74 1u
#define SMART_MPE_OUT_PRESSURE 2u
#define SMART_MPE_OUT_BEND 3u
#define SMART_MPE_OUT_N 3u
#define SMART_SMR1_SIZE 140u
#define SMART_SMR1_DATA 128u
#define SMART_SMR1_MAGIC 0x31524D53u /* "SMR1", little-endian */
#define SMART_SME1_SIZE 62u
#define SMART_SME1_DATA 52u
#define SMART_SME1_MAGIC 0x31454D53u /* "SME1", little-endian */
/* AC79 internal SRAM is coherent between CPU0 and CPU1, but publishing a
 * sequence still needs the processor fence used by the audio-worker mailbox.
 * Host tests retain a compiler-only barrier. */
#ifdef FM1_IRQ_TARGET
#define SMART_BARRIER() fm1_sram_sync()
#else
#define SMART_BARRIER() __asm__ volatile("" ::: "memory")
#endif

typedef struct {
    int8_t depth, lo, hi;        /* signed output steps */
    uint8_t destination, curve;
} smart_route_t;

typedef struct {
    int16_t value, target;       /* bipolar Q15, -32767..32767 */
    smart_route_t route[SMART_ROUTE_N];
} smart_macro_t;

typedef struct {
    int16_t target;
    smart_route_t route[SMART_ROUTE_N];
} smart_config_t;

/* Runtime-only MPE OUT assignment.  It deliberately has its own storage:
 * loading a DX7 sound or clearing SMR1 routes cannot alter external MIDI. */
typedef struct {
    int8_t depth, lo, hi;
    uint8_t meta;                 /* destination 0..3, curve 0..2 */
} smart_mpe_out_map_t;
_Static_assert(sizeof(smart_mpe_out_map_t) == 4u, "compact MPE OUT map");

/* The ISR owns smart_macro (including the slew state).  UI/main marks the
 * shadow sequence odd while it edits and even when complete.  The ISR first
 * snapshots it locally, then accepts the copy only if the sequence did not
 * change.  This is safe whether render runs on CPU0 or CPU1. */
static smart_macro_t smart_macro[NTRK][SMART_MACRO_N];
static smart_config_t smart_shadow[NTRK][SMART_MACRO_N];
static smart_mpe_out_map_t smart_mpe_out[NTRK][SMART_MACRO_N];
static smart_mpe_out_map_t smart_mpe_out_shadow[NTRK][SMART_MACRO_N];
static volatile uint32_t smart_publish_seq[NTRK], smart_applied_seq[NTRK];
static volatile uint8_t smart_reset_req[NTRK];
/* A future SME1 is opaque.  This firmware can rename its stored project
 * without touching the bytes, but must not silently replace it on SAVE or
 * autosave; a later UI conversion may explicitly clear this guard. */
static uint8_t smart_sme1_future;
static struct {
    int16_t saved[2];
    uint8_t mask;
} smart_part;

static uint32_t smart_crc32(const uint8_t *b, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *b++;
        for (uint32_t k = 0; k < 8u; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void smart_u32_put(uint8_t *b, uint32_t v)
{ b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24); }
static uint32_t smart_u32_get(const uint8_t *b)
{ return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24; }

static int16_t smart_q15(int32_t value)
{
    return (int16_t)clamp(value, -127, 127) * 258; /* 127 maps to 32766; zero stays exact */
}

static int32_t smart_value127(int16_t value)
{
    return clamp(((int32_t)value + (value < 0 ? -129 : 129)) / 258, -127, 127);
}

static void smart_edit_begin(uint32_t tr)
{
    smart_publish_seq[tr]++;
    SMART_BARRIER();
}

static void smart_edit_publish(uint32_t tr)
{
    SMART_BARRIER();
    smart_publish_seq[tr]++;
    SMART_BARRIER();
}

/* Main-loop API.  Values are the compact -127..127 controller domain. */
static void smart_macro_value(track_t *t, uint32_t slot, int32_t value)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr >= NTRK || slot >= SMART_MACRO_N)
        return;
    smart_edit_begin(tr);
    smart_shadow[tr][slot].target = smart_q15(value);
    smart_edit_publish(tr);
}

/* Configure one of the two routes. Unsupported destinations/curves are off. */
static void smart_macro_route_config(track_t *t, uint32_t slot, uint32_t route,
                                     uint32_t destination, int32_t depth,
                                     int32_t lo, int32_t hi, uint32_t curve)
{
    uint32_t tr = (uint32_t)(t - trk);
    smart_route_t *r;
    if (tr >= NTRK || slot >= SMART_MACRO_N || route >= SMART_ROUTE_N)
        return;
    if (destination != SMART_DST_FM6_MLVL && destination != SMART_DST_PAN)
        destination = SMART_DST_OFF;
    if (curve > SMART_CURVE_EXP)
        curve = SMART_CURVE_LINEAR;
    if (lo > hi) { int32_t x = lo; lo = hi; hi = x; }
    /* A runtime macro's neutral point is always zero.  A user range that
     * excludes it is widened, never shifted, so zero cannot cause a jump. */
    if (lo > 0) lo = 0;
    if (hi < 0) hi = 0;
    smart_edit_begin(tr);
    r = &smart_shadow[tr][slot].route[route];
    r->destination = (uint8_t)destination;
    r->depth = (int8_t)clamp(depth, -63, 63);
    r->lo = (int8_t)clamp(lo, -64, 63);
    r->hi = (int8_t)clamp(hi, -64, 63);
    r->curve = (uint8_t)curve;
    smart_edit_publish(tr);
}

/* Compatibility helper from phase 1: route zero, full parameter range. */
static void smart_macro_route(track_t *t, uint32_t slot, uint32_t destination, int32_t depth)
{
    smart_macro_route_config(t, slot, 0, destination, depth, -64, 63, SMART_CURVE_LINEAR);
    smart_macro_route_config(t, slot, 1, SMART_DST_OFF, 0, -64, 63, SMART_CURVE_LINEAR);
}

/* Main-loop API for phase 5B.  The output range uses the same compact
 * bipolar controller domain as Smart Macro routes; normalization preserves
 * the exact neutral point for every MIDI destination. */
static void smart_mpe_out_map_config(track_t *t, uint32_t slot, uint32_t destination,
                                     int32_t depth, int32_t lo, int32_t hi, uint32_t curve)
{
    uint32_t tr = (uint32_t)(t - trk);
    smart_mpe_out_map_t *m;
    if (tr >= NTRK || slot >= SMART_MACRO_N)
        return;
    if (destination > SMART_MPE_OUT_BEND || curve > SMART_CURVE_EXP)
        destination = SMART_MPE_OUT_OFF, curve = SMART_CURVE_LINEAR;
    if (lo > hi) { int32_t x = lo; lo = hi; hi = x; }
    if (lo > 0) lo = 0;
    if (hi < 0) hi = 0;
    smart_edit_begin(tr);
    m = &smart_mpe_out_shadow[tr][slot];
    m->depth = (int8_t)clamp(depth, -127, 127);
    m->lo = (int8_t)clamp(lo, -127, 127);
    m->hi = (int8_t)clamp(hi, -127, 127);
    m->meta = (uint8_t)(destination | curve << 2);
    smart_edit_publish(tr);
}

/* Only tests use this immediate hook in phase 5B.  It keeps the published
 * production path intact while retaining phase-5A's direct host-test setup. */
static void smart_mpe_out_map_test_config(track_t *t, uint32_t slot, uint32_t destination,
                                          int32_t depth, int32_t lo, int32_t hi, uint32_t curve)
{
    uint32_t tr = (uint32_t)(t - trk);
    smart_mpe_out_map_config(t, slot, destination, depth, lo, hi, curve);
    if (tr < NTRK && slot < SMART_MACRO_N)
        smart_mpe_out[tr][slot] = smart_mpe_out_shadow[tr][slot];
}

/* Values are performance state, never part of SMR1. */
static void smart_macro_reset_values(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr < NTRK) {
        smart_edit_begin(tr);
        for (uint32_t k = 0; k < SMART_MACRO_N; k++)
            smart_shadow[tr][k].target = 0;
        smart_edit_publish(tr);
        SMART_BARRIER();
        smart_reset_req[tr]++;
    }
}

static void smart_macro_clear_routes(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr < NTRK) {
        smart_edit_begin(tr);
        memset(smart_shadow[tr], 0, sizeof smart_shadow[tr]);
        memset(smart_mpe_out_shadow[tr], 0, sizeof smart_mpe_out_shadow[tr]);
        smart_edit_publish(tr);
        SMART_BARRIER();
        smart_reset_req[tr]++;
    }
}

/* Kept for the existing runtime tests and callers that explicitly want both. */
static void smart_macro_reset(track_t *t) { smart_macro_clear_routes(t); }

static void smart_smr1_off(uint8_t out[SMART_SMR1_SIZE])
{
    memset(out, 0, SMART_SMR1_SIZE);
    smart_u32_put(out, SMART_SMR1_MAGIC);
    out[4] = 1u; out[5] = (1u << NTRK) - 1u;
    out[6] = SMART_SMR1_DATA; out[7] = 0u;
    smart_u32_put(out + 136u, smart_crc32(out, 136u));
}

static int smart_snapshot_track(uint32_t tr, smart_config_t out[SMART_MACRO_N])
{
    for (uint32_t tries = 0; tries < 8u; tries++) {
        uint32_t seq = smart_publish_seq[tr];
        if (seq & 1u) continue;
        SMART_BARRIER();
        memcpy(out, smart_shadow[tr], sizeof smart_shadow[tr]);
        SMART_BARRIER();
        if (smart_publish_seq[tr] == seq && !(seq & 1u)) return 1;
    }
    return 0;
}

/* SMR1 and SME1 are one project configuration.  Capture both under the same
 * per-track publication sequence so an editor change cannot combine a route
 * from one edit with an external assignment from another. */
static int smart_snapshot_track_all(uint32_t tr, smart_config_t in[SMART_MACRO_N],
                                    smart_mpe_out_map_t out[SMART_MACRO_N])
{
    for (uint32_t tries = 0; tries < 8u; tries++) {
        uint32_t seq = smart_publish_seq[tr];
        if (seq & 1u) continue;
        SMART_BARRIER();
        memcpy(in, smart_shadow[tr], sizeof smart_shadow[tr]);
        memcpy(out, smart_mpe_out_shadow[tr], sizeof smart_mpe_out_shadow[tr]);
        SMART_BARRIER();
        if (smart_publish_seq[tr] == seq && !(seq & 1u)) return 1;
    }
    return 0;
}

/* A compact, explicit wire format: route order is track, macro, A/B.
 * Byte 0: destination bits 0..1, curve bits 2..3 (bits 4..7 are zero).
 * Byte 1: depth + 63 (0..126); byte 2: lo + 64; byte 3: hi + 64.
 * This never depends on C structure padding or signed representation. */
static int smart_macro_snapshot_routes(uint8_t out[SMART_SMR1_SIZE])
{
    smart_smr1_off(out);
    for (uint32_t tr = 0; tr < NTRK; tr++) {
        smart_config_t c[SMART_MACRO_N];
        if (!smart_snapshot_track(tr, c)) return 0;
        for (uint32_t m = 0; m < SMART_MACRO_N; m++) for (uint32_t r = 0; r < SMART_ROUTE_N; r++) {
            const smart_route_t *q = &c[m].route[r];
            uint32_t at = 8u + ((tr * SMART_MACRO_N + m) * SMART_ROUTE_N + r) * 4u;
            uint32_t destination = q->destination <= SMART_DST_PAN ? q->destination : SMART_DST_OFF;
            uint32_t curve = q->curve <= SMART_CURVE_EXP ? q->curve : SMART_CURVE_LINEAR;
            int32_t lo = q->lo, hi = q->hi;
            if (lo > hi) { int32_t x=lo;lo=hi;hi=x; }
            if (lo > 0) lo = 0;
            if (hi < 0) hi = 0;
            out[at] = (uint8_t)(destination | curve << 2);
            out[at + 1u] = (uint8_t)(clamp(q->depth, -63, 63) + 63);
            out[at + 2u] = (uint8_t)(clamp(lo, -64, 63) + 64);
            out[at + 3u] = (uint8_t)(clamp(hi, -64, 63) + 64);
        }
    }
    smart_u32_put(out + 136u, smart_crc32(out, 136u));
    return 1;
}

static void smart_sme1_off(uint8_t out[SMART_SME1_SIZE])
{
    memset(out, 0, SMART_SME1_SIZE);
    smart_u32_put(out, SMART_SME1_MAGIC);
    out[4] = 1u; out[5] = SMART_SME1_DATA;
    smart_u32_put(out + 58u, smart_crc32(out, 58u));
}

/* Explicit little-endian bit packing: bit 0 is byte 0 bit 0.  The 26-bit
 * records are deliberately independent of C layout and integer endianness. */
static void smart_bits_put(uint8_t *b, uint32_t *pos, uint32_t v, uint32_t n)
{ while (n--) { b[*pos >> 3] |= (uint8_t)((v & 1u) << (*pos & 7u)); v >>= 1; ++*pos; } }
static uint32_t smart_bits_get(const uint8_t *b, uint32_t *pos, uint32_t n)
{ uint32_t v=0; for (uint32_t i=0;i<n;i++,++*pos) v|=(uint32_t)((b[*pos>>3]>>(*pos&7u))&1u)<<i; return v; }

/* Capture the two extensions as one coherent set.  The caller owns both
 * output buffers, so a failed bounded retry writes no NOR record. */
static int smart_macro_snapshot_persist(uint8_t smr[SMART_SMR1_SIZE],
                                        uint8_t sme[SMART_SME1_SIZE])
{
    if (smart_sme1_future) return 0;
    smart_smr1_off(smr); smart_sme1_off(sme);
    for (uint32_t tr=0; tr<NTRK; tr++) {
        smart_config_t c[SMART_MACRO_N]; smart_mpe_out_map_t e[SMART_MACRO_N];
        if (!smart_snapshot_track_all(tr,c,e)) return 0;
        for (uint32_t m=0;m<SMART_MACRO_N;m++) {
            for (uint32_t r=0;r<SMART_ROUTE_N;r++) {
                const smart_route_t *q=&c[m].route[r]; uint32_t at=8u+((tr*SMART_MACRO_N+m)*SMART_ROUTE_N+r)*4u;
                uint32_t d=q->destination<=SMART_DST_PAN?q->destination:SMART_DST_OFF;
                uint32_t cv=q->curve<=SMART_CURVE_EXP?q->curve:SMART_CURVE_LINEAR;
                int32_t lo=q->lo,hi=q->hi; if(lo>hi){int32_t x=lo;lo=hi;hi=x;} if(lo>0)lo=0;if(hi<0)hi=0;
                smr[at]=(uint8_t)(d|cv<<2); smr[at+1u]=(uint8_t)(clamp(q->depth,-63,63)+63);
                smr[at+2u]=(uint8_t)(clamp(lo,-64,63)+64); smr[at+3u]=(uint8_t)(clamp(hi,-64,63)+64);
            }
            { const smart_mpe_out_map_t *q=&e[m]; uint32_t d=q->meta&3u,cv=(q->meta>>2)&3u;
              int32_t lo=q->lo,hi=q->hi; uint32_t pos=(tr*SMART_MACRO_N+m)*26u;
              if(d>SMART_MPE_OUT_BEND||cv>SMART_CURVE_EXP){d=0;cv=0;} if(lo>hi){int32_t x=lo;lo=hi;hi=x;}if(lo>0)lo=0;if(hi<0)hi=0;
              smart_bits_put(sme+6u,&pos,(uint32_t)(clamp(q->depth,-127,127)+127),8u);
              smart_bits_put(sme+6u,&pos,(uint32_t)-clamp(lo,-127,0),7u);
              smart_bits_put(sme+6u,&pos,(uint32_t)clamp(hi,0,127),7u);
              smart_bits_put(sme+6u,&pos,d,2u); smart_bits_put(sme+6u,&pos,cv,2u); }
        }
    }
    smart_u32_put(smr+136u,smart_crc32(smr,136u));
    smart_u32_put(sme+58u,smart_crc32(sme,58u));
    return 1;
}

/* Invalid headers leave every route off.  A valid block can still contain a
 * malformed route; that route alone is disabled.  Restoring publishes a full
 * configuration with zero targets, then asks the ISR to discard old slew. */
static void smart_macro_restore_routes(const uint8_t in[SMART_SMR1_SIZE])
{
    int valid = smart_u32_get(in) == SMART_SMR1_MAGIC && in[4] == 1u && in[5] == (1u << NTRK) - 1u &&
                in[6] == SMART_SMR1_DATA && !in[7] && smart_u32_get(in + 136u) == smart_crc32(in, 136u);
    for (uint32_t tr = 0; tr < NTRK; tr++) {
        smart_config_t c[SMART_MACRO_N];
        memset(c, 0, sizeof c);
        if (valid) for (uint32_t m = 0; m < SMART_MACRO_N; m++) for (uint32_t r = 0; r < SMART_ROUTE_N; r++) {
            uint32_t at = 8u + ((tr * SMART_MACRO_N + m) * SMART_ROUTE_N + r) * 4u;
            uint8_t meta=in[at], depth=in[at+1u], lo=in[at+2u], hi=in[at+3u];
            uint32_t destination=meta&3u, curve=(meta>>2)&3u;
            if ((meta & 0xF0u) || destination > SMART_DST_PAN || curve > SMART_CURVE_EXP || depth > 126u || lo > 127u || hi > 127u)
                continue;
            c[m].route[r].destination = (uint8_t)destination;
            c[m].route[r].curve = (uint8_t)curve;
            c[m].route[r].depth = (int8_t)((int32_t)depth - 63);
            c[m].route[r].lo = (int8_t)((int32_t)lo - 64);
            c[m].route[r].hi = (int8_t)((int32_t)hi - 64);
            if (c[m].route[r].lo > c[m].route[r].hi || c[m].route[r].lo > 0 || c[m].route[r].hi < 0)
                memset(&c[m].route[r], 0, sizeof c[m].route[r]);
        }
        smart_edit_begin(tr);
        memcpy(smart_shadow[tr], c, sizeof c);
        smart_edit_publish(tr);
        SMART_BARRIER();
        smart_reset_req[tr]++;
    }
}

/* Restore both project extensions in one publication.  An absent, corrupt or
 * future SME1 is safely OFF; valid individual records remain independently
 * rejectable.  Runtime target and slew state are reset by the ISR boundary. */
static void smart_macro_restore_persist(const uint8_t smr[SMART_SMR1_SIZE],
                                        const uint8_t sme[SMART_SME1_SIZE])
{
    int sv=smart_u32_get(smr)==SMART_SMR1_MAGIC&&smr[4]==1u&&smr[5]==(1u<<NTRK)-1u&&smr[6]==SMART_SMR1_DATA&&!smr[7]&&smart_u32_get(smr+136u)==smart_crc32(smr,136u);
    int future=smart_u32_get(sme)==SMART_SME1_MAGIC&&sme[4]!=1u;
    int ev=smart_u32_get(sme)==SMART_SME1_MAGIC&&sme[4]==1u&&sme[5]==SMART_SME1_DATA&&smart_u32_get(sme+58u)==smart_crc32(sme,58u);
    smart_sme1_future=(uint8_t)future;
    for(uint32_t tr=0;tr<NTRK;tr++) {
        smart_config_t c[SMART_MACRO_N]; smart_mpe_out_map_t e[SMART_MACRO_N]; memset(c,0,sizeof c);memset(e,0,sizeof e);
        for(uint32_t m=0;m<SMART_MACRO_N;m++) {
            if(sv) for(uint32_t r=0;r<SMART_ROUTE_N;r++){uint32_t at=8u+((tr*SMART_MACRO_N+m)*SMART_ROUTE_N+r)*4u;uint8_t x=smr[at],d=smr[at+1u],lo=smr[at+2u],hi=smr[at+3u];uint32_t dst=x&3u,cv=(x>>2)&3u;
                if(!(x&0xf0u)&&dst<=SMART_DST_PAN&&cv<=SMART_CURVE_EXP&&d<=126u&&lo<=127u&&hi<=127u){smart_route_t *q=&c[m].route[r];q->destination=dst;q->curve=cv;q->depth=(int8_t)((int32_t)d-63);q->lo=(int8_t)((int32_t)lo-64);q->hi=(int8_t)((int32_t)hi-64);if(q->lo>q->hi||q->lo>0||q->hi<0)memset(q,0,sizeof *q);}}
            if(ev){uint32_t pos=(tr*SMART_MACRO_N+m)*26u,depth=smart_bits_get(sme+6u,&pos,8u),neg=smart_bits_get(sme+6u,&pos,7u),hi=smart_bits_get(sme+6u,&pos,7u),dst=smart_bits_get(sme+6u,&pos,2u),cv=smart_bits_get(sme+6u,&pos,2u);
                if(depth<255u&&dst>SMART_MPE_OUT_OFF&&dst<=SMART_MPE_OUT_BEND&&cv<=SMART_CURVE_EXP){e[m].depth=(int8_t)((int32_t)depth-127);e[m].lo=(int8_t)-(int32_t)neg;e[m].hi=(int8_t)hi;e[m].meta=(uint8_t)(dst|cv<<2);}}
        }
        smart_edit_begin(tr); memcpy(smart_shadow[tr],c,sizeof c);memcpy(smart_mpe_out_shadow[tr],e,sizeof e);smart_edit_publish(tr);SMART_BARRIER();smart_reset_req[tr]++;
    }
}

/* Four blocks gives a short, deterministic control-rate slew without DSP work per sample. */
static int16_t smart_slew(int16_t value, int16_t target)
{
    int32_t d = (int32_t)target - value;
    if (!d)
        return value;
    d /= 4;
    if (!d)
        d = target > value ? 1 : -1;
    if ((target > value && d > target - value) || (target < value && d < target - value))
        return target;
    return (int16_t)(value + d);
}

/* Odd, Q15 curves: all return exactly zero for zero.  The exponential form
 * is a compact squared response, avoiding a per-sample or floating-point
 * calculation while retaining a useful musical taper. */
static int32_t smart_curve(int32_t x, uint32_t curve)
{
    int32_t u = x < 0 ? -x : x, y;
    if (curve == SMART_CURVE_SMOOTH) {
        int32_t q = (u * u) >> 15;
        y = (q * (3 * 32767 - 2 * u)) >> 15;
    } else if (curve == SMART_CURVE_EXP) {
        y = (u * u) >> 15;
    } else {
        y = u;
    }
    return x < 0 ? -y : y;
}

static int32_t smart_route_value(const smart_macro_t *m, const smart_route_t *r)
{
    if (!m->value || !r->depth)
        return 0;
    int32_t x = smart_curve(m->value, r->curve);
    int32_t out = (x * r->depth) / 32767;
    return clamp(out, r->lo, r->hi);
}

static int32_t smart_round_div(int32_t n, int32_t d)
{ return n < 0 ? -((-n + d / 2) / d) : (n + d / 2) / d; }

/* Read only ISR-owned state.  events_block consumes this complete previous
 * block snapshot before smart_begin advances the next control value. */
static int smart_mpe_out_values(const track_t *t, uint16_t out[SMART_MPE_OUT_N])
{
    uint32_t tr = (uint32_t)(t - trk);
    int32_t sum[SMART_MPE_OUT_N] = {0, 0, 0};
    int active = 0;
    out[0] = out[1] = 0; out[2] = 8192;
    if (tr >= NTRK || t->engine != ENGI_FM6 || t->eng_req != ENGI_FM6)
        return 0;
    for (uint32_t k = 0; k < SMART_MACRO_N; k++) {
        const smart_mpe_out_map_t *m = &smart_mpe_out[tr][k];
        uint32_t destination = m->meta & 3u, curve = (m->meta >> 2) & 3u;
        int32_t value;
        if (destination == SMART_MPE_OUT_OFF || destination > SMART_MPE_OUT_BEND ||
            curve > SMART_CURVE_EXP || !m->depth)
            continue;
        value = smart_round_div(smart_curve(smart_macro[tr][k].value, curve) * m->depth, 32767);
        sum[destination - 1u] += clamp(value, m->lo, m->hi);
        active = 1;
    }
    out[0] = (uint16_t)clamp(sum[0], 0, 127);
    out[1] = (uint16_t)clamp(sum[1], 0, 127);
    sum[2] = clamp(sum[2], -127, 127);
    out[2] = (uint16_t)(8192 + (sum[2] < 0 ? smart_round_div(sum[2] * 8192, 127) :
                                                smart_round_div(sum[2] * 8191, 127)));
    return active;
}

/* UI reads its own shadow rather than ISR state, so its cards immediately
 * reflect an edit even before the next audio control block consumes it. */
static int32_t smart_macro_target(const track_t *t, uint32_t slot)
{
    uint32_t tr = (uint32_t)(t - trk);
    return tr < NTRK && slot < SMART_MACRO_N ? smart_value127(smart_shadow[tr][slot].target) : 0;
}

static const smart_route_t *smart_macro_route_get(const track_t *t, uint32_t slot, uint32_t route)
{
    uint32_t tr = (uint32_t)(t - trk);
    return tr < NTRK && slot < SMART_MACRO_N && route < SMART_ROUTE_N ? &smart_shadow[tr][slot].route[route] : 0;
}

static int smart_destination(uint32_t destination, uint32_t *index, int32_t *lo, int32_t *hi)
{
    if (destination == SMART_DST_FM6_MLVL) { *index = P_E2; *lo = -64; *hi = 63; return 1; }
    if (destination == SMART_DST_PAN) { *index = P_PAN; *lo = -64; *hi = 63; return 1; }
    return 0;
}

/* Called immediately after mod_begin().  It sees the matrix's temporary
 * MLVL value and restores precisely that value in smart_end(). */
static __attribute__((noinline)) void smart_begin(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk), k;
    int32_t offset[2] = {0, 0};
    smart_part.mask = 0;
    if (tr >= NTRK || t->engine != ENGI_FM6 || t->eng_req != ENGI_FM6)
        return;
    if (smart_reset_req[tr]) {
        memset(smart_macro[tr], 0, sizeof smart_macro[tr]);
        smart_reset_req[tr] = 0;
        return;
    }
    if (smart_publish_seq[tr] != smart_applied_seq[tr] && !(smart_publish_seq[tr] & 1u)) {
        /* Copy only a stable shadow; slew values remain ISR-owned and
         * therefore continuous.  A concurrent edit simply leaves the last
         * complete configuration active for this block. */
        smart_config_t config[SMART_MACRO_N];
        smart_mpe_out_map_t out[SMART_MACRO_N];
        uint32_t seq = smart_publish_seq[tr];
        memcpy(config, smart_shadow[tr], sizeof config);
        memcpy(out, smart_mpe_out_shadow[tr], sizeof out);
        SMART_BARRIER();
        if (smart_publish_seq[tr] == seq) {
            for (k = 0; k < SMART_MACRO_N; k++) {
                smart_macro[tr][k].target = config[k].target;
                memcpy(smart_macro[tr][k].route, config[k].route, sizeof smart_macro[tr][k].route);
                smart_mpe_out[tr][k] = out[k];
            }
            smart_applied_seq[tr] = seq;
        }
    }
    for (k = 0; k < SMART_MACRO_N; k++) {
        smart_macro_t *m = &smart_macro[tr][k];
        m->value = smart_slew(m->value, m->target);
        for (uint32_t j = 0; j < SMART_ROUTE_N; j++) {
            smart_route_t *r = &m->route[j];
            if (r->destination == SMART_DST_FM6_MLVL)
                offset[0] += smart_route_value(m, r);
            else if (r->destination == SMART_DST_PAN)
                offset[1] += smart_route_value(m, r);
        }
    }
    for (k = 0; k < 2u; k++) {
        uint32_t index; int32_t lo, hi;
        if (!offset[k] || !smart_destination(k + SMART_DST_FM6_MLVL, &index, &lo, &hi))
            continue;
        smart_part.saved[k] = t->p[index];
        t->p[index] = (int16_t)clamp((int32_t)smart_part.saved[k] + offset[k], lo, hi);
        smart_part.mask |= (uint8_t)(1u << k);
    }
}

static __attribute__((noinline)) void smart_end(track_t *t)
{
    if (smart_part.mask & 1u)
        t->p[P_E2] = smart_part.saved[0];
    if (smart_part.mask & 2u)
        t->p[P_PAN] = smart_part.saved[1];
    smart_part.mask = 0;
}

static int smart_pan_touched(void)
{
    return (smart_part.mask & 2u) != 0;
}
