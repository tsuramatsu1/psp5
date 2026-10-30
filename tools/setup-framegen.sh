#!/usr/bin/env bash
# Export PS5_FrameGen, the frame generator, at a pinned revision.
#
#   tools/setup-framegen.sh   exports into .deps/framegen (once a revision); prints the path
#
# PS5_MihawkGen is mihawk-99's neural frame generator: about 0.43M parameters as
# Vulkan compute passes, which on this console make a frame between two rendered
# ones in a couple of milliseconds. psp5 uses FG_METHOD_NEURAL and nothing else -
# it is the only method that reads the two colour frames alone, and colour is all
# a PSP game gives us. There are no motion vectors to hand it and its depth
# buffer is the emulated one, which does not describe the picture on screen.
#
# Exported rather than vendored, the same way tools/setup-kit.sh exports the UI
# kit: the generator is 2.6 MB of someone else's work, 1.7 MB of that a weights
# file, and a pinned export keeps it out of psp5's history while keeping a build
# reproducible.
#
# PS5_FrameGen is GPL-3.0-or-later, which psp5 already is.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/.deps/framegen"
fork="${PS5_MIHAWKGEN:-$root/../PS5_MihawkGen}"

# c57232a: v1.0.0, network d5 - the released weights.
revision=c57232a9f16bcb6df5fe07c9bb631518f2b1cc0e

# What is exported beside the revision: changing this list exports again.
# framegen/src/shaders holds the compiled SPIR-V as headers, so nothing of the
# generator has to be loaded from disk except its weights.
stamp="$revision framegen"

if [[ -f $out/.revision && $(<"$out/.revision") == "$stamp" ]]; then
	echo "$out"
	exit 0
fi

if ! git -C "$fork" cat-file -e "$revision^{commit}" 2>/dev/null; then
	fork="$root/.deps/PS5_MihawkGen.git"
	[[ -d $fork ]] || git clone --quiet --bare \
		https://github.com/mihawk-99/PS5_MihawkGen.git "$fork" >&2
	git -C "$fork" fetch --quiet origin "$revision" >&2 || true
	git -C "$fork" cat-file -e "$revision^{commit}" 2>/dev/null || {
		echo "PS5_MihawkGen $revision is neither in a sibling checkout nor on GitHub" >&2
		exit 2
	}
fi

rm -rf -- "$out"
mkdir -p "$out"
git -C "$fork" archive "$revision" \
	framegen/include framegen/src framegen/models \
	LICENSE.md THIRD_PARTY_NOTICES.txt |
	tar -x -C "$out"

[[ -f $out/framegen/models/network.fgw ]] ||
	{ echo "error: the export has no network.fgw" >&2; exit 2; }

echo "$stamp" > "$out/.revision"
echo "$out"
