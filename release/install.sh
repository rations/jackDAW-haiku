#!/bin/sh
# Installer for the JackDAW stack on Haiku.
#
# Usage: sh install.sh [option]
#
#   (none)        Install everything: the system package (USB audio driver and
#                 32-bit Windows support) and the apps. Run it, reboot when it
#                 asks, then run it again to finish.
#   --apps-only   Install the apps and Wine only, on an unmodified Haiku.
#   --downgrade   On a nightly newer than the system package: move Haiku back
#                 to the nightly it is built for, then install everything.
#   --check       Check this system and show what would be done. Changes nothing.
#   --rollback    Show how to start the system as it was before the stack.
#   --uninstall   Remove the stack and go back to the official Haiku packages.
#   --yes         Do not ask before making changes.
#   --help        Show this text.
#
# Any problem stops the script with a "!!" line, before anything is changed.

KIT=$(cd "$(dirname "$0")" && pwd)
. "$KIT/release.conf" || exit 1
	# VERSION, HREV (the Haiku nightly the system package is built for) and
	# SYSREV (the system package's revision)

ARCH=x86_64
PINNED_REPO="https://eu.hpkg.haiku-os.org/haiku/master/$ARCH/r1~beta6_$HREV"
DEFAULT_REPO="https://eu.hpkg.haiku-os.org/haiku/master/$ARCH/current"
PACKAGES=/boot/system/packages
ADMIN=$PACKAGES/administrative
PREFIX="${WINEPREFIX:-$HOME/.wine}"
LOG="$HOME/jackdaw-stack-install.log"
STATE_DIR="$HOME/config/settings/jackdaw-stack"
# The package state pkgman saved before the stack's first change: the system
# as it was before.
STATE_FILE="$STATE_DIR/state-before-stack"

MODE=full
CHECK_ONLY=
DOWNGRADE=
YES=
for arg in "$@"; do
	case "$arg" in
	--apps-only) MODE=apps ;;
	--uninstall) MODE=uninstall ;;
	--rollback) MODE=rollback ;;
	--downgrade) DOWNGRADE=1 ;;
	--check) CHECK_ONLY=1 ;;
	--yes|-y) YES=1 ;;
	--help|-h) sed -n '4,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $arg (see sh install.sh --help)"; exit 2 ;;
	esac
done

# Marks a Wine prefix as set up by this release. The full install adds the
# 32-bit files, so it has its own mark.
PREFIX_DONE="$PREFIX/.jackdaw-stack-$VERSION"
[ "$MODE" = full ] && PREFIX_DONE="$PREFIX_DONE-32bit"

fail() {
	echo "!! $1"
	shift
	for line in "$@"; do echo "   $line"; done
	exit 1
}

step() {
	echo
	echo "== $*"
}

confirm() {
	[ -n "$YES" ] && return 0
	printf '%s [y/N] ' "$1"
	read -r answer
	case "$answer" in
	y|Y|yes|YES) return 0 ;;
	*) echo "Nothing was changed."; exit 1 ;;
	esac
}

hrev_number() {
	echo "$1" | sed 's/^hrev//'
}

point_haiku_repo_at() {
	yes | pkgman drop-repo Haiku > /dev/null 2>&1
	# add-repo complains about the cache of the repository just dropped;
	# that message is expected and filtered out.
	out=$(pkgman add-repo "$1" 2>&1)
	rc=$?
	printf '%s\n' "$out" | grep -v 'Failed to open repository file'
	[ $rc = 0 ] || fail "could not set the Haiku repository to $1"
}

haiku_repo_is() {
	pkgman list-repos 2>/dev/null | grep -q "base-url: *$1\$"
}

# Runs pkgman. LAST_STATE is set to the state it backed up, and the first such
# state is kept in STATE_FILE.
LAST_STATE=
run_pkgman() {
	before=$(ls -d "$ADMIN"/state_* 2>/dev/null)
	pkgman "$@"
	rc=$?
	for d in $(ls -d "$ADMIN"/state_* 2>/dev/null); do
		echo "$before" | grep -qxF "$d" || LAST_STATE=$(basename "$d")
	done
	if [ -n "$LAST_STATE" ] && [ ! -s "$STATE_FILE" ]; then
		mkdir -p "$STATE_DIR" && echo "$LAST_STATE" > "$STATE_FILE"
	fi
	return $rc
}

