# TamaMPE: first MPE note input

This implements note ownership, independent FM6 pitch bend and local Channel
Pressure timbre. CC74 and other unsupported messages on reserved zone
channels are consumed without changing track controls or DX7 programs.

## Activation

MPE is off at boot and is not stored in projects or presets. Select an FM6 track
in POLY, with ARP off and QNT OFF, then send RPN 6 on MIDI channel 1:

| Message | Value |
| --- | --- |
| CC101 (RPN MSB) | 0 |
| CC100 (RPN LSB) | 6 |
| CC6 (Data Entry MSB) | number of members, 1–15 |

For example, 3 reserves master channel 1 and member channels 2–4. CC6=0 with
RPN 6 selected disables the zone and silences its notes. Counts above 15 are
ignored; Data Entry LSB does not configure the zone. An activation on an
unsupported track leaves MPE disabled. No upper zone is implemented.

The destination track is captured at activation. Changing the selected track
does not retarget existing or subsequent member notes. Reconfiguration starts
a new session. Conventional MIDI notes on newly reserved channels are released
at activation; physical keys and owners on other channels are preserved.

## Ownership and cleanup

Each Note On allocates a logical identity and a static owner record containing
the original channel, MIDI note and destination track. Equal pitches remain
independent. Same-channel/same-note repetitions use FIFO Note Off matching.
Stolen voices retain owner records until their matching release, so a later
Note Off cannot accidentally release a different instance within that FIFO.
Note On with velocity zero is Note Off.

Master sustain applies to the zone; member sustain applies to that channel.
A released note remains held while either applicable pedal is down. CC123
releases the applicable keys and respects sustain. CC120 silences and discards
the applicable owners regardless of sustain. CC121 releases deferred notes by
resetting applicable pedals; on the master it resets all zone pedals. It also
centres the applicable bend while preserving sensitivity and restores a
still-keyed member note to zero pressure. Master pressure remains ignored. Master Note On/Off messages are consumed.

Track panic, engine/mode changes, FM6 preset reload or native transpose change,
MPE reconfiguration/disable, and MIDI queue overflow retire affected identities.
The zone configuration remains bound after panic/overflow; an unsupported
destination cannot start notes until it is FM6 POLY with ARP off and QNT OFF.

There are 64 owner slots. If owners or identity slots are exhausted, the MPE
session is silenced, all its owners are retired, and the triggering note is
dropped. Later notes may start a new session. No dynamic allocation is used.

**MIDI 1.0 limitation:** after panic, disable, reconfiguration or another session
reset, an old Note Off is indistinguishable from a new one with the same channel
and pitch. It can release a new matching note. There is no channel quarantine.

## Initial restrictions

- USB and TRS intentionally share MPE configuration, pedals and FIFO ownership.
  Do not simultaneously use controllers with overlapping MPE channels. Input
  sources are not isolated by these owner records.
- Only FM6 POLY is supported. ARP and scale quantization/microtonal modes block
  member note input and clean active MPE owners; conventional behavior is unchanged.
- Member notes use raw MIDI pitches without automatic chords, keyboard layout
  mapping, arpeggiation, capture or recording. DX7 program parameters and normal
  track tuning still apply. Physical keyboard and nonreserved MIDI channels
  continue through the original paths.
- An identity is not a voice index, preserving room for future engines and Layer.
  Prophet, Layer, Smart Macros and timbre expression are not implemented here.

## Independent FM6 pitch bend

Member bend uses the MIDI 14-bit value, with exact negative/positive endpoints
and centre. Initial sensitivity is +/-48 semitones on members and +/-2 on the
master. RPN 0 (CC101=0, CC100=0) sets each zone channel independently: CC6 sets
0–127 semitones and CC38 sets cents, clamped to 99. These settings are runtime
MPE state, separate from conventional RPN state and DX7 function settings.
Activation/reconfiguration restores the initial ranges. Panic resets bend
values but retains configured sensitivities. NRPN selection and RPN Null cancel
RPN data entry as usual.

Each voice keeps its own target/current bend in Q24 octaves. Member expression
updates only the channel's latest still-keyed instance; older overlapping notes
and notes whose keys have been released retain their own snapshots, including
sustain and release tails. No fallback to an earlier note occurs when the latest
one ends. Bend arriving before Note On initializes the next instance.
Master bend adds a separate contribution to current-session MPE voices, including
release tails, without touching conventional voices on the same track.
Session reset freezes old release tails so a new session cannot retune them.

FM6 applies this bend once, to ratio and fixed operators, on CPU0's 64-sample
control grid, with a monotonic smoothing ramp (~5 ms time constant) and exact
landing. It does not reset oscillator phases. Note On starts at the cached bend;
ongoing gestures use smoothing. The existing conventional FM6 bend range/step
settings and shared controller path are unchanged. No DX7 patch bytes are edited.

Large ranges can move operator frequencies outside the audible/Nyquist range;
the existing FM6 frequency/aliasing behavior still applies. This feature does not
add oversampling or change the DX7 engine's frequency law. USB/TRS remain shared.

## Independent FM6 Channel Pressure (provisional timbre destination)

Member Channel Pressure (0xD0) updates only the latest still-keyed instance for
that channel. Zero and absence of messages are neutral: no output gain or patch
changes. Raw pressure is stored separately from its smoothed source value for
future programmable destinations. CC74 remains reserved and unimplemented.
Master pressure is ignored; conventional channel aftertouch is unchanged.

The default assignment raises only enabled, nonzero-level, active modulators,
after AMS. It leaves carriers and shared pmod/amod/emod/AMS caches untouched.
A finished envelope or an operator below the native/post-AMS silent threshold
cannot be resurrected. Voice completion uses the original envelopes.

FM6 levels are Q24 log2 amplitude. Maximum pressure adds 0.5 log2 units
(128 internal microsteps): sqrt(2), +3.0103 dB in MODERN modulator amplitude.
MARK I attenuation changes by 512 codes; OPL quantizes its attenuation steps.
This is NOT +3 dB of audible output. Pressure 64 supplies approximately half
that offset. Computed levels remain within the existing 0..20 Q24 bounds.

The source approaches its target monotonically on FM6's 64-sample control grid
(~5 ms time constant), with exact landing. Existing operator gain ramps smooth
samples. No new sample loop or output-gain compensation is used. The bounded
pressure-level helper executes on CPU0 in XIP via the existing far-call convention,
conserving the scarce DSP SRAM. CPU1 still consumes the prepared voice plan.

Key release freezes pressure targets, including sustain/release tails. Channel
reuse cannot retarget older instances. A new note initializes from cached pressure.
CC121 restores the latest still-keyed member to zero. Session cleanup resets
channel caches to zero; FIFO bookkeeping remains intact for stolen notes.

Response depends on the preset and algorithm. Controlled MODERN fixtures responded in
31/32 algorithms; the all-carrier algorithm remained unchanged. In eight tested
factory DX7 programs, WOOD BARS and DRAWBARS had no measured response; TINE EP and NYLON PICK
had weak response in the tested note/window. The response does not compensate
by adding output gain. INIT without active modulators also remains unchanged.

Adding harmonics can change RMS, peaks and aliasing despite unchanged carriers.
The existing engine/mixer limits still apply; there is no universal loudness or
anti-aliasing guarantee. Device listening/timing and voice-steal transitions still
need hardware validation. The standard build is validated, not every optional
resident/overclock profile.
