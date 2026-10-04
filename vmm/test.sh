#!/bin/sh
#
# test.sh — start the container VM with graft and run containers in
# it, by whichever way its image offers: an API socket, a command in the VM,
# or both. Needs what graft needs, and for an image with a socket, a
# docker client (named by $DOCKER, default "docker").
#
#   vmm/test.sh

set -eu
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ND="$HERE/graft"
DIR=${GRAFT_DIR:-"$HOME/.graft"}
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

# A directory of this Mac, bind-mounted by its own path: through graft-run's
# file server and back. Skipped when nothing is shared.
if [ -n "${GRAFT_SHARE-$HOME}" ]; then
	T=$(mktemp -d "${GRAFT_SHARE-$HOME}/.graft-test.XXXXXX")
	echo "from the mac" > "$T/in.txt"
	dd if=/dev/urandom of="$T/big" bs=1048576 count=64 2> /dev/null
	bind_mount() {	# bind_mount <runtime command...>
		"$@" run --rm -v "$T:/data" alpine sh -c \
		    'cat /data/in.txt && echo from a container > /data/out.txt &&
		     sha256sum < /data/big > /data/big.sum' | grep -q "from the mac" &&
		    grep -q container "$T/out.txt" &&
		    [ "$(shasum -a 256 < "$T/big")" = "$(cat "$T/big.sum")" ]
	}
	check "the VM has the directory at the same path" \
	    "$ND" ssh "grep -q mac '$T/in.txt'"
	[ -n "$FLAVOR_SOCKET" ] &&
	    check "docker client: a bind mount of a Mac directory" bind_mount "$DOCKER"
	[ -n "$FLAVOR_CLI" ] &&
	    check "$FLAVOR_CLI in the VM: a bind mount of a Mac directory" \
	    bind_mount "$ND" "$FLAVOR_CLI"
	rm -rf "$T"
fi

"$ND" ssh "dmesg | grep -i -E 'soft lockup|rcu.*stall|BUG:' | head -5"
[ $fail -eq 0 ] && echo "VM TEST PASSED ($FLAVOR)" || { echo "VM TEST FAILED ($FLAVOR)"; exit 1; }