# state_2026-10-18_01:58:18 -> 2026-10-18 01:58:18
state_date() {
	echo "$1" | sed 's/^state_\([0-9-]*\)_\([0-9:]*\).*/\1 \2/'
}

rollback_help() {
	saved=$(cat "$STATE_FILE" 2>/dev/null)
	echo "To start the system as it was before the JackDAW stack:"
	echo "   1. Restart, and hold Shift until the boot menu appears."
	echo "   2. Choose \"Select state\" for your Haiku disk."
	if [ -n "$saved" ] && [ -d "$ADMIN/$saved" ]; then
		echo "   3. Pick the state from $(state_date "$saved")"
		echo "      (saved in $ADMIN/$saved)."
	else
		echo "   3. Pick the newest state from before the installation."
	fi
	echo "   4. Continue booting."
	echo "To leave the stack for good, run: sh $KIT/install.sh --uninstall"
}

# --- what this release contains ------------------------------------------------

SYSTEM_PKG=$(ls "$KIT"/system/haiku-*-"$SYSREV"-$ARCH.hpkg 2>/dev/null | head -1)
DEVEL_PKG=$(ls "$KIT"/system/haiku_devel-*-"$SYSREV"-$ARCH.hpkg 2>/dev/null | head -1)

package_name() {
	# name-version-revision-arch.hpkg; versions start with a digit
	basename "$1" | sed 's/-[0-9].*$//'
}

