# guest-build.sh — runs inside the Alpine ISO, as root, to build what
# nvmm-run boots: a root filesystem on /dev/vda and a matching initramfs,
# written raw to /dev/vdc. build-image.exp hands this file to the guest as
# the disk /dev/vdb and runs it with "sh /dev/vdb", after putting in front of
# it the host's public key (PUBKEY), the container images to pull into the
# image if any (IMAGES), and one of the files in flavors/, which says what
# the image runs containers with.

set -e
trap 'echo "@@RESULT:1@@"' EXIT
[ -n "${PUBKEY:-}" ] || { echo "no PUBKEY given"; exit 1; }
[ -n "${FLAVOR_SERVICE:-}" ] || { echo "no flavor given"; exit 1; }
step() { echo "@@STEP: $*"; }

REPO=https://dl-cdn.alpinelinux.org/alpine/v3.24
KVER=$(uname -r)

step "network and package repositories"
printf 'auto lo\niface lo inet loopback\n\nauto eth0\niface eth0 inet dhcp\n' \
    > /etc/network/interfaces
rc-service networking start
printf '%s/main\n%s/community\n' "$REPO" "$REPO" > /etc/apk/repositories
apk --no-progress update
apk --no-progress add e2fsprogs mkinitfs
[ -z "${IMAGES:-}" ] || apk --no-progress add $FLAVOR_BUILD_PKGS

step "initramfs with the disk and filesystem drivers, for kernel $KVER"
mkinitfs -F "base ext4 virtio network" -o /tmp/initramfs-nvmm "$KVER"
dd if=/tmp/initramfs-nvmm of=/dev/vdc bs=1M 2>/dev/null
echo "@@INITRAMFS-SIZE:$(stat -c %s /tmp/initramfs-nvmm)@@"

step "root filesystem: ext4 on the whole disk"
mkfs.ext4 -q -F -L nvmmroot /dev/vda
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
echo '/dev/vda / ext4 rw,relatime 0 1' > /mnt/etc/fstab
echo nvmm > /mnt/etc/hostname
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
# The service waits for "net"; do not let a missing card block it.
echo 'rc_need="!net"' >> "/mnt/etc/conf.d/$FLAVOR_SERVICE"
flavor_setup

if [ -n "${IMAGES:-}" ]; then
	step "container images in the target's store: $IMAGES"
	rc-service cgroups start || true
	flavor_pull $IMAGES
fi
rm -rf /mnt/var/cache/apk/*
echo "@@ROOTFS-USED: $(du -sm /mnt | cut -f1) MB@@"

step "unmount"
sync
umount /mnt
trap - EXIT
echo "@@RESULT:0@@"
