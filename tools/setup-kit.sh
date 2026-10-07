#!/usr/bin/env bash
# Export the UI kit psp5's launcher is drawn with, at a pinned revision.
#
#   tools/setup-kit.sh      exports into .deps/hui (once a revision); prints the path
#
# The kit is PS5_VKHomebrewUI, mihawk-99's fork of BlackBearReloaded's
# ps5-homebrew-ui with a Vulkan backend. psp5 uses its Aurora Shelf design as the
# home screen; PPSSPP's own UI stays for everything after a game is chosen.
#
# Exported with git archive from a sibling checkout (../PS5_VKHomebrewUI, or
# PS5_VKHOMEBREWUI) or from GitHub, so a build never depends on a working tree.
#
# The kit is GPL-3.0-or-later, which psp5 already is.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit="$root/.deps/hui"
fork="${PS5_VKHOMEBREWUI:-$root/../PS5_VKHomebrewUI}"

# 2188642: the Vulkan backend, with import_texture (a program's own image).
revision=2188642afbcc8d84d36149124f3d9e39dc1b1a05

# What is exported beside the revision: changing this list exports again.
stamp="$revision src assets/fonts assets/audio third_party/fonts"

if [[ -f $kit/.revision && $(<"$kit/.revision") == "$stamp" ]]; then
	echo "$kit"
	exit 0
fi

if ! git -C "$fork" cat-file -e "$revision^{commit}" 2>/dev/null; then
	fork="$root/.deps/PS5_VKHomebrewUI.git"
	[[ -d $fork ]] || git clone --quiet --bare \
		https://github.com/mihawk-99/PS5_VKHomebrewUI.git "$fork" >&2
	git -C "$fork" fetch --quiet origin "$revision" >&2 || true
	git -C "$fork" cat-file -e "$revision^{commit}" 2>/dev/null || {
		echo "PS5_VKHomebrewUI $revision is neither in a sibling checkout nor on GitHub" >&2
		exit 2
	}
fi

rm -rf -- "$kit"
mkdir -p "$kit"
git -C "$fork" archive "$revision" \
	src assets/fonts assets/audio third_party/fonts LICENSE THIRD_PARTY_NOTICES.md |
	tar -x -C "$kit"
echo "$stamp" > "$kit/.revision"
echo "$kit"
