# guest-build.sh — runs inside the Alpine ISO, as root, to build what
# graft-run boots:
#
#   - a root filesystem, compressed and read-only (squashfs);
#   - a data disk (ext4) that becomes the VM's /var/lib, where every runtime
#     keeps its images and containers;
#   - an initramfs that can mount both.
#
# Changes to the root while the VM runs are kept in memory and gone at the
# next boot; only /var/lib lasts. That keeps the image small, makes replacing
# it safe, and leaves the containers alone when it is replaced.
#
# build-image.exp hands this file to the guest as the disk /dev/vdb and runs
# it with "sh /dev/vdb", after putting in front of it the host's public key
# (PUBKEY), the container images to pull if any (IMAGES), and one of the
# files in flavors/, which says what the VM runs containers with. The tree is
# assembled on /dev/vda, which is then reformatted as the data disk; the
# initramfs and the squashfs leave through /dev/vdc, raw, at offsets 0 and
# SQUASHFS_AT megabytes.

set -e
trap 'echo "@@RESULT:1@@"' EXIT
[ -n "${PUBKEY:-}" ] || { echo "no PUBKEY given"; exit 1; }
[ -n "${FLAVOR_SERVICE:-}" ] || { echo "no flavor given"; exit 1; }
step() { echo "@@STEP: $*"; }

REPO=https://dl-cdn.alpinelinux.org/alpine/v3.24
SQUASHFS_AT=64
KVER=$(uname -r)

step "network and package repositories"
printf 'auto lo\niface lo inet loopback\n\nauto eth0\niface eth0 inet dhcp\n' \
    > /etc/network/interfaces
rc-service networking start
printf '%s/main\n%s/community\n' "$REPO" "$REPO" > /etc/apk/repositories
apk --no-progress update
apk --no-progress add e2fsprogs mkinitfs squashfs-tools openssh-keygen
[ -z "${IMAGES:-}" ] || apk --no-progress add $FLAVOR_BUILD_PKGS

step "initramfs with the disk and filesystem drivers, for kernel $KVER"
mkinitfs -F "base ext4 virtio network squashfs" -o /tmp/initramfs-graft "$KVER"
dd if=/tmp/initramfs-graft of=/dev/vdc bs=1M 2>/dev/null
echo "@@INITRAMFS-SIZE:$(stat -c %s /tmp/initramfs-graft)@@"

step "the root tree"
mkfs.ext4 -q -F /dev/vda
modprobe ext4
mount -t ext4 /dev/vda /mnt
mkdir -p /mnt/etc/apk
cp -a /etc/apk/keys /mnt/etc/apk/
cp /etc/apk/repositories /mnt/etc/apk/
apk --no-progress --root /mnt --initdb \
    --repositories-file /etc/apk/repositories add alpine-base e2fsprogs \
    openssh-server $FLAVOR_PKGS

step "kernel modules: the ISO's own, less what a VM cannot use"
mkdir -p /mnt/lib/modules
cp -a "/.modloop/modules/$KVER" /mnt/lib/modules/
# Drivers for hardware this machine does not have, filesystems nobody will
# put under a container host (NFS among them), and KVM, which has nothing to
# nest on. Everything a container needs stays.
K="/mnt/lib/modules/$KVER/kernel"
rm -rf "$K/sound" "$K/drivers/gpu" "$K/drivers/usb" "$K/drivers/scsi" \
    "$K/drivers/target" "$K/drivers/nvme" "$K/drivers/xen" \
    "$K/drivers/net/ethernet" "$K/drivers/net/wireless" "$K/drivers/hid" \
    "$K/drivers/md" "$K/drivers/infiniband" "$K/drivers/media" \
    "$K/fs/xfs" "$K/fs/ocfs2" "$K/fs/btrfs" "$K/fs/f2fs" "$K/fs/smb" \
    "$K/fs/ceph" "$K/fs/gfs2" "$K/fs/jfs" "$K/fs/nilfs2" "$K/fs/ntfs3" \
    "$K/fs/udf" "$K/fs/reiserfs" "$K/fs/bcachefs" "$K/net/ceph" \
    "$K/net/sctp" "$K/net/bluetooth" "$K/net/wireless" "$K/net/mac80211" \
    "$K/fs/nfs" "$K/fs/nfsd" "$K/fs/lockd" "$K/fs/nfs_common" \
    "$K/net/sunrpc" "$K/arch" "$K/drivers/crypto" "$K/drivers/block/drbd" \
    "$K/drivers/block/rnbd"
depmod -b /mnt "$KVER"

