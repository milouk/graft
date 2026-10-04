#!/bin/sh
#
# bootstrap-deps.sh — get what QEMU needs to build, without a package manager.
#
# QEMU's build needs Python 3.9 or newer (macOS ships one with the command
# line tools), ninja, pkg-config, glib and libslirp. Homebrew no longer installs on
# x86_64 Macs, and MacPorts compiles some eighty packages from source to
# provide them. So: ninja, meson and pkg-config come as ready-made Python
# wheels, and glib and libslirp are built from source, in a few minutes.
#
# Everything lands under <build dir>/deps; nothing is installed system-wide.
#
# Usage:  ./tools/bootstrap-deps.sh
#         NVMM_BUILD_DIR=/some/dir ./tools/bootstrap-deps.sh
#
set -eu

GLIB_SERIES=2.88
GLIB_VERSION=2.88.3
SLIRP_VERSION=4.9.5

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${NVMM_BUILD_DIR:-"$ROOT/build"}
DEPS="$BUILD/deps"
PREFIX="$DEPS/prefix"
VENV="$DEPS/venv"

if [ "$(uname -m)" != "x86_64" ]; then
	echo "ERROR: this builds for x86_64 Macs; this shell is $(uname -m)." >&2
	exit 1
fi

PYTHON=${PYTHON:-/usr/bin/python3}
"$PYTHON" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' || {
	echo "ERROR: $PYTHON is older than 3.9. Install the Xcode command line" >&2
	echo "tools (xcode-select --install) or set PYTHON=/path/to/python3." >&2
	exit 1
}

mkdir -p "$DEPS"

# tomli is for QEMU's own configure step: Python 3.9 has no built-in TOML
# parser, and QEMU's build reads a TOML file.
if [ ! -x "$VENV/bin/ninja" ] || [ ! -x "$VENV/bin/meson" ] ||
    ! "$VENV/bin/python" -c 'import tomli, packaging, pkgconf' 2>/dev/null; then
	echo "==> build tools (meson, ninja, pkg-config)"
	"$PYTHON" -m venv "$VENV"
	"$VENV/bin/python" -m pip install -q --upgrade pip
	"$VENV/bin/python" -m pip install -q meson ninja packaging pkgconf tomli
fi

# The wheel ships the real pkgconf binary next to a chatty Python wrapper.
PKGCONF=$("$VENV/bin/python" -c 'import pkgconf, os; print(os.path.join(os.path.dirname(pkgconf.__file__), ".bin", "pkgconf"))')
[ -x "$PKGCONF" ] || { echo "ERROR: pkgconf binary not found in the wheel" >&2; exit 1; }
ln -sf "$PKGCONF" "$VENV/bin/pkg-config-real"

if [ ! -f "$PREFIX/lib/pkgconfig/glib-2.0.pc" ]; then
	TARBALL="$DEPS/glib-$GLIB_VERSION.tar.xz"
	if [ ! -f "$TARBALL" ]; then
		echo "==> downloading glib $GLIB_VERSION"
		curl -fsSL --retry 3 -o "$TARBALL.part" \
		    "https://download.gnome.org/sources/glib/$GLIB_SERIES/glib-$GLIB_VERSION.tar.xz"
		mv "$TARBALL.part" "$TARBALL"
	fi
	rm -rf "$DEPS/glib-$GLIB_VERSION"
	tar -xf "$TARBALL" -C "$DEPS"

	# glib needs libffi. macOS ships it, but without the pkg-config file
	# glib looks for, and glib would otherwise build a bundled copy. Describe
	# the system one instead.
	SDK=$(xcrun --show-sdk-path)
	if [ ! -f "$SDK/usr/include/ffi/ffi.h" ]; then
		echo "ERROR: no libffi headers in $SDK" >&2
		exit 1
	fi
	mkdir -p "$PREFIX/lib/pkgconfig"
	cat > "$PREFIX/lib/pkgconfig/libffi.pc" <<PC
Name: libffi
Description: The libffi shipped with macOS
Version: 3.4
Libs: -lffi
Cflags: -I$SDK/usr/include/ffi
PC

	echo "==> building glib (a few minutes)"
	cd "$DEPS/glib-$GLIB_VERSION"
	PATH="$VENV/bin:$PATH" PKG_CONFIG="$VENV/bin/pkg-config-real" \
	PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
	    "$VENV/bin/meson" setup _build \
	    --prefix="$PREFIX" --libdir=lib --buildtype=release \
	    -Dtests=false -Dintrospection=disabled -Dman-pages=disabled \
	    -Ddocumentation=false -Dnls=disabled -Dselinux=disabled \
	    -Dxattr=false -Dlibmount=disabled -Dsysprof=disabled \
	    -Ddtrace=disabled -Dglib_debug=disabled \
	    > "$DEPS/glib-setup.log" 2>&1 || {
		tail -25 "$DEPS/glib-setup.log" >&2
		echo "ERROR: glib configure failed; see $DEPS/glib-setup.log" >&2
		exit 1
	}
	PATH="$VENV/bin:$PATH" "$VENV/bin/ninja" -C _build install \
	    > "$DEPS/glib-build.log" 2>&1 || {
		grep -E "error:|FAILED" "$DEPS/glib-build.log" | head -20 >&2
		echo "ERROR: glib build failed; see $DEPS/glib-build.log" >&2
		exit 1
	}
fi

# libslirp is QEMU's user-mode network stack: a guest gets outbound network
# access with no privileges and no host configuration. QEMU stopped bundling
# it, so without this a VM has no network unless it is run as root with vmnet.
if [ ! -f "$PREFIX/lib/pkgconfig/slirp.pc" ]; then
	TARBALL="$DEPS/libslirp-$SLIRP_VERSION.tar.gz"
	if [ ! -f "$TARBALL" ]; then
		echo "==> downloading libslirp $SLIRP_VERSION"
		curl -fsSL --retry 3 -o "$TARBALL.part" \
		    "https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$SLIRP_VERSION/libslirp-v$SLIRP_VERSION.tar.gz"
		mv "$TARBALL.part" "$TARBALL"
	fi
	rm -rf "$DEPS/libslirp-v$SLIRP_VERSION"
	tar -xf "$TARBALL" -C "$DEPS"

	echo "==> building libslirp"
	cd "$DEPS/libslirp-v$SLIRP_VERSION"
	PATH="$VENV/bin:$PATH" PKG_CONFIG="$VENV/bin/pkg-config-real" \
	PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
	    "$VENV/bin/meson" setup _build \
	    --prefix="$PREFIX" --libdir=lib --buildtype=release \
	    > "$DEPS/slirp-setup.log" 2>&1 || {
		tail -25 "$DEPS/slirp-setup.log" >&2
		echo "ERROR: libslirp configure failed; see $DEPS/slirp-setup.log" >&2
		exit 1
	}
	PATH="$VENV/bin:$PATH" "$VENV/bin/ninja" -C _build install \
	    > "$DEPS/slirp-build.log" 2>&1 || {
		grep -E "error:|FAILED" "$DEPS/slirp-build.log" | head -20 >&2
		echo "ERROR: libslirp build failed; see $DEPS/slirp-build.log" >&2
		exit 1
	}
fi

echo "==> ready"
echo "    ninja       $("$VENV/bin/ninja" --version)"
echo "    meson       $("$VENV/bin/meson" --version)"
echo "    pkg-config  $("$VENV/bin/pkg-config-real" --version)"
echo "    glib        $(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" "$VENV/bin/pkg-config-real" --modversion glib-2.0)"
echo "    libslirp    $(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" "$VENV/bin/pkg-config-real" --modversion slirp)"
