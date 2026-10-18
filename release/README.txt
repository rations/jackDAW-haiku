JackDAW stack for Haiku
=======================

Audio production on Haiku: JackDAW (a multitrack DAW), the JACK audio server
and its tools, Jack Graph (a patchbay), LV2 and VST3 hosting, and the plug-ins
NAMku (Neural Amp Modeler), DRUMku (drum sampler) and hktuner. With Wine and
VSTBridge it also runs Windows VST2 and VST3 plug-ins.

The full install also replaces Haiku's system package with a build that adds:
- a USB Audio Class 2.0 driver, for class-compliant USB audio interfaces
- 32-bit Windows program support (many plug-in installers need it)
- the self-debugging support that iLok (PACE) protected plug-ins check for

That system package contains all of Haiku, so it fits exactly one nightly,
named in release.conf. The installer moves your system to that nightly and
keeps it there.

Requirements: 64-bit (x86_64) Haiku, an internet connection for pkgman, and
this folder in a path without spaces (your home folder is fine).


Install
-------
Open Terminal in this folder and run:

    sh install.sh

The script checks the system, shows what it is going to do, and asks before
changing anything. When it asks you to, reboot and run it again. The second
run tests the new kernel and sets up Wine.

To see what it would do without changing anything:

    sh install.sh --check

To install only the apps and Wine on an unmodified Haiku (no USB audio driver,
no 32-bit Windows programs, no iLok plug-ins):

    sh install.sh --apps-only


After installing
----------------
1. Open Jack Graph (Deskbar > Applications) and then Jack Settings. Pick your
   interface (USB interfaces show up as usb/1 and so on), 48000 Hz, and press
   Start. 256 frames/period with 3 periods is a safe start.
2. Start JackDAW.
3. Windows plug-ins: install them with wine (for example
   "wine ~/Downloads/SomePluginSetup.exe"), then open VSTBridge, add the
   plug-in folder and press Sync. They then appear in JackDAW.


If something goes wrong
-----------------------
The script stops with a "!!" line that says what to do. Nothing after that
point has been changed.

If Haiku does not start after the reboot: restart and hold Shift to open the
boot menu. Choose "Select state" and pick the newest state from before the
installation. That starts the system as it was, with the official packages.

Logs: ~/jackdaw-stack-install.log (Wine setup) and
~/jackdaw-stack-kernel-test.log (kernel self-test).


Uninstall
---------
    sh install.sh --uninstall

This removes the stack's packages, points the Haiku repository back at the
current nightly and updates Haiku to it. Your Wine prefix (~/.wine) and your
JackDAW projects are kept.


Notes
-----
- Do not run "pkgman full-sync". It replaces this Wine with the HaikuPorts
  build and removes VSTBridge. "pkgman update" is safe.
- Haiku updates wait for the next release of the stack, because the system
  package fits only one nightly.
- Before setting up Wine, the installer backs up the Wine registry, where
  some plug-ins keep their licences, to ~/wine-registry-backup-<date>.
