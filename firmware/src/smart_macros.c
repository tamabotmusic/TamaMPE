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

/* The ISR owns smart_macro (including the slew state).  UI/main marks the
 * shadow sequence odd while it edits and even when complete.  The ISR first
 * snapshots it locally, then accepts the copy only if the sequence did not
 * change.  This is safe whether render runs on CPU0 or CPU1. */
static smart_macro_t smart_macro[NTRK][SMART_MACRO_N];
static smart_config_t smart_shadow[NTRK][SMART_MACRO_N];
static volatile uint32_t smart_publish_seq[NTRK], smart_applied_seq[NTRK];
static volatile uint8_t smart_reset_req[NTRK];
static struct {
    int16_t saved[2];
    uint8_t mask;
} smart_part;

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

static void smart_macro_reset(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr < NTRK) {
        smart_edit_begin(tr);
        memset(smart_shadow[tr], 0, sizeof smart_shadow[tr]);
        smart_edit_publish(tr);
        SMART_BARRIER();
        smart_reset_req[tr]++;
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
        uint32_t seq = smart_publish_seq[tr];
        memcpy(config, smart_shadow[tr], sizeof config);
        SMART_BARRIER();
        if (smart_publish_seq[tr] == seq) {
            for (k = 0; k < SMART_MACRO_N; k++) {
                smart_macro[tr][k].target = config[k].target;
                memcpy(smart_macro[tr][k].route, config[k].route, sizeof smart_macro[tr][k].route);
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
