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
# Usage:  ./tools/build-qemu.sh [qemu-version]
#
# The default version is 11.1.2 on macOS 12 and later. Older systems get
# 7.2.22, the last series that builds with their compiler and Python; it has
# booted Linux under this driver on macOS 10.15.
#         NVMM_BUILD_DIR=/some/dir ./tools/build-qemu.sh
#
# Everything is built under the build directory; nothing is installed
# system-wide. The result is <build>/qemu/qemu-<version>/build/qemu-system-x86_64.
#
set -eu

OS_MAJOR=$(sw_vers -productVersion | cut -d. -f1)
if [ "$OS_MAJOR" -ge 12 ]; then
	DEFAULT_VERSION=11.1.2
else
	DEFAULT_VERSION=7.2.22
fi
VERSION=${1:-$DEFAULT_VERSION}
QEMU_MAJOR=${VERSION%%.*}
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
make -C "$ROOT" BUILD="$BUILD" libnvmm >/dev/null
[ -f "$BUILD/libnvmm.a" ] || { echo "ERROR: libnvmm did not build" >&2; exit 1; }

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
# The variable holding the host OS was renamed from targetos to host_os in 9.0.
for var in ("host_os", "targetos"):
    old = "if %s == 'netbsd'\n  nvmm = cc.find_library('nvmm'" % var
    new = ("if %s == 'netbsd' or %s == 'darwin'\n"
           "  nvmm = cc.find_library('nvmm'" % (var, var))
    if new in src:
        print("    already patched")
        break
    if src.count(old) == 1:
        open(path, "w").write(src.replace(old, new))
        print("    patched meson.build")
        break
else:
    sys.exit("ERROR: the nvmm check in meson.build does not look as expected "
             "for this QEMU version; patch it by hand.")
PY

echo "==> configuring"
mkdir -p "$SRC/build"
cd "$SRC/build"
# No graphical output: a container host has no use for it. Current QEMU can
# then do without pixman altogether; QEMU 7 cannot, and is told to leave the
# Cocoa window and the disassembler out instead. The crypto libraries are
# optional: QEMU falls back to its own implementations.
if [ "$QEMU_MAJOR" -ge 9 ]; then
	DISPLAY_FLAGS="--disable-pixman"
else
	DISPLAY_FLAGS="--disable-cocoa --disable-capstone"
fi
# shellcheck disable=SC2086
../configure \
    --python="$DEPS/venv/bin/python" \
    --ninja="$DEPS/venv/bin/ninja" \
    --target-list=x86_64-softmmu \
    --enable-nvmm \
    --enable-slirp \
    $DISPLAY_FLAGS \
    --disable-gcrypt \
    --disable-gnutls \
    --disable-nettle \
    --disable-docs \
    --disable-guest-agent \
    --disable-werror \
    --extra-cflags="-I$BUILD/include" \
    --extra-ldflags="-L$BUILD" \
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
