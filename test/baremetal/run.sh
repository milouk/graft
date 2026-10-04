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

# Link first, as its own step: a link failure must never be mistaken for a
# test result.
docker run --rm -v "$ROOT:/work" -w /work "$IMAGE" \
	ld.lld -m elf_x86_64 -nostdlib -static -z max-page-size=4096 \
	    -T test/baremetal/linker.ld -o build/bare/nvmm-test.elf \
	    $(cd "$ROOT" && ls build/bare/*.o)

LOG="$ROOT/build/bare/nvmm-test.log"
set +e
docker run --rm -v "$ROOT:/work" -w /work "$IMAGE" \
	timeout 180 qemu-system-x86_64 \
	    -machine q35 -accel tcg -cpu "$CPU" -m 512 \
	    -display none -serial stdio -monitor none -no-reboot \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -kernel build/bare/nvmm-test.elf "$@" 2>&1 | tee "$LOG" |
	grep -v "TCG doesn't support requested feature"
set -e

# The kernel prints this line only after every check has passed, and then asks
# the emulator to exit. Trust the line, not an exit status that other failures
# could also produce.
if grep -q "^ALL TESTS PASSED" "$LOG"; then
	echo "test kernel: PASS"
	exit 0
fi
echo "test kernel: FAIL (see $LOG)" >&2
exit 1
