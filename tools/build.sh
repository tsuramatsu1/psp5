#!/usr/bin/env bash
# Cross-build psp5: PPSSPP as a standalone PlayStation 5 title.
#
# The pinned PPSSPP tree is configured directly as the top-level CMake project,
# because upstream resolves its module paths and source lists through
# ${CMAKE_SOURCE_DIR}. Everything psp5 adds lives in tooling/ps5-toolchain.cmake,
# patches/ppsspp/ and src/.
#
# Two patches are applied in order:
#   ps5-port.patch        the console port itself - the JIT's code cache, the guest
#                         memory arena, thread names, the RADV workarounds. Taken
#                         unmodified from PS5_RetroArch, where it is proven.
#   ps5-standalone.patch  what a title needs and a libretro core does not: the
#                         Vulkan loader against statically linked RADV, and a PS5
#                         branch in the platform selection that builds this title.
#
# PSP5_DEV=1 builds the tree as it stands (no reset, no patches), which is how the
# port is edited: change .deps/ppsspp-src, build with PSP5_DEV=1, then write the
# patch back with
#   git -C .deps/ppsspp-src diff <base> > patches/ppsspp/ps5-standalone.patch
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

# PPSSPP v1.20.4. Pinned, because the patches are written against this tree.
revision=fa50bb1976065c4f8b1b47af227d367fe9771555

# Note: no apostrophes in these messages. A single quote inside ${var:?word} starts
# a quoted section as far as the parser is concerned, and the whole script fails to
# parse with an unmatched-quote error pointing at the last line.
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the payload SDK fork (it needs target/include/ps5platform)}"
: "${PS5_RADV:?set PS5_RADV to the radv-release of PS5_Vulkan (it needs lib/libvulkan_radeon.ps5.a)}"

# zlib must be the one RADV was built with, not PPSSPP's bundled copy: RADV's
# archive is whole-archived into the title and carries its own, so two copies are a
# duplicate of every deflate*/inflate* symbol at link time. PS5_Vulkan keeps it
# beside the driver, so default to that and let it be overridden.
if [[ -z ${PS5_ZLIB:-} ]]; then
	PS5_ZLIB=$PS5_RADV/../zlib/root/usr
fi
[[ -f $PS5_ZLIB/lib/libz.a ]] ||
	{ echo "error: set PS5_ZLIB to the zlib RADV was built with (a prefix with lib/libz.a)" >&2; exit 2; }

export PS5_PAYLOAD_SDK PS5_RADV PS5_ZLIB

[[ -x $PS5_PAYLOAD_SDK/bin/prospero-clang ]] ||
	{ echo "error: no prospero-clang in $PS5_PAYLOAD_SDK/bin" >&2; exit 2; }
[[ -f $PS5_PAYLOAD_SDK/target/include/ps5platform/exec.h ]] ||
	{ echo "error: this SDK has no ps5platform/exec.h - psp5 needs the fork, not the upstream SDK" >&2; exit 2; }
[[ -f $PS5_RADV/lib/libvulkan_radeon.ps5.a ]] ||
	{ echo "error: no libvulkan_radeon.ps5.a in $PS5_RADV/lib" >&2; exit 2; }

source_dir="${PSP5_SOURCE_DIR:-$root/.deps/ppsspp-src}"
build_dir="$root/build/ps5"

# ---------------------------------------------------------------------------
# The source tree
# ---------------------------------------------------------------------------
# PPSSPP needs its submodules (ext/glslang, ext/armips, ffmpeg and the rest), so
# this is a pinned clone rather than a tarball.
if [[ ! -d $source_dir/.git ]]; then
	echo "==> fetching PPSSPP $revision (large: submodules included)"
	mkdir -p "$(dirname "$source_dir")"
	git clone --filter=blob:none --no-checkout https://github.com/hrydgard/ppsspp.git "$source_dir"
fi

if [[ -n ${PSP5_DEV:-} ]]; then
	echo "==> PSP5_DEV: building the tree as it stands"
else
	echo "==> resetting the pinned tree and applying the psp5 patches"
	git -C "$source_dir" cat-file -e "$revision^{commit}" 2>/dev/null ||
		git -C "$source_dir" fetch --quiet origin tag v1.20.4
	git -C "$source_dir" checkout --force --quiet "$revision"
	git -C "$source_dir" clean -qfdx -e build
	git -C "$source_dir" submodule update --init --recursive --jobs 8 --quiet

	# The ffmpeg submodule is a separate repository whose pinned commit is not on a
	# branch tip, so a filtered clone can miss it.
	ffmpeg_rev=$(git -C "$source_dir" ls-tree HEAD ffmpeg | awk '{print $3}')
	if ! git -C "$source_dir/ffmpeg" cat-file -e "$ffmpeg_rev^{commit}" 2>/dev/null; then
		git -C "$source_dir/ffmpeg" fetch --quiet origin '+refs/heads/*:refs/remotes/origin/*'
	fi
	git -C "$source_dir/ffmpeg" checkout --force --quiet "$ffmpeg_rev"

	git -C "$source_dir" apply --whitespace=nowarn "$root/patches/ppsspp/ps5-port.patch"
	git -C "$source_dir" apply --whitespace=nowarn "$root/patches/ppsspp/ps5-standalone.patch"
fi

# Reproducibility: libpng embeds __DATE__ and __TIME__ unless SOURCE_DATE_EPOCH is
# set, and that one varying string also shifts the linker's string merging. The
# pinned commit's own timestamp keeps the value self-documenting.
SOURCE_DATE_EPOCH=$(git -C "$source_dir" show -s --format=%ct "$revision")
export SOURCE_DATE_EPOCH

# ---------------------------------------------------------------------------
# Configure and build
# ---------------------------------------------------------------------------
# USE_FFMPEG is off until tools/build-ffmpeg.sh has run; the PSP's video and some
# of its audio need it, so a release build sets PSP5_FFMPEG_PREFIX.
ffmpeg_args=(-DUSE_FFMPEG=OFF)
if [[ -n ${PSP5_FFMPEG_PREFIX:-} ]]; then
	ffmpeg_args=(-DUSE_FFMPEG=ON -DUSE_SYSTEM_FFMPEG=OFF -DFFMPEG_DIR="$PSP5_FFMPEG_PREFIX")
fi

echo "==> configuring"
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$root/tooling/ps5-toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DPSP5_SRC_DIR="$root/src" \
	-DLIBRETRO=OFF -DUNITTEST=OFF -DHEADLESS=OFF -DUSE_CCACHE=OFF \
	-DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF \
	-DUSE_SYSTEM_LIBPNG=OFF -DUSE_SYSTEM_ZSTD=OFF -DUSE_SYSTEM_LIBZIP=OFF \
	-DUSE_SYSTEM_FREETYPE=OFF \
	-DUSING_GLES2=OFF -DUSE_WAYLAND_WSI=OFF -DUSING_X11_VULKAN=OFF \
	-DUSE_VULKAN_DISPLAY_KHR=ON \
	"${ffmpeg_args[@]}"

echo "==> building"
cmake --build "$build_dir" --parallel "${JOBS:-$(nproc)}"

# CMake produces archives, not an executable: the console's link is tools/link-title.sh.
printf '==> built %s and %s in %s\n' libpsp5_platform.a libpsp5_app.a "$build_dir/lib"
echo "    now: tools/link-title.sh (needs PS5_VULKAN) to link and sign the title"
