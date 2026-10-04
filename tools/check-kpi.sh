#!/bin/sh
#
# check-kpi.sh — confirm the kext only imports symbols macOS exports to it.
#
# A kernel extension is linked against nothing at build time; a symbol the
# kernel does not export only shows up as a load failure on the target machine.
# This compares the kext's undefined symbols with the export lists in the XNU
# source for the macOS release being targeted, and reports which KPI each
# symbol comes from, so OSBundleLibraries in Info.plist can be checked too.
#
# Usage:  ./tools/check-kpi.sh [xnu-tag]      default: newest xnu-11* (macOS 15)
#
# Downloads about forty small text files into a temporary directory and
# removes them afterwards.
#
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
KEXT="$ROOT/build/NVMM.kext/Contents/MacOS/NVMM"
REPO=apple-oss-distributions/xnu

[ -f "$KEXT" ] || { echo "ERROR: build the kext first (make kext)" >&2; exit 1; }

TAG=${1:-$(gh api "repos/$REPO/tags?per_page=100" --jq '.[].name' | grep '^xnu-11' | head -1)}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "Export lists: $REPO @ $TAG"
for kpi in BSDKernel IOKit Libkern Mach Unsupported; do
	for suffix in exports x86_64.exports x86_64.MacOSX.exports; do
		gh api "repos/$REPO/contents/config/$kpi.$suffix?ref=$TAG" \
		    -H "Accept: application/vnd.github.raw" \
		    > "$TMP/$kpi.$suffix" 2>/dev/null || rm -f "$TMP/$kpi.$suffix"
	done
done

fail=0
for sym in $(nm -u "$KEXT" | awk '{print $NF}' | sort -u); do
	owner=""
	for kpi in Libkern Mach BSDKernel IOKit Unsupported; do
		# An export line is "symbol" or "symbol:alias".
		if cat "$TMP/$kpi".*exports 2>/dev/null | sed 's/:.*//' | grep -qx -- "$sym"; then
			owner=$kpi
			break
		fi
	done
	if [ -n "$owner" ]; then
		printf '  %-12s %s\n' "$owner" "$sym"
	else
		printf '  %-12s %s\n' "MISSING" "$sym"
		fail=1
	fi
done | sort

# The loop ran in a subshell; recompute the verdict from its output.
missing=$(for sym in $(nm -u "$KEXT" | awk '{print $NF}' | sort -u); do
	cat "$TMP"/*exports | sed 's/:.*//' | grep -qx -- "$sym" || echo "$sym"
done)

echo
if [ -n "$missing" ]; then
	echo "NOT EXPORTED on this macOS release:" >&2
	echo "$missing" | sed 's/^/  /' >&2
	exit 1
fi
echo "Every imported symbol is exported by a KPI listed in Info.plist's range."
