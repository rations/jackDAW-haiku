# JackDAW-Haiku stack 0.5 (hrev60256)

Native audio production on Haiku: JackDAW, a multitrack DAW built on Haiku's Interface Kit,
with the JACK server, a patchbay, VST3, LV2 and VST2 hosting, three plug-ins (**NAMku**,
**DRUMku**, **hktuner**), and **Windows plug-ins through Wine**, including iLok-protected
ones.

**New in 0.5: one download, one script.** Earlier releases needed a kernel driver built from
source and a boot-menu change on every boot. Everything now comes as packages, installed by
`install.sh`:

```sh
sh install.sh            # run it, reboot when it says so, run it again
```

Looking for testers with a class-compliant USB Audio Class 2.0 interface, and for reports on
Windows plug-ins.

---

## What's new since 0.4

### The USB audio driver is part of the system

The UAC2 driver used to be a non-packaged override: you built it from source for your exact
nightly, and then disabled the stock `xhci` in the boot menu on every boot. It now ships
inside a custom build of Haiku's own system package, together with the kernel changes below.
The installer puts it in place with pkgman, as an update to the official package. Media-OS is
no longer needed (remove it first if you installed it).

The system package is built for **hrev60256** only, because it contains all of Haiku. The
installer updates your system to that nightly and pins the Haiku repository there.

### 32-bit Windows programs

Wine can now run 32-bit Windows code. Many installers and licence tools need it, even for
64-bit plug-ins: iLok's License Support installer (Bonjour and its custom actions) and
Toontrack's licence manager, among others. This needs the custom system package, which adds
per-team local descriptor tables to the Haiku kernel. That is the same approach FreeBSD uses
for Wine.

### iLok (PACE) protected plug-ins

Plug-ins protected by PACE/iLok check that the process can debug itself: they set the trap
flag and hardware breakpoints on their own threads, and expect the exceptions Windows would
raise. On stock Haiku those traps were dropped, so these plug-ins crashed, or reported no
licence. The system package delivers them, and Wine 11.19-4 passes them on as Windows
does. Tested: iLok License Manager, and the Neural DSP Archetype Gojira X and Fortin NTS
VST3s, licensed, in JackDAW.

### Wine 11.19-4 (was 11.8-10)

