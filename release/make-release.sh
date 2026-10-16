#!/bin/sh
# Assemble the JackDAW stack release archive from built packages.
#
#   sh make-release.sh <input dir> <output dir>
#
# <input dir> holds the built files:
#   *.hpkg                  the stack packages (one of each, see PACKAGES below)
#   system/*.hpkg           haiku and haiku_devel, from build-system-package.sh
#   tools/                  wow64_ldt_test, install-driver.sh,
#                           wine-mono-<version>-x86.msi
# Only the files named here are copied, so nothing else in the input directory
# (installers, licence files, logs) can end up in the archive.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
IN=${1:?input dir}
OUT=${2:?output dir}

VERSION=0.5
HREV=hrev60256
NAME="jackdaw-stack-$VERSION"

PACKAGES="jack jack_tools jackgraph lv2_haiku vst3_haiku hktuner namku drumku wine
	vstbridge jackdaw"
TOOLS="wow64_ldt_test install-driver.sh"

fail() {
	echo "!! $*" >&2
	exit 1
}

# one <dir> <pattern>: the only file in <dir> matching <pattern>
one() {
	dir=$1
	set -- "$dir"/$2
	[ $# -eq 1 ] && [ -f "$1" ] || fail "expected one match for $dir/$2, got: $*"
	echo "$1"
}

D="$OUT/$NAME"
rm -rf "$D"
mkdir -p "$D/packages" "$D/system" "$D/tools"

for p in $PACKAGES; do
	# the version starts with a digit, which keeps jack from matching jack_tools
	cp "$(one "$IN" "$p-[0-9]*-x86_64.hpkg")" "$D/packages/"
done
cp "$(one "$IN/system" "haiku-r1~beta6_$HREV-2-x86_64.hpkg")" "$D/system/"
cp "$(one "$IN/system" "haiku_devel-r1~beta6_$HREV-2-x86_64.hpkg")" "$D/system/"
for t in $TOOLS; do
	cp "$(one "$IN/tools" "$t")" "$D/tools/"
done
cp "$(one "$IN/tools" "wine-mono-*-x86.msi")" "$D/tools/"
chmod 755 "$D/tools/wow64_ldt_test"

cp "$HERE/install.sh" "$HERE/README.txt" "$D/"
cp "$HERE/RELEASE_NOTES-$VERSION.md" "$D/RELEASE_NOTES.md"
printf 'HREV=%s\nVERSION=%s\n' "$HREV" "$VERSION" > "$D/release.conf"

# The only Windows file in a release is the Wine Mono installer.
EXTRA=$(cd "$D" && find . -type f \( -iname '*.exe' -o -iname '*.msi' -o -iname '*.dll' \) \
	! -path './tools/wine-mono-*-x86.msi')
[ -z "$EXTRA" ] || fail "unexpected Windows files: $EXTRA"

(cd "$D" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort \
	| xargs -d '\n' sha256sum > SHA256SUMS)

python3 -I - "$OUT" "$NAME" "$NAME-$HREV.zip" <<'EOF'
import os, sys, zipfile
out, name = sys.argv[1], sys.argv[2]
archive = os.path.join(out, sys.argv[3])
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(os.path.join(out, name)):
        dirs.sort()
        for f in sorted(files):
            path = os.path.join(root, f)
            info = zipfile.ZipInfo.from_file(path, os.path.relpath(path, out))
            # keep the execute bits; hpkg and msi files are already compressed
            info.compress_type = zipfile.ZIP_STORED if f.endswith((".hpkg", ".msi")) \
                else zipfile.ZIP_DEFLATED
            with open(path, "rb") as src:
                z.writestr(info, src.read())
print(archive)
EOF
(cd "$D" && sha256sum -c --quiet SHA256SUMS) || fail "checksums do not verify"
ZIP="$OUT/$NAME-$HREV.zip"
echo "== $(du -h "$ZIP" | cut -f1) $ZIP"
unzip -l "$ZIP" | sed -n '4,$p'
