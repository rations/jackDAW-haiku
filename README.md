# JackDAW (Haiku)

A native Haiku multitrack digital audio workstation, built on the Interface Kit, using
JACK for audio and MIDI. It is the top of a stack of native Haiku audio projects — see
[STACK.md](STACK.md) for how the pieces fit together.

## Features

- Audio and MIDI tracks, recording, and a timeline with clips and regions
- Piano-roll MIDI editor
- Mixer with per-track volume/mute/solo and metering
- VST3, LV2 and VST2 effects and instruments, with their own editors in the FX window
- Windows VST2/VST3 plug-ins through vstbridge and Wine, including iLok-protected ones
- MIDI control-surface mapping (learn a footswitch/CC to transport, mute, FX bypass, ...)
- Project save/load (`.jdaw` bundles) and offline/realtime render/export

## Install

Download the stack release from the
[releases page](https://github.com/rations/jackDAW-haiku/releases), unzip it, and run
`sh install.sh` in Terminal. It installs JackDAW together with everything it uses:
- the JACK server
- the patchbay
- the plug-ins
- Wine with vstbridge for Windows plug-ins
- a Haiku system package with a USB Audio Class 2.0 driver and 32-bit Windows support

[STACK.md](STACK.md) explains the pieces and how they are built.

## Requirements

- Haiku x86_64. The full install moves the system to the nightly the release is built for
  (0.5: hrev60256); `install.sh --apps-only` works on any recent nightly.
- A running **JACK server**. Start it from Jack Graph → Jack Settings (pick your interface), or
  from Terminal:
  ```
  jackd -X haikumidi -d hmulti -d /dev/audio/hmulti/usb/1 -r 48000 -p 256 -n 3
  ```
- VST3, LV2 and VST2 plug-ins are picked up from the system and user `add-ons/media/VST3`,
  `media/LV2` and `media/vstplugins` directories, so the `namku`, `drumku` and `hktuner`
  packages need no extra configuration.

## Building

Native on Haiku (x86_64), after jack-port-haiku, VST3-haiku and LV2-haiku (see STACK.md for
the order):
- `packaging/make-hpkg.sh` builds the package; the version comes from `VERSION`.
- `./build-from-source.sh` builds and installs into `/boot/system/non-packaged`.

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
