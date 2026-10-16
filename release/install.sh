#!/bin/sh
# Install the JackDAW stack on Haiku. Run it from the release folder, in Terminal:
#
#   sh install.sh               everything: the system package (USB audio driver
#                               and 32-bit Windows support) and the apps
#   sh install.sh --apps-only   the apps only, on a stock Haiku
#
# The full install needs a reboot in the middle: run the script, reboot when
# it says so, then run it again. Every problem stops the script with a "!!"
# line, before anything is changed.
KIT=$(cd "$(dirname "$0")" && pwd)
. "$KIT/release.conf" || exit 1
	# HREV, the Haiku revision this release is built for, and VERSION
REPO="https://eu.hpkg.haiku-os.org/haiku/master/x86_64/r1~beta6_$HREV"
ADMIN=/boot/system/packages/administrative
PACKAGES=/boot/system/packages
PREFIX="${WINEPREFIX:-$HOME/.wine}"
LOG="$HOME/jackdaw-stack-install.log"

APPS_ONLY=
case "$1" in
--apps-only) APPS_ONLY=1 ;;
"") ;;
*) sed -n '2,10p' "$0"; exit 2 ;;
esac

fail() {
	echo "!! $*"
	exit 1
}

hrev_number() {
	echo "$1" | sed 's/^hrev//'
}

# Point the Haiku repository at this release's revision.
pin_haiku_repo() {
	yes | pkgman drop-repo Haiku > /dev/null 2>&1
	# add-repo reports the cache of the repository just dropped as missing;
	# that is expected, not an error.
	out=$(pkgman add-repo "$REPO" 2>&1)
	rc=$?
	printf '%s\n' "$out" | grep -v 'Failed to open repository file'
	[ $rc = 0 ] || fail "could not add the Haiku repository for $HREV"
}

# --- checks that change nothing ----------------------------------------------

[ "$(uname -s)" = Haiku ] || fail "this installs on Haiku"
case "$KIT" in
*" "*) fail "move this folder to a path without spaces (for example your home" \
	"folder), then run the script again" ;;
esac
[ "$(getarch -p 2>/dev/null)" = x86_64 ] || fail "this release is for 64-bit (x86_64) Haiku"
(cd "$KIT" && sha256sum -c --quiet SHA256SUMS) \
	|| fail "the release files are damaged or incomplete; download them again"

# Some unzip tools drop the execute bit.
chmod +x "$KIT/tools/wow64_ldt_test" 2>/dev/null

# The kernel self-test passes only on the system package of this release, once
# it is running.
kernel_is_ours() {
	"$KIT/tools/wow64_ldt_test" 1 > /dev/null 2>&1
}

RUNNING=$(uname -v | awk '{print $1}')
echo ">> JackDAW stack $VERSION for $HREV. This system: $RUNNING."

# --- after the reboot: finish the full install -------------------------------

finish() {
	echo ">> 1/3 kernel self-test (32-bit code on this CPU, about a minute)"
	if "$KIT/tools/wow64_ldt_test" > "$HOME/jackdaw-stack-kernel-test.log" 2>&1; then
		echo "   all passed"
	else
		tail -5 "$HOME/jackdaw-stack-kernel-test.log"
		fail "the kernel self-test failed; please report it with" \
			"$HOME/jackdaw-stack-kernel-test.log"
	fi
	update_prefix 2/3
	checks 3/3
}

