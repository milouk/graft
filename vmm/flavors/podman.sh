# podman — Podman, with its service answering the Docker API. A docker client
# on the host works against it, as does a podman client; and podman itself
# can be run inside the VM.

FLAVOR_PKGS="podman podman-openrc"
FLAVOR_BUILD_PKGS="podman"
FLAVOR_SERVICE=podman
FLAVOR_SOCKET=/run/podman/podman.sock
FLAVOR_CLI=podman

flavor_setup() { :; }

# Podman has no daemon to start: it writes straight into the store.
flavor_pull() {
	for img in "$@"; do
		podman --root /mnt/var/lib/containers/storage pull -q \
		    "docker.io/library/$img"
	done
}