step "system configuration"
# A login on the serial console, root with no password.
echo 'ttyS0::respawn:/sbin/getty -L 115200 ttyS0 vt100' >> /mnt/etc/inittab
chroot /mnt passwd -d root
echo '/dev/vdb /var/lib ext4 rw,relatime 0 2' > /mnt/etc/fstab
echo graft > /mnt/etc/hostname
# Host keys made now: the root does not keep what is written to it, and
# making them at every boot would cost time and change the VM's identity.
ssh-keygen -A -f /mnt
# eth0 is brought up if it is there; a VM without a network still boots.
printf 'auto lo\niface lo inet loopback\n\nallow-hotplug eth0\nauto eth0\niface eth0 inet dhcp\n' \
    > /mnt/etc/network/interfaces
for s in devfs dmesg mdev; do chroot /mnt rc-update add $s sysinit; done
for s in hwclock modules sysctl hostname bootmisc networking; do
	chroot /mnt rc-update add $s boot
done
for s in mount-ro killprocs savecache; do
	chroot /mnt rc-update add $s shutdown
done
for s in cgroups $FLAVOR_SERVICE; do chroot /mnt rc-update add $s default; done
# An SSH server, with the host's key as the only way in. The host reaches
# the runtime's socket, or runs its command, through it; see vmm/graft.
for s in sshd; do chroot /mnt rc-update add $s default; done
# Alpine ships its SSH server with forwarding off, and the runtime's socket
# travels as a forwarded connection.
sed -i 's/^AllowTcpForwarding no/AllowTcpForwarding yes/' /mnt/etc/ssh/sshd_config
echo 'AllowStreamLocalForwarding yes' >> /mnt/etc/ssh/sshd_config
mkdir -p /mnt/root/.ssh
chmod 700 /mnt/root/.ssh
echo "$PUBKEY" > /mnt/root/.ssh/authorized_keys
chmod 600 /mnt/root/.ssh/authorized_keys
# A directory of the host, if one is shared: mounted where the host has it,
# so that a path means the same thing on both sides and "-v $PWD:/x" works
# as it would on the host. The path arrives on the kernel command line.
cat > /mnt/etc/init.d/graft-share <<'SHARE'
#!/sbin/openrc-run
description="Mount the directory shared by the host"

depend() {
	need localmount
	before containerd docker podman
}

start() {
	for arg in $(cat /proc/cmdline); do
		case "$arg" in
		graft.share=*)
			dir=${arg#graft.share=}
			# Somewhere of its own, never on top of the system.
			case "$dir" in
			/|/bin*|/etc*|/lib*|/proc*|/sbin*|/sys*|/usr*|/var*|/dev*|/run*) continue ;;
			/*) ;;
			*) continue ;;
			esac
			ebegin "Mounting $dir from the host"
			modprobe -a 9pnet_virtio 9p 2>/dev/null
			mkdir -p "$dir" && mount -t 9p \
			    -o trans=virtio,version=9p2000.L,msize=262144,cache=mmap \
			    share0 "$dir"
			eend $?
			;;
		esac
	done
	return 0
}
SHARE
chmod +x /mnt/etc/init.d/graft-share
chroot /mnt rc-update add graft-share default

# The service waits for "net"; do not let a missing card block it.
echo 'rc_need="!net"' >> "/mnt/etc/conf.d/$FLAVOR_SERVICE"
flavor_setup

rm -rf /mnt/var/cache/apk/*
echo "@@ROOTFS-USED: $(du -sm /mnt | cut -f1) MB@@"

step "compress the root"
tar -C /mnt/var/lib -cf /tmp/varlib.tar .
mksquashfs /mnt /tmp/root.squashfs -comp zstd -Xcompression-level 19 \
    -noappend -quiet -no-progress
dd if=/tmp/root.squashfs of=/dev/vdc bs=1M seek=$SQUASHFS_AT 2>/dev/null
echo "@@SQUASHFS-SIZE:$(stat -c %s /tmp/root.squashfs)@@"
rm -f /tmp/root.squashfs

step "the data disk"
sync
umount /mnt
mkfs.ext4 -q -F -L graftdata /dev/vda
mount -t ext4 /dev/vda /mnt
tar -C /mnt -xf /tmp/varlib.tar
if [ -n "${IMAGES:-}" ]; then
	step "container images in the store: $IMAGES"
	rc-service cgroups start || true
	flavor_pull /mnt $IMAGES
fi

step "unmount"
sync
umount /mnt
trap - EXIT
echo "@@RESULT:0@@"
