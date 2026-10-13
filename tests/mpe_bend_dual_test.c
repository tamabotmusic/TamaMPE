/* SPDX-License-Identifier: GPL-3.0-only */
#define DUAL_CORE_TEST_NO_MAIN 1
#include "dual_core_test.c"

static void message(uint32_t status, uint32_t a, uint32_t b)
{
    assert(midi_enqueue((status >> 4) | (status << 8) | (a << 16) | (b << 24), 1));
}
static void bend_message(uint32_t ch, int32_t raw)
{
    uint32_t value = (uint32_t)(raw + 8192);
    message(0xE0u | ch, value & 127u, value >> 7);
}
static void mpe_case(int parallel, uint32_t alg, uint32_t engine)
{
    mpe_clear(1);
    memset(&mpe_zone, 0, sizeof mpe_zone);
    setup_case(alg, engine, 0);
    serial_render = !parallel;
    audio_worker_online = 1;
    mi_r = mi_w = 0;
    mpe_configure(3);
    assert(mpe_zone.members == 3);
    message(0x91, 60, 100); message(0x92, 60, 110);
    for (uint32_t pos = 0; pos < FRAMES; pos += CTL) {
        int32_t out[2 * CTL];
        if (pos == CTL * 16u) { bend_message(1, 8191); bend_message(2, -8192); }
        if (pos == CTL * 48u) bend_message(0, 8191);
        if (pos == CTL * 64u) { message(0xB0, 64, 127); message(0x81, 60, 0); }
        if (pos == CTL * 80u) { message(0x91, 65, 110); bend_message(1, -8192); }
        if (pos == CTL * 112u) { bend_message(1, 0); bend_message(2, 0); bend_message(0, 0); }
        if (pos == CTL * 144u) message(0xB0, 64, 0);
        if (pos == CTL * 160u) message(0xB0, 123, 0);
        mix_block(out, CTL);
        assert(!fm6_pending);
        if (!parallel) memcpy(reference + 2u * pos, out, sizeof out);
        else assert(!memcmp(reference + 2u * pos, out, sizeof out));
    }
    mpe_clear(1);
}
int main(void)
{
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, worker_thread, NULL));
    while (!AW_LOAD(&audio_worker.ready)) sched_yield();
    uint32_t before = fm6_pairs;
    for (uint32_t engine = 0; engine < 3; engine++)
        for (uint32_t alg = 0; alg < 32; alg++) {
            mpe_case(0, alg, engine);
            mpe_case(1, alg, engine);
        }
    assert(fm6_pairs > before);
    assert(!pthread_cancel(thread)); assert(!pthread_join(thread, NULL));
    printf("MPE bend: 96 algorithm/kernel cases serial/CPU1 PCM identical; %u actual pairs\n", fm6_pairs - before);
    return 0;
}
