#!/usr/bin/env bash
# Copy the Windows working tree into the WSL build tree.
#
#   tools/sync-wsl.sh [windows-tree] [wsl-tree]
#
# psp5 is edited on Windows and built in WSL, and /root/psp5 is a copy rather
# than a mount: a build reads the copy, so the copy has to be refreshed first.
#
# --delete is what makes it a copy and not a merge - a file deleted on Windows
# has to go here too, or a stale source keeps building. That is also what makes
# this script worth having: `rsync -a --delete` run by hand, without these
# excludes, deletes everything the WSL tree has and Windows does not. Three
# things live only here:
#
#   env.sh      the machine's paths (SDK, RADV, ffmpeg prefix). Without it
#               build.sh stops at its first ${var:?} check.
#   build/      the CMake tree, and build/ffmpeg - hours of cross-built FFmpeg
#               that nothing else can hand back.
#   .deps/      the pinned PPSSPP checkout and the UI kit, fetched by build.sh
#               and setup-kit.sh.
#
# Deleting those costs a full rebuild, so they are excluded here rather than
# remembered at the prompt.
#
# Copyright (C) 2026 the psp5 authors
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

windows=${1:-/mnt/c/Users/Tsuramatsu/Documents/Repos/psp5}
wsl=${2:-/root/psp5}

[[ -d $windows ]] || { echo "error: no Windows tree at $windows" >&2; exit 2; }
[[ -d $wsl ]] || { echo "error: no WSL tree at $wsl" >&2; exit 2; }

rsync -a --delete \
	--exclude '.git/' \
	--exclude 'env.sh' \
	--exclude 'build/' \
	--exclude '.deps/' \
	--exclude 'dist/' \
	-- "$windows/" "$wsl/"

printf '==> synced %s -> %s\n' "$windows" "$wsl"
