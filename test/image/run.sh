#!/bin/sh
#
# run.sh — build an image and try it, without an AMD CPU: in a container,
# with QEMU's software emulation in place of nvmm-run.
#
#   test/image/run.sh [flavor ...]          default: every flavor
#
# The Alpine ISO and its kernel are downloaded once into build/image-test.
# ALPINE_RELEASE and ALPINE_VERSION choose them, and must agree with the
# release named in vmm/guest-build.sh.

set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
DL="$ROOT/build/image-test"
REL=${ALPINE_RELEASE:-v3.24}
VER=${ALPINE_VERSION:-3.24.2}
URL="https://dl-cdn.alpinelinux.org/alpine/$REL/releases/x86_64"

mkdir -p "$DL"
[ -f "$DL/alpine-virt.iso" ] ||
    curl -fL --retry 3 -o "$DL/alpine-virt.iso" "$URL/alpine-virt-$VER-x86_64.iso"
[ -f "$DL/vmlinuz-virt" ] ||
    curl -fL --retry 3 -o "$DL/vmlinuz-virt" "$URL/netboot/vmlinuz-virt"

docker build -q -t graft-image-test "$ROOT/test/image" > /dev/null

[ $# -gt 0 ] || set -- $(cd "$ROOT/vmm/flavors" && ls *.sh | sed 's/\.sh$//')
rc=0
for flavor in "$@"; do
	# The tree goes in as a tar stream: a bind mount of it is slow, and
	# the build writes large files that have no business on the host.
	tar -C "$ROOT" -cf - vmm test/image 2>/dev/null |
	    docker run --rm -i -v "$DL:/dl:ro" graft-image-test sh -c \
	    "mkdir /work && tar -C /work -xf - 2>/dev/null && sh /work/test/image/in-container.sh $flavor" || rc=1
done
exit $rc
