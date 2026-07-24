# The JackDAW audio stack on Haiku

A set of native Haiku projects that together turn a stock Haiku install into a working
JACK-based recording/production system with a DAW and VST3 plug-ins. This document
explains what each piece is and the order to install them.

Everything targets **Haiku nightly hrev59867, x86_64**. The USB-audio driver is ABI-tied
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
        ├─► vstbridge-haiku ──► vstbridge   Wine bridge: Windows VST2 + VST3 (needs wine)
        │  (vendors the SDK)                 → native Haiku stub add-ons the DAW loads
        │
  LV2-haiku ────────────────► lv2_haiku   LV2 stack (lilv/serd/sord/sratom/zix,
        │  (static libs, MIT/ISC)           spec bundles), JACK LV2 host + tools
        ├─► hktuner ─────────► hktuner      chromatic tuner LV2 (native GUI)
        │
  jackDAW-haiku ────────────► jackdaw     the multitrack DAW (hosts VST3 + LV2 + VST2)
```

`lv2_haiku` is both a build-time and a runtime dependency of `jackdaw`: the libraries are
linked statically, but lilv needs the LV2 **specification bundles** it installs to
`add-ons/media/LV2` (on lilv's compiled-in default `LV2_PATH`; see the path conventions
below) to load any plug-in.

| Package | Repo | Role | License |
|---|---|---|---|
| (driver) | haiku-kernel-usb | UAC2 USB-audio support (non-packaged override) | fork of Haiku (MIT) |
| `jack` | jack-port-haiku | JACK2 server, libjack, audio/MIDI backends | GPL v2 / LGPL v2.1 |
| `jack_tools` | jack-example-tools | standard JACK CLI utilities | GPL v2 |
| `jackgraph` | jack-graph-haiku | native patchbay + server control GUI | MIT |
| `vst3_haiku` | VST3-haiku | JACK VST3 host + example plug-ins (+ the SDK) | MIT |
| `namku` | NAMku | Neural Amp Modeler VST3 | MIT (+ MPL 2.0, Eigen) |
| `drumku` | DRUMku | MIDI drum-sampler VST3 | MIT |
| `vstbridge` | vstbridge-haiku | Wine bridge for Windows VST2/VST3 → native stubs (requires `wine`) | GPL v3 (yabridge fork) |
| `lv2_haiku` | LV2-haiku | LV2 stack + spec bundles, JACK LV2 host + tools | ISC |
| `hktuner` | hktuner | chromatic tuner LV2 plug-in | MIT |
| `jackdaw` | jackDAW-haiku | the DAW | MIT |

## Plugin & data path conventions

Every host and plug-in in this stack uses the **canonical Haiku add-on locations** — a
`media/` segment under the add-ons directory, with the format name as the leaf. These are
the *only* directories a host searches and the *only* directories a plug-in installs to:

| Format | Install dir (user, non-packaged)                     | Discovery                                                    |
|--------|------------------------------------------------------|-------------------------------------------------------------|
| VST2   | `~/config/non-packaged/add-ons/media/vstplugins`     | `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/vstplugins/")` |
| VST3   | `~/config/non-packaged/add-ons/media/VST3`           | `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/VST3/")`  |
| LV2    | `~/config/non-packaged/add-ons/media/LV2`            | `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/LV2/")` / `LV2_PATH` = those dirs |
| CLAP   | `~/config/non-packaged/add-ons/media/CLAP`           | `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/CLAP/")`  |

Rules, binding across all repos:

- **No dot-folders.** Never `~/.vst`, `~/.vst3`, `~/.clap`, `~/.lv2`, or `~/.jackdaw`. Haiku
  organises per-user files under the `~/config` hierarchy; use it.
- **Discovery goes through `find_paths`/`BPathFinder`** with the subpaths above — never a
  hardcoded `/boot/...` path. One `find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, …)` call already
  resolves every install location: system packaged, system non-packaged, and the user's
  `~/config/non-packaged`. The matching packaged dir is `add-ons/media/<fmt>` under each
  install root (`/boot/system`, `/boot/system/non-packaged`, `~/config/non-packaged`).
- **VST2 is loaded as a native Haiku add-on.** Its entry symbol is resolved as
  `VSTPluginMain`, then the BeOS default `main_plugin`, then legacy `main`. VST2/BeOS
  plug-ins usually carry **no file extension** — discovery must not require `.so` (the `.so`
  some carry is an artifact of the vstbridge Wine stubs).
