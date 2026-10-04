# podman — Podman, with its service answering the Docker API. A docker client
# on the host works against it, as does a podman client; and podman itself
# can be run inside the VM.

FLAVOR_PKGS="podman podman-openrc iptables"
FLAVOR_BUILD_PKGS="podman"
FLAVOR_SERVICE=podman
FLAVOR_SOCKET=/run/podman/podman.sock
FLAVOR_CLI=podman

flavor_setup() { :; }

# Podman has no daemon to start: it writes straight into the store. It does
# not guess a registry for a short name, so one is given the way docker
# would: a first component with a dot or a colon in it, or "localhost", is a
# registry already.
flavor_pull() {
	store=$1/containers/storage
	shift
	for img in "$@"; do
		case "$img" in
		*/*)	case "${img%%/*}" in
			*.*|*:*|localhost) ;;
			*) img="docker.io/$img" ;;
			esac ;;
		*)	img="docker.io/library/$img" ;;
		esac
		podman --root "$store" pull -q "$img"
	done
	# Podman's own database records the store as being under /mnt, and
	# would refuse the one it finds at boot. It holds nothing yet: the
	# images are in the store itself.
	rm -rf "$store/db.sql" "$store/libpod"
}
