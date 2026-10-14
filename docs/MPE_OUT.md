# TamaMPE: preliminary USB MPE OUT

MPE OUT is a separate USB-MIDI path for the physical FM-1 keyboard. It never
forwards USB or TRS MIDI input, so an external MPE controller cannot form a
feedback loop through TamaMPE. TRS output is intentionally not assumed.

The lower zone uses MIDI channel 1 as master and configurable member channels
2–16. Enabling a zone transmits RPN 6 on the master, then RPN 0 with a +/-48
semitone range on every member before it accepts notes. A member stays assigned
to one transmitted note instance until its matching Note Off; equal pitches and
chords therefore remain independent.

The initial implementation exposes `mpe_out_configure(members)` internally for
the future editor/control surface. It is disabled by default and has no user UI
yet, so normal MIDI OUT is unchanged. This deliberately avoids assigning a
meaning to stored project fields before the editor design is reviewed.

Internal per-note calls emit 14-bit bend, Channel Pressure and CC74. Their
neutral values are bend 8192, pressure 0 and CC74 0. The FM-1 keyboard does not
invent pressure or Slide values; Smart Macros may become explicit sources later.

If the USB output queue rejects a Note Off, or USB resets/disconnects, local
ownership is discarded and All Sound Off is scheduled for master and every
member after reconnecting. This is a safe recovery, but a disconnected receiver
cannot be silenced until the cable/host returns. Notes are not retransmitted
after reconnect; play them again.