- **User data/settings** go through `find_directory(B_USER_SETTINGS_DIRECTORY)` /
  `B_USER_*` — e.g. JackDAW scratch recordings live under
  `~/config/settings/JackDAW/recordings`, not a dot-folder.

`vstbridge` installs the native stub add-ons it generates for Windows plug-ins into the same
`add-ons/media/{vstplugins,VST3}` locations (under a `vstbridge/` subfolder), so the DAW
finds bridged and native plug-ins through the one discovery path.

## vstbridge — Windows plug-ins

`vstbridge` (a Haiku port of yabridge) is optional and independent of the rest of the stack: it
adds Windows plug-ins to a working install, and nothing else here links against it.

- **Formats: VST2 and VST3 only.** There is no CLAP bridge on Haiku — upstream's CLAP bridge
  builds on the X11 editor layer this port drops, so the package is built with `-Dclap=false`
  and ships the VST2 + VST3 chainloader/bridge libraries only. `add-ons/media/CLAP` stays
  reserved for later.
- **Wine is required, and it must be the patched build.** `wine` is a hard dependency in the
  `.PackageInfo`, and each bridged plug-in runs in a Wine host process (`vstbridge-host.exe`)
  that the chainloader launches. Use the **`wine-11.8-3-x86_64.hpkg` shipped with this stack**
  — it carries Haiku fixes that are not in any other Wine build, the essential one being running
  static initialisers in Winelib modules (the bridge host is a C++ Winelib module). Install it
  in the same command as the bridge so pkgman resolves the dependency locally:

  ```sh
  pkgman install ./wine-11.8-3-x86_64.hpkg ./vstbridge-*.hpkg
  ```

  It is built from the recipe in `haiku-wine-port` (a patched copy of the HaikuPorts
  `app-emulation/wine` port); rebuild it there for a different Haiku revision.
- **Two front-ends, same job.** `VSTBridge` is a native Interface Kit application installed to
  `apps/` and linked into *Deskbar → Applications* (via `data/deskbar/menu/Applications`, the
  way every stock Haiku app gets there). `vstbridgectl` is the equivalent CLI. Either one scans
  the Windows plug-in directories you configure and syncs one native stub add-on per plug-in
  into `add-ons/media/{vstplugins,VST3}/vstbridge`; JackDAW then loads them through its normal
  discovery path. The upstream GTK GUI is Linux-only and is not built or packaged here.
- **Wine graphics driver: use `null`.** The editor is bridged by capturing the plug-in's window
  and repainting it in JackDAW's FX window; the plug-in's own on-screen window is never wanted.
  The `null` driver declines to create a driver window surface, which sends win32u down its
  offscreen path — the window still gets somewhere to draw and still receives `WM_PAINT`, but
  never reaches a display. (Hiding the window instead does not work: an invisible window gets no
  surface and stops painting.) It is chosen once per prefix, by explorer when it creates the
  desktop, from `HKCU\Software\Wine\Drivers\Graphics`:

  ```sh
  wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f
  ```

  Any other driver still works — the editor is simply visible twice. On the HaikuPorts Wayland
  driver (`winewayland.drv`, what the port builds with, `--with-wayland --without-x`) a bridged
  plug-in opens a floating window *and* renders in the FX window. That driver is the one to keep
  if the prefix also runs ordinary Windows programs, since `null` leaves those with no window at
  all. The bridge logs a warning naming the driver whenever it is not `null`
  (`src/wine-host/editor-haiku.cpp`).
- **No `WINELOADER` export needed.** Wine 11.8-3 resolves its loader through Haiku's
  `find_path()` rather than `/proc`, so a bare `wine` picked up from `PATH` works from any
  working directory. Earlier builds needed `export WINELOADER=/boot/system/bin/wine` in the
  profile because winegcc's generated `.exe` wrapper scripts fall back to bare `wine`; that
  workaround is now unnecessary (vstbridge still honours `WINELOADER` if you set it, e.g. to
  point different prefixes at different Wine builds).

## Getting the sources

