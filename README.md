# mpc-vst-robovox

**Robovox** – a robot vocoder as a VST2 **insert effect** for the Akai MPC OS plugin host
(Force, MPC Live/One/X/Key). Put it on an audio track whose input is the microphone: the voice is
the modulator, a built-in synth the carrier. Made for robotic, technoid voices – monotone droids,
auto-tuned robots, machine choirs, ring-modulated "exterminate" voices, bitcrushed units.

`vst/` is the plugin (see `vst/build.sh`), built the same way as `mpc-vst-fsvr`: one hand-written
VST2 shell, no JUCE, no GUI; the screen is an MPC skin (`vst/layout.conf`). No latency is added –
the mic is processed in the audio thread directly.

| Tab | What it does |
|---|---|
| VOICE | The carrier. CARRIER: SAW, SQUARE, PULSE, SUPER (three detuned saws per voice), BUZZ (narrow pulse), NOISE (whisper robot). PITCH: **FIXED** plays ROOT + CHORD all the time; **LATCH** – the last MIDI note becomes the root and stays; **MIDI** plays the held notes (up to 4), silent without one – the classic keyboard vocoder; **FOLLOW** tracks the singer's pitch and snaps it to SCALE (FREE, CHROMATIC, MAJOR, MINOR, PHRYGIAN, PENTA, relative to ROOT) – the auto-tuned robot. CHORD (MONO, OCTAVE, FIFTH, MINOR, MAJOR, SUS4, MIN7) applies in FIXED, LATCH and FOLLOW. OCTAVE ±2, GLIDE 0–400 ms, SPREAD (SUPER detune). |
| VOCODER | BANDS 8/12/16/20/24 (fewer = more robotic, more = clearer words). SHIFT ±12 semitones moves the carrier bands: down = giant mech, up = tiny bot. Q from wide to ringing. RELEASE 8–400 ms. SIBILANCE: the mic's highs (s, t, sh) straight through. UNVOICED: noise into the carrier while the mic is hissy, so consonants stay intelligible. FREEZE holds the current spectrum: sing a vowel, freeze, and it sustains as a pad without the mic. |
| ROBOT | CRUSH (sample rate + bit reduction), RING (ring modulator 20 Hz–1.8 kHz, 0 = off; ~30 Hz = the classic sci-fi robot), WIDTH (bands alternately left/right), GATE SYNC (trance gate on the host tempo: 1/4, 1/8, 1/16, 1/32; open while the transport is stopped). |
| IN/OUT | INPUT (L+R, LEFT, RIGHT – pick the side your mic is on if only one is), IN GAIN −12…+24 dB, NOISE GATE (−80…−20 dB, 0 = off – stops room noise and monitor bleed from opening the vocoder), MIX (0 = dry), OUTPUT ±12 dB with a soft knee above −3 dBFS. |
| PRESETS | 32 slots: PRESET knob + LOAD / SAVE. Slots 1–14 are factory sounds, 15–32 INIT. The slots are also the plugin's programs, so they show in the MPC preset list. SAVE writes `robovox_user/slot_NN.txt` next to the plugin (`/sdcard/vst/robovox_user/`). |

Factory slots: 1 CLASSIC ROBOT, 2 AUTOTUNE BOT, 3 CYLON, 4 RING DROID, 5 MACHINE CHOIR, 6 VOCODER 78,
7 WHISPER UNIT, 8 GIANT MECH, 9 NANO BOT, 10 FREEZE PAD, 11 BITCRUSH UNIT, 12 GOA ORACLE (FOLLOW in
Phrygian with a fifth), 13 TRANCE GATE (LATCH, minor chord, 1/16 gate), 14 MIDI KEYS.

Q-Links carry only knobs and selectors (no buttons or switches).

## Setup on the Force

1. Audio track, input = the mic's input (In 1, In 2 or the stereo pair), **monitoring on** (In or
   Auto) – an insert only processes live input while the track monitors it.
2. Robovox as an insert on that track. Start with slot 1 and speak: the robot answers in A2.
3. If nothing happens: IN GAIN up, or NOISE GATE down; INPUT = LEFT/RIGHT if the mic sits on one side
   of a stereo input.

**Playing the pitch from a MIDI track (LATCH, MIDI).** MPC OS sends no MIDI to insert effects, so
Robovox opens its own ALSA sequencer port **"Robovox MIDI In"** (the way Oneiroi FX does). Enable it in
Preferences → MIDI if it is not, then make a MIDI track with that port as its **output**; its notes
play the carrier, its program changes 0–31 load slots. Several Robovox instances each open their own port.

CC from a controller is not mapped (yet): the Q-Links and the MPC's own MIDI learn on the parameters work.

**CPU** (x86 benchmark, `vst/test.sh`): ~1.1 % of a core for 16 bands, ~1.3 % for 24 bands SUPER with a
4-note chord or FOLLOW; 0.1 % while idle (gate closed: the vocoder is skipped). Expect several times
that on the Force's Cortex-A17; the real load goes to `/tmp/robovox_vst.log` every ~5 s
(`ssh root@<device> cat /tmp/robovox_vst.log`).

Build/deploy workflow: see `sd88me/mpc-vst-plugins`' `docs/PORTING.md`; GitHub Actions: *VST release
(draft)* (`.github/workflows/vst-release.yml`).
License: GPL-3.0-or-later (the VST shell follows mpc-vst-fsvr).

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
