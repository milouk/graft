#!/bin/sh
#
# install-autostart.sh — load the driver at boot and start the VM at login.
#
#   sudo tools/install-autostart.sh              install, and start now
#   sudo tools/install-autostart.sh --uninstall  remove
#
# Two jobs are installed:
#
#   - A system job that loads NVMM.kext from a root-owned copy and gives
#     /dev/nvmm to the user who ran this, and to nobody else.
#   - A job of that user that runs "graft start" when they log in, once the
#     device is there.
#
# The VM's size, and anything else "graft" takes from its environment, is
# read at each start from ~/.graft/vm.env if that file exists, for example:
#
#   GRAFT_CPUS=4
#   GRAFT_MEM=8192
#
# Build first: make kext vmm.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WHO=${SUDO_USER:-}
LIB="/Library/Application Support/Graft"
DAEMON=/Library/LaunchDaemons/sh.graft.driver.plist

[ "$(id -u)" -eq 0 ] && [ -n "$WHO" ] && [ "$WHO" != root ] ||
    { echo "run this with sudo, as the user the VM is for" >&2; exit 1; }
HOME_DIR=$(dscl . -read "/Users/$WHO" NFSHomeDirectory | awk '{print $2}')
USER_ID=$(id -u "$WHO")
AGENT="$HOME_DIR/Library/LaunchAgents/sh.graft.vm.plist"

if [ "${1:-}" = "--uninstall" ]; then
	launchctl bootout "gui/$USER_ID" "$AGENT" 2>/dev/null || true
	launchctl bootout system "$DAEMON" 2>/dev/null || true
	rm -f "$AGENT" "$DAEMON"
	rm -rf "$LIB"
	echo "removed; the driver stays loaded until the next restart"
	exit 0
fi

[ -d "$ROOT/build/NVMM.kext" ] && [ -x "$ROOT/build/graft-run" ] ||
    { echo "nothing built in $ROOT/build; run: make kext vmm" >&2; exit 1; }

# A kext is only loaded from a copy that root owns.
mkdir -p "$LIB"
rm -rf "$LIB/NVMM.kext"
cp -R "$ROOT/build/NVMM.kext" "$LIB/NVMM.kext"
chown -R root:wheel "$LIB"
chmod -R go-w "$LIB"

cat > "$LIB/load-driver" <<EOF
#!/bin/sh
# Installed by tools/install-autostart.sh. Loads the driver, then hands the
# device to its one user.
[ -e /dev/nvmm ] || /usr/bin/kmutil load -p "$LIB/NVMM.kext" ||
    /sbin/kextload "$LIB/NVMM.kext"
i=0
while [ ! -e /dev/nvmm ] && [ \$i -lt 20 ]; do sleep 0.5; i=\$((i + 1)); done
chown "$WHO" /dev/nvmm && chmod 600 /dev/nvmm
EOF
chmod 755 "$LIB/load-driver"

cat > "$DAEMON" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>sh.graft.driver</string>
	<key>ProgramArguments</key>
	<array>
		<string>$LIB/load-driver</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>StandardErrorPath</key>
	<string>/var/log/graft-driver.log</string>
	<key>StandardOutPath</key>
	<string>/var/log/graft-driver.log</string>
</dict>
</plist>
EOF
chown root:wheel "$DAEMON"
chmod 644 "$DAEMON"

# "graft start" leaves the VM and its network helper running and returns;
# launchd must not take them down with it (AbandonProcessGroup).
sudo -u "$WHO" mkdir -p "$HOME_DIR/Library/LaunchAgents"
cat > "$AGENT" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>sh.graft.vm</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/sh</string>
		<string>-c</string>
		<string>i=0; while [ ! -r /dev/nvmm ] &amp;&amp; [ \$i -lt 60 ]; do sleep 1; i=\$((i + 1)); done; set -a; [ -f "$HOME_DIR/.graft/vm.env" ] &amp;&amp; . "$HOME_DIR/.graft/vm.env"; set +a; exec "$ROOT/vmm/graft" start</string>
	</array>
	<key>EnvironmentVariables</key>
	<dict>
		<key>PATH</key>
		<string>$HOME_DIR/.graft/bin:$ROOT/build:/usr/bin:/bin:/usr/sbin:/sbin</string>
	</dict>
	<key>RunAtLoad</key>
	<true/>
	<key>AbandonProcessGroup</key>
	<true/>
	<key>StandardErrorPath</key>
	<string>$HOME_DIR/.graft/autostart.log</string>
	<key>StandardOutPath</key>
	<string>$HOME_DIR/.graft/autostart.log</string>
</dict>
</plist>
EOF
chown "$WHO" "$AGENT"
chmod 644 "$AGENT"

launchctl bootout system "$DAEMON" 2>/dev/null || true
launchctl bootstrap system "$DAEMON"
launchctl bootout "gui/$USER_ID" "$AGENT" 2>/dev/null || true
launchctl bootstrap "gui/$USER_ID" "$AGENT" 2>/dev/null ||
    echo "the VM will start at $WHO's next login"

echo "installed: the driver loads at boot, and the VM starts when $WHO logs in"
echo "  VM size and other settings: $HOME_DIR/.graft/vm.env"
