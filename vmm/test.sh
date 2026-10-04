#!/bin/sh
#
# test.sh — start the container VM with nvmm-docker and run containers in
# it, by whichever way its image offers: an API socket, a command in the VM,
# or both. Needs what nvmm-docker needs, and for an image with a socket, a
# docker client (named by $DOCKER, default "docker").
#
#   vmm/test.sh

set -eu
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ND="$HERE/nvmm-docker"
DIR=${NVMM_DOCKER_DIR:-"$HOME/.nvmm-docker"}
DOCKER=${DOCKER:-docker}
FLAVOR_SOCKET=
FLAVOR_CLI=
. "$DIR/flavor.env"

fail=0
check() {	# check <what> <command...>
	what=$1
	shift
	if "$@" > "$DIR/test.log" 2>&1; then
		echo "ok    $what"
	else
		echo "FAIL  $what"
		tail -5 "$DIR/test.log"
		fail=1
	fi
}
port_answers() {
	i=0
	while [ $i -lt 30 ]; do
		curl -s -m 2 "http://127.0.0.1:$1" | grep -q nginx && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

"$ND" start
trap '"$ND" stop > /dev/null 2>&1 || true' EXIT

if [ -n "$FLAVOR_SOCKET" ]; then
	export DOCKER_HOST="unix://$DIR/docker.sock"
	check "docker client: a container runs" \
	    sh -c "$DOCKER run --rm alpine echo hello | grep -q hello"
	check "docker client: input is piped through" \
	    sh -c "echo piped | $DOCKER run --rm -i alpine cat | grep -q piped"
	check "docker client: a container is started with a published port" \
	    "$DOCKER" run -d --rm --name graft-test-web -p 18080:80 nginx:alpine
	check "docker client: the port answers on this Mac" port_answers 18080
	"$DOCKER" rm -f graft-test-web > /dev/null 2>&1 || true
fi
if [ -n "$FLAVOR_CLI" ]; then
	check "$FLAVOR_CLI in the VM: a container runs" \
	    sh -c "'$ND' $FLAVOR_CLI run --rm alpine echo hello | grep -q hello"
	check "$FLAVOR_CLI in the VM: a container is started with a published port" \
	    "$ND" "$FLAVOR_CLI" run -d --name graft-test-web2 -p 18081:80 nginx:alpine
	check "$FLAVOR_CLI in the VM: the port answers on this Mac" port_answers 18081
	"$ND" "$FLAVOR_CLI" rm -f graft-test-web2 > /dev/null 2>&1 || true
fi

"$ND" ssh "dmesg | grep -i -E 'soft lockup|rcu.*stall|BUG:' | head -5"
[ $fail -eq 0 ] && echo "VM TEST PASSED ($FLAVOR)" || { echo "VM TEST FAILED ($FLAVOR)"; exit 1; }