If git is not installed, install it first, then clone the repos you need
(the machine must be online — `pkgman` also pulls each package's runtime deps on install):

```sh
pkgman install -y git
```

Prebuilt `.hpkg`s are attached to each project's GitHub release (and all nine to the
[jackDAW-haiku full-stack release](https://github.com/rations/jackDAW-haiku/releases)), so
for the packaged install you only need to clone what you build from source. The USB-audio
driver is always built locally and additionally needs the Haiku source on the
`usb-audio-uac2` branch — its clone commands are in `haiku-kernel-usb/dist/INSTALL.md`
(section 0).

## Install order (prebuilt packages)

1. **USB-audio driver** (only if using a USB interface) — build + install per
   `haiku-kernel-usb/dist/INSTALL.md` (which starts by installing `git` and cloning the
   `usb-audio-uac2` Haiku source), then reboot. Verify: the interface enumerates and a node
   appears under `/dev/audio/hmulti/`.
2. **Everything else** — build each `.hpkg` on the nightly with its
   `packaging/make-hpkg.sh` (build order: jack → tools/graph/vst3_haiku/lv2_haiku →
   namku/drumku/hktuner → jackdaw), then install the lot; `pkgman` pulls external deps
   (glib2, libsndfile, libsamplerate) from HaikuPorts:
   ```
   pkgman install ./jack-*.hpkg ./jack_tools-*.hpkg ./jackgraph-*.hpkg \
                  ./vst3_haiku-*.hpkg ./lv2_haiku-*.hpkg \
                  ./namku-*.hpkg ./drumku-*.hpkg ./hktuner-*.hpkg ./jackdaw-*.hpkg
   ```
3. **`vstbridge`** (optional, only for Windows VST2/VST3 plug-ins) — kept out of the group
   install because it needs the patched Wine, which is large (~360 MB) and only useful here:
   ```
   pkgman install ./wine-11.8-3-x86_64.hpkg ./vstbridge-*.hpkg
   ```

## Install order (from source)

Each repo has a `build-from-source.sh` that installs into `/boot/system/non-packaged`.
Run them in dependency order (later ones link against the earlier ones):

```
jack-port-haiku      # libjack -> /boot/system/non-packaged
jack-example-tools   # needs jack
jack-graph-haiku     # needs jack
VST3-haiku           # SDK static libs + vst3jackhost
LV2-haiku            # lilv/serd/sord/sratom/zix static + spec bundles + lv2jackhost
NAMku                # needs the VST3-haiku SDK
DRUMku               # needs the VST3-haiku SDK
hktuner              # needs the LV2-haiku staged headers
jackDAW-haiku        # needs jack + the VST3-haiku SDK + the LV2-haiku stack
vstbridge-haiku      # optional; needs wine (winegcc) + meson/ninja, vendors its own SDK
```

`vstbridge-haiku` builds with `packaging/make-hpkg.sh` rather than a `build-from-source.sh`:
meson/ninja produce the bridge libraries and the Winelib host, `make -C tools/vstbridgectl`
produces both front-ends, and the script stages `apps/VSTBridge`, `bin/vstbridgectl`, the Wine
host and the `lib/` bridges into one `.hpkg`.

## Run
Launch **JackGraph** (to wire ports / manage the server) and **JackDAW** from Deskbar
(Applications). NAMku/DRUMku appear in JackDAW's VST3 plug-in list automatically, and
hktuner in its LV2 list; plug-ins that ship a native editor open it inside the FX window.

Start the JACK server in Jack Graph > Jack Settings (adjust the capture device path to your interface):

or start Jack with the terminal

```
jackd -X haikumidi -d hmulti -d /dev/audio/hmulti/usb/1 -r 48000 -p 128 -n 3
```

## Notes

- The HaikuPorts `.recipe` files (in each repo's `packaging/`) are drafted but not yet
  validated — run `haikuporter -c <name>` on the target and pin each `SOURCE_URI` before
  relying on them.
- The MIT/permissive licensing is clean: the VST3 SDK 3.8 is MIT, and JACK client apps
  link `libjack` (LGPL v2.1), so nothing forces copyleft on the MIT projects. `vstbridge` is
  the exception: it is a GPL v3 yabridge fork (separate package, links nothing into the others).
- **Versions:** the coordinated add-ons/media cutover is released as `0.3.0-1` across the whole
  updated stack (`jackdaw`, `vst3_haiku`, `lv2_haiku`, `namku`, `drumku`, `hktuner`); `vstbridge`
  starts at `0.1.0-1`. `jack`/`jack_tools`/`jackgraph` were unchanged. The `-1` is the package
  revision, which the Haiku `.PackageInfo` format requires; bump the **version** (`0.3.0` →
  `0.4.0`) for changes rather than the revision. Never reuse a published version — `pkgman`
  compares the label, not the bits.
- **`vstbridge` is new and lightly tested.** The package installs and both front-ends launch,
  but the bridge has not been through a broad plug-in sweep. `VSTBridge` has no vector icon yet,
  so it shows the generic executable icon in Deskbar/Tracker. See the vstbridge section above
  for the VST2/VST3-only and Wine-version caveats.
