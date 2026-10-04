# guest-build.sh — runs inside the Alpine ISO, as root, to build what
# nvmm-run boots: a root filesystem on /dev/vda and a matching initramfs,
# written raw to /dev/vdc. build-image.exp hands this file to the guest as
# the disk /dev/vdb and runs it with "sh /dev/vdb".

set -e
trap 'echo "@@RESULT:1@@"' EXIT
[ -n "${PUBKEY:-}" ] || { echo "no PUBKEY given"; exit 1; }
step() { echo "@@STEP: $*"; }

REPO=https://dl-cdn.alpinelinux.org/alpine/v3.24
KVER=$(uname -r)

step "network and package repositories"
printf 'auto lo\niface lo inet loopback\n\nauto eth0\niface eth0 inet dhcp\n' \
    > /etc/network/interfaces
rc-service networking start
printf '%s/main\n%s/community\n' "$REPO" "$REPO" > /etc/apk/repositories
apk --no-progress update
apk --no-progress add e2fsprogs docker mkinitfs

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
    --repositories-file /etc/apk/repositories add alpine-base docker \
    e2fsprogs openssh-server

step "kernel modules: the ISO's own"
mkdir -p /mnt/lib/modules
cp -a "/.modloop/modules/$KVER" /mnt/lib/modules/

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
for s in cgroups docker; do chroot /mnt rc-update add $s default; done
# An SSH server, with the host's key (build-image.exp puts PUBKEY at the top
# of this script) as the only way in. The host reaches the Docker socket
# through it; see vmm/nvmm-docker.
for s in sshd; do chroot /mnt rc-update add $s default; done
# Alpine ships its SSH server with forwarding off, and the Docker socket
# travels as a forwarded connection.
sed -i 's/^AllowTcpForwarding no/AllowTcpForwarding yes/' /mnt/etc/ssh/sshd_config
echo 'AllowStreamLocalForwarding yes' >> /mnt/etc/ssh/sshd_config
mkdir -p /mnt/root/.ssh
chmod 700 /mnt/root/.ssh
echo "$PUBKEY" > /mnt/root/.ssh/authorized_keys
chmod 600 /mnt/root/.ssh/authorized_keys
# Docker's service waits for "net"; do not let a missing card block it.
echo 'rc_need="!net"' >> /mnt/etc/conf.d/docker

step "container images in the target's Docker store"
rc-service cgroups start || true
dockerd --data-root /mnt/var/lib/docker > /tmp/dockerd.log 2>&1 &
for i in $(seq 60); do docker info > /dev/null 2>&1 && break; sleep 2; done
docker info > /dev/null
docker pull -q hello-world
docker pull -q alpine
docker images
kill "$(pidof dockerd)"
for i in $(seq 30); do pidof dockerd > /dev/null || break; sleep 1; done

step "unmount"
sync
umount /mnt
trap - EXIT
echo "@@RESULT:0@@"
