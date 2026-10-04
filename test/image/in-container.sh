#!/bin/sh
#
# in-container.sh — build an image of one flavour, boot it under QEMU's
# software emulation, and run a container in it. Run by test/image/run.sh,
# inside the test container, with the repository unpacked in /work and the
# Alpine ISO and kernel in /dl.
#
# This is nvmm-run's image without nvmm-run: QEMU stands in for it, so it
# tests the image and not the hypervisor. It needs no AMD CPU.

set -eu
FLAVOR=${1:-containerd}
OUT=/out/$FLAVOR
KEY="$OUT/id_nvmm"
mkdir -p "$OUT"

echo "== build ($FLAVOR)"
FLAVOR=$FLAVOR SIZE=2G /work/vmm/build-image.exp /usr/bin/qemu-system-x86_64 \
    /dl/alpine-virt.iso "$OUT" > "$OUT/build.log" 2>&1 || {
	grep -a -E "###|@@STEP|rror" "$OUT/build.log" | tail -15
	echo "IMAGE TEST FAILED: build"
	exit 1
}
grep -a -o "@@ROOTFS-USED: [0-9]* MB" "$OUT/build.log" | tail -1
. "$OUT/flavor.env"

echo "== boot"
qemu-system-x86_64 -accel tcg -M q35 -m 1G -smp 2 -display none -monitor none \
    -serial "file:$OUT/console.log" -kernel /dl/vmlinuz-virt \
    -initrd "$OUT/initramfs-nvmm" \
    -append "console=ttyS0 $(cat "$OUT/cmdline" 2>/dev/null || echo "root=/dev/vda rootfstype=ext4 modules=ext4")" \
    -drive "file=$OUT/rootfs.img,if=virtio,format=raw" \
    $( [ -f "$OUT/data.img" ] && echo "-drive file=$OUT/data.img,if=virtio,format=raw" ) \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=n0 &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

vm() {
	ssh -q -i "$KEY" -p 2222 -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 root@127.0.0.1 "$@"
}
i=0
until vm true 2>/dev/null; do
	i=$((i + 1))
	if [ $i -gt 90 ] || ! kill -0 $QPID 2>/dev/null; then
		tail -20 "$OUT/console.log"
		echo "IMAGE TEST FAILED: the VM did not come up"
		exit 1
	fi
	sleep 2
done
echo "up after about $((i * 2))s"

fail=0
check() {	# check <what> <command...>
	what=$1
	shift
	if "$@" > "$OUT/check.log" 2>&1; then
		echo "ok    $what"
	else
		echo "FAIL  $what"
		tail -5 "$OUT/check.log"
		fail=1
	fi
}

check "service $FLAVOR is started" vm "rc-service $FLAVOR status"
if [ -n "$FLAVOR_SOCKET" ]; then
	check "its socket is there" vm "test -S $FLAVOR_SOCKET"
fi
if [ -n "$FLAVOR_CLI" ]; then
	check "$FLAVOR_CLI answers" vm "for i in \$(seq 30); do $FLAVOR_CLI info >/dev/null 2>&1 && exit 0; sleep 2; done; exit 1"
	check "a container runs" vm "$FLAVOR_CLI run --rm alpine echo hello | grep -q hello"
	check "a published port answers" vm "$FLAVOR_CLI run -d --name web -p 8080:80 nginx:alpine >/dev/null && for i in \$(seq 30); do wget -qO- http://127.0.0.1:8080 2>/dev/null | grep -q nginx && exit 0; sleep 2; done; exit 1"
fi
echo "-- in the VM:"
vm "free -m | sed -n 2p; df -m / | tail -1; dmesg | grep -i -c -E 'error|fail' || true"
vm poweroff 2>/dev/null || true
i=0
while kill -0 $QPID 2>/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done

[ $fail -eq 0 ] && echo "IMAGE TEST PASSED ($FLAVOR)" || { echo "IMAGE TEST FAILED ($FLAVOR)"; exit 1; }
