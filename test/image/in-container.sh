#!/bin/sh
#
# in-container.sh — build an image of one flavour, boot it under QEMU's
# software emulation, and run a container in it. Run by test/image/run.sh,
# inside the test container, with the repository unpacked in /work and the
# Alpine ISO and kernel in /dl.
#
# This is graft-run's image without graft-run: QEMU stands in for it, so it
# tests the image and not the hypervisor. It needs no AMD CPU.

set -eu
FLAVOR=${1:-containerd}
OUT=/out/$FLAVOR
KEY="$OUT/id_graft"
mkdir -p "$OUT"

echo "== build ($FLAVOR)"
FLAVOR=$FLAVOR SIZE=4G /work/vmm/build-image.exp /usr/bin/qemu-system-x86_64 \
    /dl/alpine-virt.iso "$OUT" > "$OUT/build.log" 2>&1 || {
	grep -a -E "###|@@STEP" "$OUT/build.log" | tail -5
	tail -c 1500 "$OUT/build.log" | tr -d '\r'
	echo "IMAGE TEST FAILED: build"
	exit 1
}
grep -a -o "@@ROOTFS-USED: [0-9]* MB" "$OUT/build.log" | tail -1
ls -l "$OUT/root.squashfs" "$OUT/initramfs-graft" | awk '{printf "%s: %.0f MB\n", $NF, $5 / 1048576}'
. "$OUT/flavor.env"

# Two shared directories. /qshare is served by QEMU and named on the kernel
# command line, which tests the image: it must mount it by itself, at the
# same path, before the runtime starts. /share is served by vmm/p9.c over
# TCP, which tests graft-run's file server against the real client.
mkdir -p /qshare /share
echo "from the host" > /qshare/hello.txt
echo "from the host" > /share/hello.txt
cc -O2 -Wall -Wextra -Werror -I/work/vmm -o /tmp/p9-serve \
    /work/test/image/p9-serve.c /work/vmm/p9.c
/tmp/p9-serve 5640 /share &
SPID=$!

echo "== boot"
qemu-system-x86_64 -accel tcg -M q35 -m 1G -smp 2 -display none -monitor none \
    -serial "file:$OUT/console.log" -kernel /dl/vmlinuz-virt \
    -initrd "$OUT/initramfs-graft" \
    -append "console=ttyS0 $(cat "$OUT/cmdline") graft.share=/qshare" \
    -virtfs local,path=/qshare,mount_tag=share0,security_model=none \
    -drive "file=$OUT/root.squashfs,if=virtio,format=raw,readonly=on" \
    -drive "file=$OUT/data.img,if=virtio,format=raw" \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=n0 &
QPID=$!
trap 'kill $QPID $SPID 2>/dev/null || true' EXIT

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
	check "its socket is there" vm "for i in \$(seq 60); do test -S $FLAVOR_SOCKET && exit 0; sleep 2; done; tail -5 /var/log/$FLAVOR.log; exit 1"
fi
if [ -n "$FLAVOR_CLI" ]; then
	check "$FLAVOR_CLI answers" vm "for i in \$(seq 30); do $FLAVOR_CLI info >/dev/null 2>&1 && exit 0; sleep 2; done; exit 1"
	check "a container runs" vm "$FLAVOR_CLI run --rm alpine echo hello | grep -q hello"
	check "a published port answers" vm "$FLAVOR_CLI run -d --name web -p 8080:80 nginx:alpine >/dev/null && for i in \$(seq 30); do wget -qO- http://127.0.0.1:8080 2>/dev/null | grep -q nginx && exit 0; sleep 2; done; exit 1"
fi

