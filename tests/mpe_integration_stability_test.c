/* SPDX-License-Identifier: GPL-3.0-only */
/* Cross-check the deliberately separate local MPE-IN and keyboard MPE-OUT
 * paths under the lifecycle events that can otherwise leave ownership stale. */
#define MPE_NOTES_NO_MAIN 1
#include "mpe_notes_test.c"
#include <assert.h>

static void out_ready(uint32_t members)
{
    memset(&mpe_out, 0, sizeof mpe_out);
    mpe_out_forget(); mo_w = mo_r = 0;
    usb.config = 1; usb.suspended = 0;
    mpe_out_configure(members);
    for (uint32_t i = 0; i < 3u + 4u * members; i++) {
        mpe_out_service();
        mo_r = mo_w; /* model the USB endpoint consuming setup packets */
    }
    assert(mpe_out_ready());
}

int main(void)
{
    note_id_t local;
    uint16_t remote;

    /* Incoming member-channel expression remains a local FM6 concern.  It
     * must neither allocate a USB owner nor emit MIDI, even while MPE OUT is
     * configured and the selected track changes. */
    setup(); out_ready(2); uint32_t out_at = mo_w;
    song.sel = 2;
    queued(0x91, 60, 100, 1); local = owner(1, 60); assert(local && sounding(local));
    queued(0xE1, 127, 127, 1); queued(0xD1, 100, 0, 1); queued(0xB1, 74, 90, 1);
    assert(mo_w == out_at && !mpe_out_owner[0].id && !mpe_out_owner[1].id);
    assert(sounding(local)->note_id == local);
    puts("MPE IN stays local and does not bridge expression to USB MPE OUT OK");

    /* Local identities and keyboard-originated external identities coexist.
     * A local-zone reconfiguration owns only the local table, so it cannot
     * release a remote note on an unrelated member channel. */
    song.sel = 0;
    remote = mpe_out_note_on_track(&trk[3], 0, 0, 67, 100);
    assert(remote && mpe_out_find(remote) && mpe_out_find(remote)->track == 3u);
    configure(1);
    assert(!owners() && !identities() && mpe_out_find(remote));
    mpe_out_note_off(remote); assert(!mpe_out_find(remote));
    puts("MPE IN zone changes retain independent MPE OUT ownership OK");

    /* Panic is the one intentional common lifecycle boundary: it retires
     * local identities and external keyboard owners together, then schedules
     * remote All Sound Off before any deferred expression can run. */
    setup(); out_ready(1); queued(0x91, 61, 100, 1);
    remote = mpe_out_note_on_track(&trk[0], 0, 0, 62, 100);
    assert(owners() == 1u && remote && mpe_out_find(remote));
    panic_req |= 1u; events_block(CTL);
    assert(!owners() && !identities() && !mpe_out_find(remote) && mpe_out.resync);
    puts("panic clears local MPE identities and external USB owners together OK");

    /* A USB reset clears only remote ownership and keyboard mappings.  The
     * local MPE path deliberately remains independent because USB and TRS
     * share its channel state; an explicit MIDI panic/CC120 remains the safe
     * way to silence a possibly mixed-source local session. */
    setup(); out_ready(1); queued(0x91, 63, 100, 1); local = owner(1, 63);
    remote = mpe_out_note_on_track(&trk[0], 0, 0, 64, 100); assert(remote);
    kb_mpe_out[0][0] = remote;
    mpe_out_usb_reset();
    assert(!mpe_out_find(remote) && !kb_mpe_out[0][0] && mpe_out.resync);
    assert(owner(1, 63) == local && sounding(local));
    panic_req |= 1u; events_block(CTL);
    assert(!owners() && !identities());
    puts("USB recovery clears output state; mixed-source local input remains explicitly recoverable OK");
    return 0;
}