missing_apps() {
	for p in "$KIT"/packages/*.hpkg; do
		[ -e "$PACKAGES/$(basename "$p")" ] || echo "$p"
	done
}

installed_apps() {
	for p in "$KIT"/packages/*.hpkg; do
		[ -e "$PACKAGES/$(basename "$p")" ] && package_name "$p"
	done
}

system_installed() {
	[ -n "$SYSTEM_PKG" ] && [ -e "$PACKAGES/$(basename "$SYSTEM_PKG")" ]
}

# Files in /boot/system name the package they come from, so the kernel's
# names the system package that is running, not one waiting for a reboot.
system_running() {
	[ "$(catattr -r SYS:PACKAGE_FILE /boot/system/kernel_$ARCH 2>/dev/null | tr -d '\0')" \
		= "$(basename "$SYSTEM_PKG")" ]
}

# --- driver files from release 0.4 -------------------------------------------
# Release 0.4 installed the USB audio driver as non-packaged files and blocked
# the packaged xhci. They would load into the new system and must go.

OLD_BLOCK='Package haiku {
	BlockedEntries {
		add-ons/kernel/boot/xhci
		add-ons/kernel/busses/usb/xhci
	}
}'
OLD_FILES="$HOME/config/non-packaged/add-ons/kernel/busses/usb/xhci
$HOME/config/non-packaged/add-ons/kernel/drivers/bin/usb_audio
$HOME/config/non-packaged/add-ons/kernel/drivers/dev/audio/hmulti/usb_audio
$HOME/config/non-packaged/add-ons/media/hmulti_audio.media_addon
$HOME/config/settings/uac2-driver-installed"
PACKAGES_SETTINGS=/boot/system/settings/packages

old_driver_present() {
	echo "$OLD_FILES" | while read -r f; do
		if [ -e "$f" ] || [ -L "$f" ]; then echo "$f"; fi
	done
	if [ -f "$PACKAGES_SETTINGS" ] && [ "$(cat "$PACKAGES_SETTINGS")" = "$OLD_BLOCK" ]; then
		echo "$PACKAGES_SETTINGS"
	fi
}

remove_old_driver() {
	old_driver_present | while read -r f; do
		rm -f "$f" && echo "   removed $f"
	done
}

# --- checks (change nothing) -----------------------------------------------------

[ "$(uname -s)" = Haiku ] || fail "this script runs on Haiku"

if [ "$MODE" = rollback ]; then
	rollback_help
	exit 0
fi

[ "$(getarch -p 2>/dev/null)" = $ARCH ] || fail "this release is for 64-bit (x86_64) Haiku"
case "$KIT" in
*" "*) fail "this folder's path contains a space" \
	"Move the folder somewhere without spaces, such as your home folder, and run" \
	"the script again." ;;
esac
(cd "$KIT" && sha256sum -c --quiet SHA256SUMS) \
	|| fail "some release files are damaged or missing" "Download the release again."
[ -n "$SYSTEM_PKG" ] && [ -n "$DEVEL_PKG" ] || fail "the system/ folder is incomplete"

# Some unzip tools drop the execute bit.
chmod +x "$KIT/tools/wow64_ldt_test" 2>/dev/null

RUNNING=$(uname -v | awk '{print $1}')
echo "JackDAW stack $VERSION, built for Haiku $HREV. This system runs $RUNNING."

if [ "$MODE" != uninstall ]; then
	# The list holds one package file name per line. If it holds anything else,
	# pkgman can try to reinstall the whole system.
	if grep -q -v -E '^$|\.hpkg$' "$ADMIN/activated-packages" 2>/dev/null; then
		[ -n "$CHECK_ONLY" ] && fail "$ADMIN/activated-packages is damaged" \
			"Run the script without --check to move it aside, then reboot."
		mv "$ADMIN/activated-packages" "$ADMIN/activated-packages.damaged"
		fail "the list of active packages was damaged, and has been moved aside" \
			"Reboot, then run the script again."
	fi
fi

if [ "$MODE" = full ]; then
	# Kernel modules in system non-packaged load before the packaged ones.
	# Media-OS leaves an xhci there.
	[ -e /boot/system/non-packaged/add-ons/kernel/busses/usb/xhci ] \
		&& fail "another USB stack is installed in /boot/system/non-packaged" \
			"(/boot/system/non-packaged/add-ons/kernel/busses/usb/xhci)." \
			"If it is Media-OS, run" \
			"/boot/system/non-packaged/data/mediaos-usb/uninstall.sh, reboot, and run" \
			"this script again."
	if [ -f "$PACKAGES_SETTINGS" ] && grep -q 'busses/usb/xhci' "$PACKAGES_SETTINGS" \
			&& [ "$(cat "$PACKAGES_SETTINGS")" != "$OLD_BLOCK" ]; then
		fail "$PACKAGES_SETTINGS blocks the packaged xhci driver" \
			"Remove the xhci lines from it, reboot, and run the script again."
	fi
fi

# --- uninstall -------------------------------------------------------------------

if [ "$MODE" = uninstall ]; then
	APPS=$(installed_apps | tr '\n' ' ')
	OLD=$(old_driver_present)
	step "This will:"
	[ -n "$APPS" ] && echo "   - uninstall: $APPS"
	[ -n "$OLD" ] && echo "   - remove the USB driver files of release 0.4"
	if system_installed || haiku_repo_is "$PINNED_REPO"; then
		echo "   - set the Haiku repository back to the current nightly, and update"
		echo "     Haiku to it (this replaces the system package)"
	fi
	echo "   Your Wine prefix ($PREFIX) is kept."
	[ -n "$CHECK_ONLY" ] && exit 0
	confirm "Continue?"

	wineserver -k 2>/dev/null
	if [ -n "$APPS" ]; then
		# shellcheck disable=SC2086
		run_pkgman uninstall -y $APPS || fail "pkgman could not uninstall the apps"
	fi
	[ -n "$OLD" ] && remove_old_driver
	if system_installed || haiku_repo_is "$PINNED_REPO"; then
		point_haiku_repo_at "$DEFAULT_REPO"
		run_pkgman update -y || fail "the update to the current nightly failed"
		if system_installed; then
			echo
			echo "The Haiku repository has no newer nightly yet, so the system package"
			echo "is still installed."
			rollback_help
		else
			rm -f "$STATE_FILE"
		fi
		step "Done. Reboot to finish."
	else
		rm -f "$STATE_FILE"
		step "Done."
	fi
	exit 0
fi

# --- decide what to do -----------------------------------------------------------

NEED=$(missing_apps)
if [ "$MODE" = full ]; then
	if system_installed && system_running; then
		ACTION=finish
	elif system_installed; then
		ACTION=reboot
	elif [ "$RUNNING" = "$HREV" ]; then
		ACTION=install
	elif [ "$(hrev_number "$RUNNING")" -lt "$(hrev_number "$HREV")" ]; then
		ACTION=update
	elif [ -n "$DOWNGRADE" ]; then
		ACTION=downgrade
	else
		fail "this system ($RUNNING) is newer than the system package ($HREV)" \
			"The system package contains all of Haiku and fits $HREV only. Choose one:" \
			"" \
			"sh install.sh --apps-only   keep this nightly and install the apps and" \
			"                            Wine (no USB audio driver, no 32-bit Windows" \
			"                            programs, no iLok plug-ins)" \
			"sh install.sh --downgrade   move this system back to $HREV, then install" \
			"                            everything (it explains what that means first)" \
			"" \
			"Or wait for a release built for a newer nightly."
	fi
else
	if [ "$(hrev_number "$RUNNING")" -lt "$(hrev_number "$HREV")" ]; then
		ACTION=update
	else
		ACTION=install
	fi
fi

if [ "$ACTION" = reboot ]; then
	fail "the system package is installed, but the previous system is still running" \
		"Reboot, then run the script again."
fi

step "This will:"
case "$ACTION" in
update)
	if [ "$MODE" = full ]; then
		echo "   - update Haiku from $RUNNING to $HREV, the nightly the system package"
		echo "     is built for, and keep the Haiku repository on it"
	else
		echo "   - update Haiku to at least $HREV (Wine needs it)"
	fi
	echo "   Then reboot and run this script again."
	;;
downgrade)
	echo "   - set the Haiku repository to $HREV and run \"pkgman full-sync\", which"
	echo "     makes every package match the repositories. That moves Haiku back"
	echo "     from $RUNNING to $HREV."
	echo "   Then reboot and run this script again to install the stack."
	echo
	echo "   Before you continue:"
	echo "   - The fixes and features of the nightlies after $HREV are gone until a"
	echo "     release for a newer nightly comes out."
	echo "   - full-sync also removes packages that are in no repository, such as ones"
	echo "     you installed from .hpkg files. pkgman lists every change and asks"
	echo "     before it applies them."
	echo "   - To undo it, boot the state pkgman saves (sh install.sh --rollback shows"
	echo "     how), or run sh install.sh --uninstall, which updates Haiku to the"
	echo "     current nightly again."
	;;
install)
	[ -n "$(old_driver_present)" ] && echo "   - remove the USB driver files of release 0.4"
	if [ "$MODE" = full ]; then
		haiku_repo_is "$PINNED_REPO" \
			|| echo "   - keep the Haiku repository on $HREV, so updates do not replace the system"
		echo "   - install the system package: $(basename "$SYSTEM_PKG")"
	fi
	for p in $NEED; do echo "   - install $(basename "$p")"; done
	if [ "$MODE" = full ]; then
		echo "   Then reboot and run this script again."
	else
		echo "   - set up the Wine prefix $PREFIX"
	fi
	;;
finish)
	for p in $NEED; do echo "   - install $(basename "$p")"; done
	echo "   - run the kernel self-test (about a minute)"
	[ -f "$PREFIX_DONE" ] || echo "   - set up the Wine prefix $PREFIX"
	echo "   - check the installation"
	;;
esac
[ -n "$CHECK_ONLY" ] && exit 0
confirm "Continue?"

# --- the Wine prefix -------------------------------------------------------------

# Wine adds its 32-bit files to a prefix while creating or updating it. An
# existing prefix is updated by removing its update stamp.
setup_prefix() {
	[ -f "$PREFIX_DONE" ] && return 0
	step "Setting up the Wine prefix $PREFIX (a few minutes; windows may flash)"
	wineserver -k 2>/dev/null
	if [ -d "$PREFIX" ]; then
		B="$HOME/wine-registry-backup-$(date +%Y%m%d-%H%M%S)"
		mkdir -p "$B" && cp "$PREFIX"/*.reg "$B"/ \
			&& echo "   registry backed up to $B"
		[ "$MODE" = full ] && rm -f "$PREFIX/.update-timestamp"
	fi
	# Wine looks for its Mono installer in ~/.cache/wine before downloading it.
	mkdir -p "$HOME/.cache/wine"
	cp "$KIT"/tools/wine-mono-*-x86.msi "$HOME/.cache/wine/"
	WINEDEBUG=-all wine cmd /c ver >> "$LOG" 2>&1
	wineserver -w
	if [ ! -d "$PREFIX/drive_c/windows/mono" ]; then
		echo "   installing Wine Mono"
		WINEDEBUG=-all wine msiexec /i "$(ls "$HOME"/.cache/wine/wine-mono-*-x86.msi | tail -1)" \
			/qn >> "$LOG" 2>&1
		wineserver -w
	fi
	[ -d "$PREFIX/drive_c" ] && touch "$PREFIX_DONE"
}

report() {
	step "Checks"
	echo "   Wine: $(wine --version 2>/dev/null || echo 'not found')"
	if [ -d "$PREFIX/drive_c/windows/mono" ]; then
		echo "   Wine Mono: installed"
	else
		echo "   Wine Mono: missing (.NET installers will not work)"
	fi
	if [ "$MODE" = full ]; then
		W32=$(WINEDEBUG=-all wine 'C:\windows\syswow64\cmd.exe' /c echo %PROCESSOR_ARCHITECTURE% \
			2>/dev/null | tr -d '\r')
		wineserver -w
		echo "   32-bit Windows programs: ${W32:-not working} (expected: x86)"
		DEVS=$(ls /dev/audio/hmulti 2>/dev/null | tr '\n' ' ')
		echo "   audio devices: ${DEVS:-none (is the interface plugged in?)}"
	fi
	step "Done. Next:"
	echo "   1. Open Jack Graph (Deskbar > Applications), then Jack Settings. Pick your"
	echo "      interface (USB interfaces appear as usb/1 and so on) and press Start."
	echo "   2. Start JackDAW."
	echo "   3. Windows plug-ins: install them with wine, then open VSTBridge, add"
	echo "      their folder and press Sync."
	echo "   Wine's output is in $LOG."
	echo "   To go back to the system from before the stack: sh install.sh --rollback"
}

install_packages() {
	[ -n "$1" ] || return 0
	wineserver -k 2>/dev/null
	step "Installing packages"
	# shellcheck disable=SC2086
	run_pkgman install -y $1 || fail "the installation failed; nothing was changed"
}

reboot_and_rerun() {
	step "Reboot now, then run this script again: sh $KIT/install.sh$1"
	if [ -n "$LAST_STATE" ]; then
		echo
		echo "If Haiku does not start after the reboot: restart, hold Shift to open the"
		echo "boot menu, choose \"Select state\" and pick the state from"
		echo "$(state_date "$LAST_STATE"). That is the system just before this step."
		echo "sh install.sh --rollback shows how to go back to the system from before"
		echo "the JackDAW stack."
	fi
}

# --- do it -----------------------------------------------------------------------

case "$ACTION" in
update)
	step "Updating Haiku"
	[ "$MODE" = full ] && point_haiku_repo_at "$PINNED_REPO"
	run_pkgman update -y || fail "the update failed"
	if [ "$MODE" = apps ]; then
		reboot_and_rerun " --apps-only"
	else
		reboot_and_rerun ""
	fi
	;;
downgrade)
	step "Moving Haiku back to $HREV"
	point_haiku_repo_at "$PINNED_REPO"
	if [ -n "$YES" ]; then
		run_pkgman full-sync -y
	else
		run_pkgman full-sync
	fi
	if [ $? != 0 ]; then
		point_haiku_repo_at "$DEFAULT_REPO"
		fail "full-sync did not complete, so the Haiku repository was set back to the" \
			"current nightly. Nothing else was changed."
	fi
	reboot_and_rerun ""
	;;
install)
	if [ -n "$(old_driver_present)" ]; then
		step "Removing the USB driver files of release 0.4"
		remove_old_driver
	fi
	if [ "$MODE" = full ]; then
		haiku_repo_is "$PINNED_REPO" || point_haiku_repo_at "$PINNED_REPO"
		SYS="$SYSTEM_PKG"
		# haiku_devel must match haiku exactly; update it only if it is installed.
		ls "$PACKAGES"/haiku_devel-*.hpkg > /dev/null 2>&1 && SYS="$SYS $DEVEL_PKG"
		install_packages "$SYS $NEED"
		reboot_and_rerun ""
	else
		install_packages "$NEED"
		setup_prefix
		report
	fi
	;;
finish)
	install_packages "$NEED"
	step "Kernel self-test (about a minute)"
	if "$KIT/tools/wow64_ldt_test" > "$HOME/jackdaw-stack-kernel-test.log" 2>&1; then
		echo "   passed"
	else
		tail -5 "$HOME/jackdaw-stack-kernel-test.log"
		fail "the kernel self-test failed" \
			"Please report it, with $HOME/jackdaw-stack-kernel-test.log attached."
	fi
	setup_prefix
	report
	;;
esac