Rebased onto Wine 11.19, with 17 patches (see
[haiku-wine-port](https://github.com/rations/haiku-wine-port-jackdaw)). Besides 32-bit code
and PACE support:
- **Registry changes persist.** wineserver crashed at every shutdown, losing registry changes
  since its last periodic save. That included licences kept in the registry.
- **Named pipes no longer fail with `ERROR_ALERTED`.** Wine can now signal a single thread
  on Haiku. Installers built with WiX Burn lost their engine connection without it.
- **.NET installers work.** Wine Mono installs, because Windows symbolic links are stored in
  BFS attributes. The installer adds Wine Mono to your prefix.
- **iLok installer fixes:** service failure actions, Windows 10 manifests for
  msiexec/rundll32, and TLS through GnuTLS, which is now a declared dependency.

### The apps

- **JackDAW 0.6.0:**
  - The MIDI control input also reaches effect plug-ins' own MIDI features. One footswitch
    press can mute a track and switch the amp plug-in's channel.
  - VST3 instruments get Program Change.
  - LV2 editors can send files and state to their plug-in (atom ports), and their minimum
    size is respected.
  - The FX window's Load model / Load IR buttons are gone: NAMku's own editor has them.
  - The app shows its version in Tracker's Get Info.
- **NAMku 0.4.0:**
  - Large host blocks are processed in full; their tail used to hold stale audio.
  - Correct latency reporting.
  - Output modes are offered according to what the loaded model supports.
- **DRUMku 0.4.0:**
  - A kit rack page reaches all 64 slots; the pads reach slots 1–9.
  - Notes in large host blocks are no longer lost.
- **hktuner 0.4.0:** the tuner readout works in hosts that follow the LV2 UI specification
  strictly.
- **Jack Graph 0.2:**
  - Dragging, clicking and disconnecting cables are reliable, and audio and MIDI cannot be
    cross-connected.
  - The wheel zooms around the pointer.
  - It copes with the server going away.
- **vstbridge 0.2.0:**
  - Built for Wine 11.19: mouse motion reaches the plug-in at once, not on the next click.
  - An optional fix for JUCE popup menus.
- **JACK 1.9.23-7:**
  - No D-Bus dependency.
  - Works with drivers that ignore the requested buffer count; `jackd` crashed on those
    before.

## Install

1. Download `jackdaw-stack-0.5-hrev60256.zip`, unzip it, and open a Terminal in the folder.
2. Run `sh install.sh`. It checks the system, and stops with a `!!` line on any problem,
   before changing anything. If your nightly is older than hrev60256, it updates Haiku first
   and asks you to reboot and run it again.
3. When it says so, reboot and run `sh install.sh` again. The second run tests the new
   kernel (about a minute) and updates the Wine prefix.

`sh install.sh --apps-only` installs only the apps and Wine, on a stock Haiku: no USB
audio driver, no 32-bit Windows programs, and no iLok plug-ins.

**If Haiku does not start** after the reboot, hold Shift (or Space) at power-on to get the boot
menu, choose *Select state*, and pick the newest state from before the installation. pkgman
keeps that state, with the official system package.

Then start JACK from **Jack Graph → Jack Settings** (interface `usb/1`, 48000 Hz,
256 frames/period, 3 periods), and launch JackDAW. For Windows plug-ins, install them with
`wine`, then open **VSTBridge** (Deskbar → Applications), add the plug-in folder and sync.

## Upgrading from 0.4

- The installer removes the old driver override and its `xhci` blocklist. You no longer
  need the boot-menu `xhci` setting.
- Remove Media-OS before installing, if you tried it: run its `uninstall.sh`, then reboot.
- Wine 11.19 keeps your prefix and its licences. The installer backs up the registry to
  `~/wine-registry-backup-<date>` before touching the prefix.
- The `wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null` advice from 0.4 still
  applies: with the `null` driver a bridged editor appears only in JackDAW's FX window.

## Known issues

- **Do not run `pkgman full-sync`.** It replaces this Wine with the HaikuPorts build and
  removes vstbridge. `pkgman update` is safe.
- **Haiku updates wait for the next release.** The system package fits hrev60256 only, and
  the Haiku repository stays pinned to it.
- The custom system package leaves out the Haiku logo artwork and the Haiku3d demo, which
  only official builds may include. It also leaves out the BIOS and network-boot loaders
  that Installer copies to a new disk; this system still boots normally, on BIOS and UEFI.
- **Heavy plug-ins:** Fortin NTS runs, but uses a lot of CPU. Raise Frames/period if you hear
  xruns.
- **vstbridge bridges VST2 and VST3 only** (no CLAP). Editors drawn with Direct3D need the
  Wayland graphics driver, which also shows the plug-in's own window.
- LV2 instruments are not loadable yet. LV2 editors other than native Haiku UIs are not
  supported.
- The plug-in scan cache is keyed by path and modification time: use
  *Options → Rescan Plugins* if a replaced plug-in does not show up.

Reports welcome. Please include your hrev, your USB interface (its ID from `listusb`), your
JACK buffer settings, and for Windows plug-ins the plug-in name and version.

## Built from

| Package | Source |
|---|---|
| `haiku`, `haiku_devel` (r1~beta6_hrev60256-2) | [rations/haiku](https://github.com/rations/haiku), branch `jackdaw-stack-0.5-hrev60256`; build script in [haiku-kernel-usb](https://github.com/rations/haiku-kernel-usb) |
| `wine` 11.19-4 | [haiku-wine-port-jackdaw](https://github.com/rations/haiku-wine-port-jackdaw) |
| `jack` 1.9.23-7 | [jack-port-haiku](https://github.com/rations/jack-port-haiku) |
| `jack_tools` 4-2 | [jack-example-tools](https://github.com/rations/jack-example-tools) |
| `jackgraph` 0.2-1 | [jack-graph-haiku](https://github.com/rations/jack-graph-haiku) |
| `vst3_haiku` 0.3.0-2 | [VST3-haiku](https://github.com/rations/VST3-haiku) |
| `lv2_haiku` 0.3.0-2 | [LV2-haiku](https://github.com/rations/LV2-haiku) |
| `namku` 0.4.0-1 | [NAMku](https://github.com/rations/NAMku) |
| `drumku` 0.4.0-1 | [DRUMku](https://github.com/rations/DRUMku) |
| `hktuner` 0.4.0-1 | [hktuner](https://github.com/rations/hktuner) |
| `vstbridge` 0.2.0-1 | [vstbridge-haiku](https://github.com/rations/vstbridge-haiku) |
| `jackdaw` 0.6.0-1 | [jackDAW-haiku](https://github.com/rations/jackDAW-haiku) |

<!-- TODO before publishing: confirm the hardware results on hrev60256 (UMC204HD, Gojira,
Fortin, iLok); remove this comment. -->
