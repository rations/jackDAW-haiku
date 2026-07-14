# JackDAW (Haiku)

A native Haiku multitrack digital audio workstation, built on the Interface Kit, using
JACK for audio and MIDI. It is the top of a stack of native Haiku audio projects — see
[STACK.md](STACK.md) for how the pieces fit together and the recommended install order.

## Features

- Audio and MIDI tracks, recording, and a timeline with clips and regions
- Piano-roll MIDI editor
- Mixer with per-track volume/mute/solo and metering
- VST3 effects and instruments (hosts plug-ins like NAMku and DRUMku)
- MIDI control-surface mapping (learn a footswitch/CC to transport, mute, FX bypass, ...)
- Project save/load (`.jdaw` bundles) and offline/realtime render/export

## Requirements

- A running **JACK server** (the `jack` package / jack-port-haiku). Start it, e.g.:

Jack Graph > Jack Settings (adjust the capture device path to your interface)

- Or from the terminal
  ```
  jackd -X haikumidi -d hmulti -d /dev/audio/hmulti/usb/1 -r 48000 -p 128 -n 3
  ```
- If you use a **USB audio interface**, install the UAC2 driver first (haiku-kernel-usb) —
  a stock Haiku will not expose class-compliant USB audio.
- VST3 plug-ins are picked up from the system and user `add-ons/vst3` directories, so
  installing the `namku` / `drumku` packages makes them available with no extra config.

## Install

Native on Haiku (x86_64). Either:

- **Prebuilt package:** build once with `packaging/make-hpkg.sh`, then
  `pkgman install ./jackdaw-0.1-1-x86_64.hpkg` (external deps — glib2, libsndfile,
  libsamplerate — are pulled from HaikuPorts automatically).
- **From source:** `./build-from-source.sh` (build jack-port-haiku and VST3-haiku from
  source first — see STACK.md for the order).

A HaikuPorts recipe is in `packaging/jackdaw-0.1.recipe` (validate with
`haikuporter -c jackdaw`).

## MIDI CC / pitch-bend into VST3 instruments — needs hardware testing

JackDAW delivers MIDI CC, pitch-bend, and channel-pressure into VST3 instruments via the
plug-in's `IMidiMapping` (built at load, applied on the RT thread with pre-allocated
parameter queues). This path is implemented but has **only been verified for the MIDI
control surface** (a footswitch mapped through Options → MIDI Control). Driving a
**CC-aware VST3 synth** (mod-wheel / pitch-bend changing a plug-in parameter audibly) has
**not yet been confirmed on hardware** — it needs an instrument that implements
`IMidiMapping` and a controller that emits CC/pitch-bend. (DRUMku declares no CC
assignment, so it ignores CC by design.) Reports welcome.

## License

MIT (see [LICENSE](LICENSE)). Includes portions of the Steinberg VST3 SDK (MIT). Links
JACK's `libjack` (LGPL v2.1). VST is a trademark of Steinberg Media Technologies GmbH.
