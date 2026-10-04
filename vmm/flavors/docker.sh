# docker — the Docker daemon. Its client and plugins are not installed: they
# run on the host, which reaches the daemon through its socket.

FLAVOR_PKGS="docker-engine docker-openrc"	# installed in the image
FLAVOR_BUILD_PKGS="docker"			# needed while building, to pull
FLAVOR_SERVICE=docker				# started at boot
FLAVOR_SOCKET=/var/run/docker.sock		# what the host is given, if anything
FLAVOR_CLI=					# what is run inside the VM, if anything

flavor_setup() { :; }

# flavor_pull <var-lib> <image>...: pull into the store kept under that
# directory, which is the VM's /var/lib.
flavor_pull() {
	varlib=$1
	shift
	dockerd --data-root "$varlib/docker" > /tmp/daemon.log 2>&1 &
	for i in $(seq 60); do docker info > /dev/null 2>&1 && break; sleep 2; done
	docker info > /dev/null
	for img in "$@"; do docker pull -q "$img"; done
	kill "$(pidof dockerd)"
	for i in $(seq 30); do pidof dockerd > /dev/null || break; sleep 1; done
}
