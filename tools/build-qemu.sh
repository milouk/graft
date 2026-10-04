#!/bin/sh
#
# build-qemu.sh — build QEMU for macOS x86_64 with the NVMM accelerator.
#
# QEMU has had an `nvmm` accelerator since 6.0, but its build only looks for
# it on NetBSD. This downloads a QEMU release, widens that one check to macOS,
# and builds it against this repository's libnvmm.
#
# Run it on the Intel or AMD Mac that will use it. QEMU's own dependencies
# come from MacPorts; Homebrew no longer installs on x86_64 Macs.
#
#   sudo port -N install pkgconfig ninja glib2 libpixman python313
#   sudo port select --set python3 python313
#
# Usage:  ./tools/build-qemu.sh [qemu-version]      default: 11.1.2
#
# Everything is built under ./build/qemu; nothing is installed system-wide.
# The result is build/qemu/qemu-<version>/build/qemu-system-x86_64.
#
set -eu

VERSION=${1:-11.1.2}
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WORK="$ROOT/build/qemu"
SRC="$WORK/qemu-$VERSION"
TARBALL="$WORK/qemu-$VERSION.tar.xz"

# MacPorts lives in /opt/local and is not always on a non-login shell's PATH.
PATH="/opt/local/bin:/opt/local/sbin:$PATH"
export PATH
PKG_CONFIG_PATH="/opt/local/lib/pkgconfig:/opt/local/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

if [ "$(uname -m)" != "x86_64" ]; then
	echo "ERROR: this builds for x86_64 Macs; this machine is $(uname -m)." >&2
	exit 1
fi

for tool in pkg-config ninja python3; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "ERROR: $tool not found. Install MacPorts, then run:" >&2
		echo "  sudo port -N install pkgconfig ninja glib2 libpixman python313" >&2
		echo "  sudo port select --set python3 python313" >&2
		exit 1
	}
done
pkg-config --exists glib-2.0 || {
	echo "ERROR: glib not found. Run: sudo port -N install glib2 libpixman" >&2
	exit 1
}

echo "==> libnvmm"
make -C "$ROOT" libnvmm >/dev/null
[ -f "$ROOT/build/libnvmm.a" ] || { echo "ERROR: libnvmm did not build" >&2; exit 1; }

mkdir -p "$WORK"
if [ ! -f "$TARBALL" ]; then
	echo "==> downloading QEMU $VERSION"
	curl -fL --retry 3 -o "$TARBALL.part" \
	    "https://download.qemu.org/qemu-$VERSION.tar.xz"
	mv "$TARBALL.part" "$TARBALL"
fi

if [ ! -d "$SRC" ]; then
	echo "==> unpacking"
	tar -xf "$TARBALL" -C "$WORK"
fi

echo "==> allowing the nvmm accelerator on macOS"
python3 - "$SRC/meson.build" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = "if host_os == 'netbsd'\n  nvmm = cc.find_library('nvmm'"
new = "if host_os == 'netbsd' or host_os == 'darwin'\n  nvmm = cc.find_library('nvmm'"
if new in src:
    print("    already patched")
elif src.count(old) == 1:
    open(path, "w").write(src.replace(old, new))
    print("    patched meson.build")
else:
    sys.exit("ERROR: the nvmm check in meson.build does not look as expected "
             "for this QEMU version; patch it by hand.")
PY

echo "==> configuring"
mkdir -p "$SRC/build"
cd "$SRC/build"
../configure \
    --python="$(command -v python3)" \
    --target-list=x86_64-softmmu \
    --enable-nvmm \
    --disable-docs \
    --disable-guest-agent \
    --disable-werror \
    --extra-cflags="-I$ROOT/build/include" \
    --extra-ldflags="-L$ROOT/build" \
    > configure.log 2>&1 || {
	tail -30 configure.log >&2
	echo "ERROR: configure failed; full log in $SRC/build/configure.log" >&2
	exit 1
}
grep -E "NVMM support|HVF support|TCG support" configure.log | sed 's/^/    /'
grep -q "NVMM support *: YES" configure.log || {
	echo "ERROR: QEMU configured without NVMM; see $SRC/build/configure.log" >&2
	exit 1
}

echo "==> building (this takes a while)"
ninja qemu-system-x86_64 qemu-img > build.log 2>&1 || {
	grep -E "error:|FAILED" build.log | head -30 >&2
	echo "ERROR: build failed; full log in $SRC/build/build.log" >&2
	exit 1
}

echo
echo "Built: $SRC/build/qemu-system-x86_64"
"$SRC/build/qemu-system-x86_64" --version | head -1
"$SRC/build/qemu-system-x86_64" -accel help
