#!/bin/sh
#
# final-check.sh — everything that can be checked on a Mac without AMD-V,
# in one run. Needs the self-test kext loaded and sudo.
#
#   ./final-check.sh <dir with nvmm-selftest and nvmm-fake-test> [qemu binary]
#
# Prints FINAL CHECK PASSED on the last line, or says what failed.

set -u

DIR=${1:?usage: final-check.sh <test dir> [qemu-system-x86_64]}
QEMU=${2:-}
KEXT_ID=org.nvmm.driver.NVMMSelfTest
KEXT_PATH=/private/var/tmp/NVMMSelfTest.kext
ROUNDS=5
fails=0

say()  { printf '\n== %s\n' "$*"; }
bad()  { printf 'FAILED: %s\n' "$*"; fails=$((fails + 1)); }

# IOKit objects the driver creates. A count that keeps growing is a leak.
counts() {
	ioclasscount IOBufferMemoryDescriptor IOMultiMemoryDescriptor \
	    IOMemoryMap 2>/dev/null | tr '\n' ' '
}
count_of() {
	ioclasscount "$1" 2>/dev/null | sed -n 's/.*= *\([0-9][0-9]*\).*/\1/p'
}

say "system"
sw_vers -productVersion
sysctl -n machdep.cpu.brand_string
uptime

say "kext"
if ! kmutil showloaded 2>/dev/null | grep -q "$KEXT_ID"; then
	echo "FINAL CHECK FAILED: $KEXT_ID is not loaded"
	exit 1
fi
kmutil showloaded 2>/dev/null | grep "$KEXT_ID"
if [ -f "$KEXT_PATH/Contents/MacOS/NVMMSelfTest" ]; then
	echo "on disk: $(dwarfdump -u "$KEXT_PATH/Contents/MacOS/NVMMSelfTest" \
	    2>/dev/null | head -1)"
fi
ls -l /dev/nvmm /dev/nvmm-selftest || bad "device nodes missing"

say "glue self-test, $ROUNDS rounds"
# One warm-up round first: the first run after load allocates things that
# stay for the life of the kext, and those are not leaks.
sudo "$DIR/nvmm-selftest" > /tmp/nvmm-final-st.log 2>&1 ||
    bad "nvmm-selftest warm-up (see /tmp/nvmm-final-st.log)"
grep -E "in-kernel checks|2M runs|contiguous" /tmp/nvmm-final-st.log
tail -1 /tmp/nvmm-final-st.log
before=$(counts)
buf0=$(count_of IOBufferMemoryDescriptor)
i=1
while [ $i -le $ROUNDS ]; do
	if sudo "$DIR/nvmm-selftest" > /tmp/nvmm-final-st.log 2>&1; then
		printf 'round %d: %s\n' $i "$(tail -1 /tmp/nvmm-final-st.log)"
	else
		bad "nvmm-selftest round $i"
		grep -E "FAIL|MISMATCH|mismatch" /tmp/nvmm-final-st.log | head -10
	fi
	i=$((i + 1))
done

say "a process that exits with 64 MiB still mapped"
sudo "$DIR/nvmm-selftest" --leak | tail -1 || bad "leak run"
sudo "$DIR/nvmm-selftest" > /tmp/nvmm-final-st.log 2>&1 ||
    bad "nvmm-selftest after the leak run"
tail -1 /tmp/nvmm-final-st.log

say "libnvmm against the stand-in engine, $ROUNDS rounds"
i=1
while [ $i -le $ROUNDS ]; do
	if sudo "$DIR/nvmm-fake-test" > /tmp/nvmm-final-ft.log 2>&1; then
		printf 'round %d: %s\n' $i \
		    "$(tail -2 /tmp/nvmm-final-ft.log | tr '\n' ' ')"
	else
		bad "nvmm-fake-test round $i"
		grep -E "FAIL" /tmp/nvmm-final-ft.log | head -10
	fi
	i=$((i + 1))
done

if [ -x "$DIR/nvmm-guest-test" ]; then
	say "real-guest test, the stages that run no guest"
	sudo "$DIR/nvmm-guest-test" 2 || bad "nvmm-guest-test stages 1-2"
fi

if [ -n "$QEMU" ] && [ -x "$QEMU" ]; then
	for machine in q35 microvm; do
		say "QEMU -accel nvmm -M $machine -m 1G"
		printf 'info status\ninfo registers\nquit\n' |
		    sudo "$QEMU" -accel nvmm -M "$machine" -m 1G -display none \
		    -monitor stdio -serial none -net none \
		    > /tmp/nvmm-final-qemu.log 2>&1
		rc=$?
		grep -E "operational|VM status|^RIP|error|failed" \
		    /tmp/nvmm-final-qemu.log | head -6
		[ $rc -eq 0 ] || bad "QEMU $machine exited with $rc"
		grep -q "operational" /tmp/nvmm-final-qemu.log ||
		    bad "QEMU $machine: accelerator did not start"
		grep -q "VM status: running" /tmp/nvmm-final-qemu.log ||
		    bad "QEMU $machine: machine not running"
	done
	sudo "$DIR/nvmm-selftest" > /tmp/nvmm-final-st.log 2>&1 ||
	    bad "nvmm-selftest after QEMU"
	printf 'driver after QEMU: %s\n' "$(tail -1 /tmp/nvmm-final-st.log)"
else
	say "QEMU: skipped (no binary given)"
fi

say "IOKit object counts"
after=$(counts)
buf1=$(count_of IOBufferMemoryDescriptor)
echo "before: $before"
echo "after:  $after"
# Other drivers come and go too, so allow a little noise; a leak of one
# buffer per round would show as far more than this.
if [ -n "$buf0" ] && [ -n "$buf1" ] && [ $((buf1 - buf0)) -gt 8 ]; then
	bad "IOBufferMemoryDescriptor grew by $((buf1 - buf0))"
fi

say "unload and load again"
if sudo kmutil unload -b "$KEXT_ID"; then
	[ -e /dev/nvmm ] && bad "/dev/nvmm still there after unload"
	if sudo kmutil load -p "$KEXT_PATH"; then
		if sudo "$DIR/nvmm-selftest" > /tmp/nvmm-final-st.log 2>&1; then
			printf 'after reload: %s\n' \
			    "$(tail -1 /tmp/nvmm-final-st.log)"
		else
			bad "nvmm-selftest after reload"
		fi
	else
		bad "kext did not load again"
	fi
else
	bad "kext did not unload (something still has the device open?)"
fi

say "kernel log"
sudo dmesg 2>/dev/null | grep -i "nvmm" | tail -12

rm -f /tmp/nvmm-final-st.log /tmp/nvmm-final-ft.log /tmp/nvmm-final-qemu.log

echo
if [ $fails -eq 0 ]; then
	echo "FINAL CHECK PASSED"
else
	echo "FINAL CHECK FAILED: $fails problem(s)"
	exit 1
fi
