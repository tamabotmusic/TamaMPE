# TamaMPE: preliminary USB MPE OUT

MPE OUT is a separate USB-MIDI path for the physical FM-1 keyboard. It never
forwards USB or TRS MIDI input, so an external MPE controller cannot form a
feedback loop through TamaMPE. TRS output is intentionally not assumed.

The lower zone uses MIDI channel 1 as master and configurable member channels
2–16. Enabling a zone transmits RPN 6 on the master, then RPN 0 with a +/-48
semitone range on every member before it accepts notes. A member stays assigned
to one transmitted note instance until its matching Note Off; equal pitches and
chords therefore remain independent.

Hold **HOME** to open Settings, select **MPE OUT**, then use knob 1 (or OCT+)
to choose **OFF** or 1–15 member channels. The choice is live-only: it starts
at OFF on every boot and is never saved in projects, presets or flash. While
the screen says **SENDING**, the audio block is transmitting a safe transition;
new external notes are deliberately suppressed until the receiver has the full
new configuration. The FM-1's internal synth keeps playing throughout.

Changing a live zone first discards the local output identities, sends All
Sound Off on the old master and member channels, then writes RPN 6 = 0 on the
master to leave that zone. Only then does it configure the requested new zone.
This conservative policy requires a fresh key press after a change. It also
means conventional MIDI events cannot be interleaved with an incomplete MPE
configuration. If USB is disconnected or its queue is full, this state machine
waits and resumes when transmission is possible.

Internal per-note calls emit 14-bit bend, Channel Pressure and CC74. Their
neutral values are bend 8192, pressure 0 and CC74 0. The FM-1 keyboard does not
invent pressure or Slide values; Smart Macros may become explicit sources later.

If the USB output queue rejects a Note Off, or USB resets/disconnects, local
ownership is discarded and All Sound Off is scheduled for master and every
member after reconnecting. This is a safe recovery, but a disconnected receiver
cannot be silenced until the cable/host returns. Notes are not retransmitted
after reconnect; play them again.
