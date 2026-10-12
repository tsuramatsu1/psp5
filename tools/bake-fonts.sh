#!/usr/bin/env bash
# Bake psp5's font atlases over the kit's.
#
#   tools/bake-fonts.sh [out-dir] [ppsspp-lang-dir]
#
# The kit ships six faces holding 113 glyphs each - essentially ASCII. That is
# enough for English and nothing else: a game called "God of War(R)" drew a
# question mark, and so did every Japanese, Chinese, Korean, Arabic, Hebrew,
# Thai and Lao label in the interface. These bake the same six faces with the
# characters psp5 can actually draw.
#
# This script exists because the bake used to be a command someone typed. When
# the glyph set turned out to be too narrow there was nothing in the repo
# saying how the fonts had been made, and the fallback chain - the part that
# decides whether Chinese renders at all - had to be guessed at. It is here now,
# so a re-bake is one command and the chain is reviewable.
#
# Three faces carry the full set. The other three are decorative or fixed-width
# and are only ever used for Latin: a Japanese label never reaches them, and
# baking 2,600 glyphs into a pixel font would cost megabytes to no end.
#
# Needs: python3, pillow, numpy, fonttools, and the Noto families below
# (fonts-noto-core and fonts-noto-cjk on Debian and Ubuntu).
#
# Copyright (C) 2026 the psp5 authors
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:-$root/assets/fonts}
langs=${2:-${PSP5_SOURCE_DIR:-$root/.deps/ppsspp-src}/assets/lang}
kit=${PSP5_UI_KIT:-$root/.deps/hui}
ttf=$kit/third_party/fonts

[[ -d $langs ]] || { echo "error: no PPSSPP language files at $langs" >&2; exit 2; }
[[ -d $ttf ]] || { echo "error: no kit fonts at $ttf" >&2; exit 2; }
mkdir -p "$out"

noto=/usr/share/fonts/truetype/noto
notocjk=/usr/share/fonts/opentype/noto

# The order matters: the first face that has a character wins. Noto Sans first
# for Latin, Cyrillic and Greek, because its shapes sit closer to Inter's than
# the CJK family's do; then one family per script that is nobody else's.
fallback="$noto/NotoSans-Regular.ttf"
fallback+=",$notocjk/NotoSansCJK-Regular.ttc"
fallback+=",$noto/NotoSansArabic-Regular.ttf"
fallback+=",$noto/NotoSansHebrew-Regular.ttf"
fallback+=",$noto/NotoSansThai-Regular.ttf"
fallback+=",$noto/NotoSansLao-Regular.ttf"

# SOURCE is no longer read (see make-font.py), but the flag still takes it.
interface=("--interface" "$root/src/ui/PS5Settings.cpp" "$langs"
            "--own" "$root/assets/lang/psp5")

bake_wide() {
	local src=$1 name=$2
	printf '==> %s (full set)\n' "$name"
	python3 "$root/tools/make-font.py" "$ttf/$src" "$out/$name" \
		--scripts latin,cyrillic,greek --fallback "$fallback" "${interface[@]}"
}

bake_latin() {
	local src=$1 name=$2
	printf '==> %s (Latin only)\n' "$name"
	python3 "$root/tools/make-font.py" "$ttf/$src" "$out/$name" --scripts latin
}

bake_wide Inter-Regular.ttf inter-regular.huifont
bake_wide Inter-SemiBold.ttf inter-semibold.huifont
bake_wide Montserrat-Medium.ttf montserrat-medium.huifont

bake_latin DejaVuSansMono.ttf dejavu-sans-mono.huifont
bake_latin PatrickHand-Regular.ttf patrick-hand.huifont
bake_latin PressStart2P-Regular.ttf press-start-2p.huifont

printf '==> %s bytes in %s\n' "$(du -sb "$out" | cut -f1)" "$out"
