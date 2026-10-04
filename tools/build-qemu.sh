#!/bin/sh
#
# build-qemu.sh — build QEMU for macOS x86_64 with the NVMM accelerator.
#
# QEMU has had an `nvmm` accelerator since 6.0, but its build only looks for
# it on NetBSD. This downloads a QEMU release, widens that one check to macOS,
# and builds it against this repository's libnvmm.
#
# Run it on the Intel or AMD Mac that will use it. It needs only the Xcode
# command line tools: QEMU's other dependencies are fetched and built by
# tools/bootstrap-deps.sh, which this script runs if they are missing.
#
# Usage:  ./tools/build-qemu.sh [qemu-version]      default: 11.1.2
#         NVMM_BUILD_DIR=/some/dir ./tools/build-qemu.sh
#
# Everything is built under the build directory; nothing is installed
# system-wide. The result is <build>/qemu/qemu-<version>/build/qemu-system-x86_64.
#
set -eu

VERSION=${1:-11.1.2}
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${NVMM_BUILD_DIR:-"$ROOT/build"}
DEPS="$BUILD/deps"
WORK="$BUILD/qemu"
SRC="$WORK/qemu-$VERSION"
TARBALL="$WORK/qemu-$VERSION.tar.xz"

if [ "$(uname -m)" != "x86_64" ]; then
	echo "ERROR: this builds for x86_64 Macs; this shell is $(uname -m)." >&2
	exit 1
fi

# Cheap when everything is already there, and it picks up anything a newer
# version of the bootstrap adds.
"$ROOT/tools/bootstrap-deps.sh" 

# A deliberately bare PATH. QEMU's configure probes for dozens of optional
# libraries through helper programs on the PATH, and whatever a package
# manager left on this machine (possibly for another CPU architecture) must
# not leak into the build.
PATH="$DEPS/venv/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PATH
PKG_CONFIG="$DEPS/venv/bin/pkg-config-real"
export PKG_CONFIG
PKG_CONFIG_PATH="$DEPS/prefix/lib/pkgconfig"
export PKG_CONFIG_PATH

echo "==> libnvmm"
make -C "$ROOT" libnvmm >/dev/null
[ -f "$ROOT/build/libnvmm.a" ] || { echo "ERROR: libnvmm did not build" >&2; exit 1; }

mkdir -p "$WORK"
if [ ! -f "$TARBALL" ]; then
	echo "==> downloading QEMU $VERSION"
	curl -fsSL --retry 3 -o "$TARBALL.part" \
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
# pixman is only needed for graphical output, which a container host has no
# use for. The crypto libraries are optional too: QEMU falls back to its own
# implementations.
../configure \
    --python="$DEPS/venv/bin/python" \
    --ninja="$DEPS/venv/bin/ninja" \
    --target-list=x86_64-softmmu \
    --enable-nvmm \
    --enable-slirp \
    --disable-pixman \
    --disable-gcrypt \
    --disable-gnutls \
    --disable-nettle \
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
grep -E "NVMM support|TCG support|slirp support|vmnet" configure.log | sed 's/^ */    /'
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