# Wine installs its 32-bit files into a prefix only while creating it. An
# existing prefix from before 32-bit support is brought up to date the same
# way, by removing its update stamp.
update_prefix() {
	echo ">> $1 updating the Wine prefix $PREFIX (a few minutes, windows may flash)"
	wineserver -k 2>/dev/null
	if [ -d "$PREFIX" ]; then
		B="$HOME/wine-registry-backup-$(date +%Y%m%d-%H%M%S)"
		mkdir -p "$B" && cp "$PREFIX"/*.reg "$B"/ \
			&& echo "   registry (plug-in licences) backed up to $B"
		[ -f "$PREFIX/.update-timestamp" ] && [ -z "$APPS_ONLY" ] \
			&& mv "$PREFIX/.update-timestamp" "$PREFIX/.update-timestamp.before-32bit"
	fi
	# Wine looks for its Mono installer in ~/.cache/wine before downloading it.
	mkdir -p "$HOME/.cache/wine"
	cp "$KIT"/tools/wine-mono-*-x86.msi "$HOME/.cache/wine/" 2>/dev/null
	WINEDEBUG=-all wine cmd /c ver > "$LOG" 2>&1
	wineserver -w
	if [ ! -d "$PREFIX/drive_c/windows/mono" ]; then
		echo "   installing Wine Mono"
		WINEDEBUG=-all wine msiexec /i "$(ls "$HOME"/.cache/wine/wine-mono-*-x86.msi | tail -1)" \
			/qn >> "$LOG" 2>&1
		wineserver -w
	fi
}

checks() {
	echo ">> $1 checks"
	echo "   wine: $(wine --version 2>/dev/null)"
	if [ -d "$PREFIX/drive_c/windows/mono" ]; then
		echo "   Wine Mono: installed"
	else
		echo "   Wine Mono: MISSING (.NET installers will fail)"
	fi
	if [ -z "$APPS_ONLY" ]; then
		ARCH=$(WINEDEBUG=-all wine 'C:\windows\syswow64\cmd.exe' /c echo %PROCESSOR_ARCHITECTURE% \
			2>/dev/null | tr -d '\r')
		wineserver -w
		echo "   32-bit Windows programs: ${ARCH:-not working} (expect x86)"
		echo "   audio devices: $(ls /dev/audio/hmulti 2>/dev/null | tr '\n' ' ')"
	fi
	echo
	echo "== Done. Next:"
	echo "   - Jack Graph (Deskbar > Applications): Jack Settings, pick your interface, Start"
	echo "   - JackDAW"
	echo "   - Windows plug-ins: install them with wine, then open VSTBridge and Sync"
	echo "   Problems: $LOG"
}

if [ -z "$APPS_ONLY" ] && [ "$RUNNING" = "$HREV" ] \
		&& ls "$PACKAGES"/haiku-*"$HREV"-2-x86_64.hpkg > /dev/null 2>&1; then
	if kernel_is_ours; then
		finish
		exit 0
	fi
	fail "the system package is installed but not running yet. Reboot, then run" \
		"this script again."
fi

# --- before installing: the system must be at the release's revision ----------

# It lists one package file per line; an unclean shutdown once filled it with
# other text, and pkgman then tried to reinstall the whole system.
if grep -q -v -E '^$|\.hpkg$' "$ADMIN/activated-packages" 2>/dev/null; then
	mv "$ADMIN/activated-packages" "$ADMIN/activated-packages.corrupt"
	fail "the list of active packages was damaged (an unclean shutdown can do" \
		"that). It was moved aside. Reboot, then run this script again."
fi

if [ "$RUNNING" != "$HREV" ]; then
	if [ "$(hrev_number "$RUNNING")" -gt "$(hrev_number "$HREV")" ]; then
		echo "!! This system is newer ($RUNNING) than the release ($HREV)."
		echo "   The system package replaces the whole of Haiku and only fits $HREV."
		echo "   Either use --apps-only, or move this system back to $HREV:"
		echo "     yes | pkgman drop-repo Haiku"
		echo "     pkgman add-repo $REPO"
		echo "     pkgman full-sync      (then reboot, and run this script again)"
		exit 1
	fi
	# Wine is built on $HREV and needs at least that revision.
	if [ -n "$APPS_ONLY" ]; then
		echo ">> updating Haiku first (answer y)"
	else
		echo ">> updating Haiku to $HREV first (answer y)"
		pin_haiku_repo
	fi
	pkgman update || fail "the update failed"
	echo
	echo "== Reboot now, then run this script again."
	exit 0
fi

if [ -z "$APPS_ONLY" ]; then
	# Media-OS replaces the USB stack, and its leftovers load before ours.
	[ -e /boot/system/non-packaged/add-ons/kernel/busses/usb/xhci ] \
		&& fail "Media-OS is (partly) installed. Remove it first with" \
			"/boot/system/non-packaged/data/mediaos-usb/uninstall.sh, then reboot."
	# The driver overrides of earlier releases would load into the new kernel.
	if [ -e "$HOME/config/non-packaged/add-ons/kernel/drivers/bin/usb_audio" ] \
			|| [ -e "$HOME/config/non-packaged/add-ons/kernel/busses/usb/xhci" ] \
			|| [ -f /boot/system/settings/packages ]; then
		echo ">> removing the USB audio driver overrides of an earlier release"
		sh "$KIT/tools/install-driver.sh" --uninstall || exit 1
	fi
	# Keep the system on this revision: the system package fits only this one.
	if ! pkgman list-repos | grep -q "base-url: *$REPO\$"; then
		echo ">> pinning the Haiku repository to $HREV"
		pin_haiku_repo
	fi
fi

# --- install -------------------------------------------------------------------

NEED=
for p in "$KIT"/packages/*.hpkg; do
	# Installing a version that is already installed fails, so skip those.
	[ -e "$PACKAGES/$(basename "$p")" ] || NEED="$NEED $p"
done
if [ -z "$APPS_ONLY" ]; then
	NEED="$NEED $(ls "$KIT"/system/haiku-*-x86_64.hpkg)"
	# haiku_devel must match haiku exactly; only update it if it is installed.
	if ls "$PACKAGES"/haiku_devel-*.hpkg > /dev/null 2>&1; then
		NEED="$NEED $(ls "$KIT"/system/haiku_devel-*-x86_64.hpkg)"
	fi
fi

wineserver -k 2>/dev/null
if [ -n "$NEED" ]; then
	echo ">> installing (answer y):"
	for p in $NEED; do echo "   $(basename "$p")"; done
	# shellcheck disable=SC2086
	pkgman install $NEED || fail "the installation failed; nothing was changed"
fi

if [ -n "$APPS_ONLY" ]; then
	update_prefix 1/2
	checks 2/2
	exit 0
fi

echo
echo "== Reboot now, then run this script again to finish:"
echo "   sh $KIT/install.sh"
echo
echo "If Haiku does not start after the reboot: restart, hold Shift (or Space) to"
echo "get the boot menu, choose \"Select state\", and pick the newest state before"
echo "today's. That is the system as it was before this installation."
