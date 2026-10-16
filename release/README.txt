JackDAW stack for Haiku
=======================

JackDAW, the JACK audio server and its tools, and plug-ins: NAMku (Neural Amp
Modeler), DRUMku (drum sampler), hktuner, and LV2 and VST3 hosting. With Wine
and vstbridge it also runs Windows VST2 and VST3 plug-ins, including ones
protected by iLok (PACE).

The full install also replaces Haiku's system package with one that adds:
- a USB Audio Class 2.0 driver, for class-compliant USB audio interfaces
- 32-bit Windows program support, which installers such as iLok's need
- the debugging support that copy-protected plug-ins check for

That system package is built for one Haiku nightly only (see release.conf).
The installer moves your system to that nightly and keeps it there.

Install
-------
Open Terminal in this folder and run:

    sh install.sh

It checks the system, updates Haiku to the right nightly if needed, and
installs everything. When it says so, reboot and run it again to finish.

On a stock Haiku, or without the USB driver and 32-bit support:

    sh install.sh --apps-only

If something goes wrong
-----------------------
The script stops with a "!!" line and tells you what to do; nothing after
that point has been changed.

If Haiku does not start after the reboot: restart and hold Shift (or Space)
to open the boot menu. Choose "Select state" and pick the newest state from
before the installation. That boots the system as it was, with the official
packages.

Notes
-----
- Do not run "pkgman full-sync": it replaces this Wine with the HaikuPorts
  build and removes vstbridge. "pkgman update" is safe.
- Haiku updates wait for the next release of this stack, because the system
  package fits only one nightly.
- Your Wine prefix and its plug-in licences are kept. The installer backs up
  the Wine registry to ~/wine-registry-backup-<date> first.
