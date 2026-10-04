# containerd — no Docker at all: containerd, and nerdctl to drive it, which
# takes docker's command line. The smallest and lightest, but there is no
# Docker API: nerdctl runs inside the VM, and nothing that wants docker.sock
# works. There is no BuildKit either, so no "nerdctl build".

FLAVOR_PKGS="containerd containerd-openrc nerdctl cni-plugins iptables crun"
FLAVOR_BUILD_PKGS="containerd nerdctl cni-plugins iptables"
FLAVOR_SERVICE=containerd
FLAVOR_SOCKET=
FLAVOR_CLI=nerdctl

flavor_setup() {
	# crun does what runc does, takes the same command line, and is a
	# twentieth of its size. containerd asks for "runc" by name.
	rm -f /mnt/usr/bin/runc
	ln -s crun /mnt/usr/bin/runc
	# Tools for debugging containerd itself.
	rm -f /mnt/usr/bin/ctr /mnt/usr/bin/containerd-stress

	# Of the network plugins, only those a single host uses.
	for f in /mnt/usr/libexec/cni/*; do
		case "$(basename "$f")" in
		bridge|host-local|loopback|portmap|firewall|tuning) ;;
		*) rm -f "$f" ;;
		esac
	done
}

flavor_pull() {
	varlib=$1
	shift
	containerd --root "$varlib/containerd" > /tmp/daemon.log 2>&1 &
	for i in $(seq 60); do nerdctl info > /dev/null 2>&1 && break; sleep 2; done
	nerdctl info > /dev/null
	for img in "$@"; do nerdctl pull -q "$img"; done
	kill "$(pidof containerd)"
	for i in $(seq 30); do pidof containerd > /dev/null || break; sleep 1; done
}
