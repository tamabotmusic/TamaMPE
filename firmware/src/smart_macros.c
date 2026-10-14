/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Smart Macros, phase 1.
 *
 * These are runtime performance controls, deliberately outside track_t and
 * the project / DX7 patch formats.  A macro has a bipolar Q15 target and
 * current value; zero is exact neutral.  The audio ISR moves current once
 * per control block.  For now each of the four slots can address FM6 MLVL
 * only, with an independently signed depth.  The temporary edit is made
 * after mod_begin() and undone before mod_end(), so neither the modulation
 * matrix nor any persistent parameter ever observes it.
 */
#define SMART_MACRO_N 4u
#define SMART_DST_OFF 0u
#define SMART_DST_FM6_MLVL 1u

typedef struct {
    int16_t value, target;       /* bipolar Q15, -32767..32767 */
    int8_t depth;                /* FM6 MLVL steps, signed */
    uint8_t destination;
} smart_macro_t;

static smart_macro_t smart_macro[NTRK][SMART_MACRO_N];
static volatile uint8_t smart_reset_req[NTRK];
static struct {
    int16_t mlvl;
    uint8_t on;
} smart_part;

static int16_t smart_q15(int32_t value)
{
    return (int16_t)clamp(value, -127, 127) * 258; /* 127 maps to 32766; zero stays exact */
}

/* Main-loop API.  Values are the compact -127..127 controller domain. */
static void smart_macro_value(track_t *t, uint32_t slot, int32_t value)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr < NTRK && slot < SMART_MACRO_N)
        smart_macro[tr][slot].target = smart_q15(value);
}

/* One route per slot in phase 1.  Unsupported destinations are safely off. */
static void smart_macro_route(track_t *t, uint32_t slot, uint32_t destination, int32_t depth)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr >= NTRK || slot >= SMART_MACRO_N)
        return;
    smart_macro[tr][slot].destination = destination == SMART_DST_FM6_MLVL ? destination : SMART_DST_OFF;
    smart_macro[tr][slot].depth = (int8_t)clamp(depth, -63, 63);
}

static void smart_macro_reset(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr < NTRK)
        smart_reset_req[tr]++;
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

/* Called immediately after mod_begin().  It sees the matrix's temporary
 * MLVL value and restores precisely that value in smart_end(). */
static __attribute__((noinline)) void smart_begin(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk), k;
    int32_t offset = 0;
    smart_part.on = 0;
    if (tr >= NTRK || t->engine != ENGI_FM6 || t->eng_req != ENGI_FM6)
        return;
    if (smart_reset_req[tr]) {
        memset(smart_macro[tr], 0, sizeof smart_macro[tr]);
        smart_reset_req[tr] = 0;
        return;
    }
    for (k = 0; k < SMART_MACRO_N; k++) {
        smart_macro_t *m = &smart_macro[tr][k];
        m->value = smart_slew(m->value, m->target);
        if (m->destination == SMART_DST_FM6_MLVL && m->depth && m->value)
            offset += ((int32_t)m->value * m->depth) / 32767;
    }
    if (!offset)
        return;
    smart_part.mlvl = t->p[P_E2];
    t->p[P_E2] = (int16_t)clamp((int32_t)smart_part.mlvl + offset, -64, 63);
    smart_part.on = 1;
}

static __attribute__((noinline)) void smart_end(track_t *t)
{
    if (smart_part.on)
        t->p[P_E2] = smart_part.mlvl;
    smart_part.on = 0;
}