echo "== shared directory"
RUN="$FLAVOR_CLI run --rm"
check "the image mounted what the command line named" vm "grep -q '^share0 /qshare 9p' /proc/mounts && grep -q host /qshare/hello.txt"
[ -z "$FLAVOR_CLI" ] || check "a container sees it at the same path" vm "$RUN -v /qshare:/qshare alpine cat /qshare/hello.txt | grep -q host"
check "graft's file server mounts" vm "modprobe -a 9pnet_fd 9p; mkdir -p /share && mount -t 9p -o trans=tcp,port=5640,version=9p2000.L,msize=262144,cache=mmap 10.0.2.2 /share"
check "the VM reads a host file" vm "grep -q host /share/hello.txt"
vm_writes() { vm 'echo from the vm > /share/vm.txt' && grep -q 'from the vm' /share/vm.txt; }
check "the host reads a VM file" vm_writes
dd if=/dev/urandom of=/share/big bs=1M count=24 2>/dev/null
big_read() { [ "$(vm 'sha256sum < /share/big')" = "$(sha256sum < /share/big)" ]; }
check "a large file reads back intact" big_read
big_write() {
	sum=$(vm 'dd if=/dev/urandom of=/share/big2 bs=1M count=24 2>/dev/null; sync; sha256sum < /share/big2')
	[ -n "$sum" ] && [ "$sum" = "$(sha256sum < /share/big2)" ]
}
check "a large file writes intact" big_write
check "a tree copies in and compares equal" vm "set -ex; mkdir /share/t; cp -r /etc/init.d /etc/ssh /etc/apk /share/t; for d in init.d ssh apk; do diff -r /etc/\$d /share/t/\$d; done; rm -r /share/t; ! test -e /share/t"
check "rename, hard link, symbolic link" vm "set -ex; cd /share; echo a > f1; mv f1 f2; ! test -e f1; ln f2 f3; ln -s f2 f4; [ \"\$(cat f4)\" = a ]; [ \"\$(readlink f4)\" = f2 ]; echo b >> f2; [ \"\$(wc -l < f3)\" = 2 ]; rm f2 f3 f4"
check "mode, truncation, times" vm "set -ex; cd /share; echo a > m; chmod 755 m; [ \"\$(stat -c %a m)\" = 755 ]; chmod 600 m; [ \"\$(stat -c %a m)\" = 600 ]; : > m; [ ! -s m ]; touch -d '2020-01-02 03:04:05' m; [ \"\$(stat -c %Y m)\" = \"\$(date -d '2020-01-02 03:04:05' +%s)\" ]; mkdir -m 750 md; [ \"\$(stat -c %a md)\" = 777 ]; rmdir md; rm m"
check "a program runs from it" vm "set -ex; cp /bin/busybox /share/true; /share/true; rm /share/true"
if [ -n "$FLAVOR_CLI" ]; then
	bind_mount() {
		vm "$RUN -v /share:/data alpine sh -c 'cat /data/hello.txt && echo from a container > /data/c.txt'" | grep -q host &&
		    grep -q container /share/c.txt
	}
	check "a container reads and writes a bind mount" bind_mount
	check "a container's working directory on the share" vm "$RUN -v /share:/share -w /share alpine sh -c 'mkdir -p d/e && touch d/e/x && ls d/e | grep -q x && rm -r d'"
	check "a container that is not root writes to it" vm "$RUN --user 1000:1000 -v /share:/data alpine sh -c 'set -ex; echo u > /data/u.txt; echo more >> /data/hello.txt; mkdir /data/ud; rmdir /data/ud; [ \"\$(stat -c %u /data/u.txt)\" = 1000 ]; [ \"\$(stat -c %u /data)\" = 1000 ]'"
fi
echo "-- in the VM:"
vm "free -m | sed -n 2p; df -m / /var/lib | tail -2; dmesg | grep -i -c -E 'error|fail' || true"
# What is written under /var/lib must still be there after a restart; the
# rest of the root must not be.
check "the data disk is what /var/lib is" vm "mount | grep -q '/dev/vdb on /var/lib'"
vm poweroff 2>/dev/null || true
i=0
while kill -0 $QPID 2>/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done

[ $fail -eq 0 ] && echo "IMAGE TEST PASSED ($FLAVOR)" || { echo "IMAGE TEST FAILED ($FLAVOR)"; exit 1; }
