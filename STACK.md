# The JackDAW audio stack on Haiku

A set of native Haiku projects that together turn a stock Haiku install into a working
JACK-based recording/production system with a DAW and VST3 plug-ins. This document
explains what each piece is and the order to install them.

Everything targets **Haiku nightly hrev59846, x86_64**. The USB-audio driver is ABI-tied
to that revision (rebuild it for any other nightly); the userland packages just need a
compatible Haiku.

## The pieces

```
  haiku-kernel-usb            USB Audio Class 2.0 driver (xhci + usb_audio + multi_audio)
        │                     -> makes class-compliant USB interfaces usable
        ▼
  jack-port-haiku  ─────────► jack        the JACK2 server + libjack + backends
        │                                  (foundation; everything below is a JACK client)
        ├─► jack-example-tools ─► jack_tools   jack_lsp / jack_connect / jack_transport / ...
        ├─► jack-graph-haiku   ─► jackgraph    native patchbay + start/stop/settings GUI
        │
  VST3-haiku ───────────────► vst3_haiku  JACK VST3 host + example plug-ins
        │  (VST3 SDK, static, MIT)          (SDK is a build-time dep of the DAW + plug-ins)
        ├─► NAMku  ───────────► namku       Neural Amp Modeler VST3 instrument/effect
        ├─► DRUMku ───────────► drumku      MIDI drum-sampler VST3 instrument
        │
  jackDAW-haiku ────────────► jackdaw     the multitrack DAW (hosts the VST3 plug-ins)
```

| Package | Repo | Role | License |
|---|---|---|---|
| (driver) | haiku-kernel-usb | UAC2 USB-audio support (non-packaged override) | fork of Haiku (MIT) |
| `jack` | jack-port-haiku | JACK2 server, libjack, audio/MIDI backends | GPL v2 / LGPL v2.1 |
| `jack_tools` | jack-example-tools | standard JACK CLI utilities | GPL v2 |
| `jackgraph` | jack-graph-haiku | native patchbay + server control GUI | MIT |
| `vst3_haiku` | VST3-haiku | JACK VST3 host + example plug-ins (+ the SDK) | MIT |
| `namku` | NAMku | Neural Amp Modeler VST3 | MIT (+ MPL 2.0, Eigen) |
| `drumku` | DRUMku | MIDI drum-sampler VST3 | MIT |
| `jackdaw` | jackDAW-haiku | the DAW | MIT |

## Install order (prebuilt packages)

1. **USB-audio driver** (only if using a USB interface) — build + install per
   `haiku-kernel-usb/dist/INSTALL.md`, then reboot. Verify: the interface enumerates and
   a node appears under `/dev/audio/hmulti/`.
2. **Everything else** — build each `.hpkg` on the nightly with its
   `packaging/make-hpkg.sh` (build order: jack → tools/graph/vst3_haiku → namku/drumku →
   jackdaw), then install the lot; `pkgman` pulls external deps (glib2, libsndfile,
   libsamplerate) from HaikuPorts:
   ```
   pkgman install ./jack-*.hpkg ./jack_tools-*.hpkg ./jackgraph-*.hpkg \
                  ./vst3_haiku-*.hpkg ./namku-*.hpkg ./drumku-*.hpkg ./jackdaw-*.hpkg
   ```

## Install order (from source)

Each repo has a `build-from-source.sh` that installs into `/boot/system/non-packaged`.
Run them in dependency order (later ones link against the earlier ones):

```
jack-port-haiku      # libjack -> /boot/system/non-packaged
jack-example-tools   # needs jack
jack-graph-haiku     # needs jack
VST3-haiku           # SDK static libs + vst3jackhost
NAMku                # needs the VST3-haiku SDK
DRUMku               # needs the VST3-haiku SDK
jackDAW-haiku        # needs jack + the VST3-haiku SDK
```

## Run

Start the JACK server (adjust the capture device path to your interface):

```
jackd -X haikumidi -d hmulti -d /dev/audio/hmulti/usb/1 -r 48000 -p 128 -n 3
```

Then launch **JackGraph** (to wire ports / manage the server) and **JackDAW** from Deskbar
(Applications). NAMku/DRUMku appear in JackDAW's VST3 plug-in list automatically.

## Notes

- The HaikuPorts `.recipe` files (in each repo's `packaging/`) are drafted but not yet
  validated — run `haikuporter -c <name>` on the target and pin each `SOURCE_URI` before
  relying on them.
- The MIT/permissive licensing is clean: the VST3 SDK 3.8 is MIT, and JACK client apps
  link `libjack` (LGPL v2.1), so nothing forces copyleft on the MIT projects.
