#!/bin/sh
#
# run.sh — link the bare-metal test kernel and boot it in an emulated AMD
# machine.
#
# QEMU's software emulator (TCG) implements AMD-V, including nested paging, so
# the SVM engine can be exercised on any host, including an Apple Silicon Mac.
# The linker and QEMU live in a Docker image so nothing is installed here.
#
# Usage:  ./test/baremetal/run.sh [extra qemu args]
#
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
IMAGE=nvmm-darwin-test
CPU=${NVMM_TEST_CPU:-EPYC}

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "Building the $IMAGE image (one-time)..."
	docker build -q -t "$IMAGE" "$ROOT/test/baremetal/docker" >/dev/null
fi

set +e
docker run --rm -v "$ROOT:/work" -w /work "$IMAGE" sh -c '
	set -e
	ld.lld -m elf_x86_64 -nostdlib -static -z max-page-size=4096 \
	    -T test/baremetal/linker.ld -o build/bare/nvmm-test.elf \
	    build/bare/*.o
	exec timeout 180 qemu-system-x86_64 \
	    -machine q35 -accel tcg -cpu "$0" -m 512 \
	    -display none -serial stdio -monitor none -no-reboot \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -kernel build/bare/nvmm-test.elf "$@"
' "$CPU" "$@"
status=$?
set -e

# isa-debug-exit turns a guest-written value v into exit status (v << 1) | 1.
case "$status" in
	1) echo "test kernel: PASS"; exit 0 ;;
	3) echo "test kernel: FAIL (a check failed; see the log above)" >&2; exit 1 ;;
	124) echo "test kernel: FAIL (timed out)" >&2; exit 1 ;;
	*) echo "test kernel: FAIL (emulator exited with status $status)" >&2; exit 1 ;;
esac
