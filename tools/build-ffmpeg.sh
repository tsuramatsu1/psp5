#!/usr/bin/env bash
# Cross-build the FFmpeg PPSSPP pins, for the PSP's video and part of its audio.
#
# Without it a game's intro and menu backgrounds are undecoded garbage and Atrac3
# audio is silent, so a release build needs this. It is a separate script because
# it is slow and does not change, while tools/build.sh is run over and over.
#
# The decoder, demuxer and parser set is the one PPSSPP's own linux_x86-64.sh
# selects, built without assembly (as that script does) and without zlib. The
# source is the tree's own ffmpeg submodule, not a system FFmpeg: PPSSPP pins a
# 3.0-era branch of its own and does not build against a current one.
#
#   ./tools/build-ffmpeg.sh
#   PSP5_FFMPEG_PREFIX=$PWD/build/ffmpeg ./tools/build.sh
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the payload SDK fork}"
sdk=$PS5_PAYLOAD_SDK
[[ -x $sdk/bin/prospero-clang ]] || { echo "error: no prospero-clang in $sdk/bin" >&2; exit 2; }

source_dir="${PSP5_SOURCE_DIR:-$root/.deps/ppsspp-src}"
[[ -d $source_dir/ffmpeg ]] ||
	{ echo "error: no ffmpeg submodule in $source_dir - run tools/build.sh first" >&2; exit 2; }

prefix="$root/build/ffmpeg"
work="$root/build/ffmpeg-build"

# The console's CPU is a Zen 2, but the assembler paths are off (as upstream's own
# script does for this tree), so --cpu buys nothing here and is left out.
flags=(
	--prefix="$prefix"
	--enable-cross-compile --target-os=freebsd --arch=x86_64
	--cc="$sdk/bin/prospero-clang" --ar="$sdk/bin/prospero-ar"
	--ranlib="$sdk/bin/prospero-ranlib" --nm=nm
	--disable-shared --enable-static --enable-pic --disable-asm --disable-zlib
	--disable-everything --disable-avdevice --disable-filters --disable-programs
	--disable-network --disable-avfilter --disable-postproc --disable-encoders
	--disable-doc --disable-debug
	--extra-cflags="-D__STDC_CONSTANT_MACROS -O2 -fPIC -w"
	--enable-decoder=h264,mpeg4,h263,h263p,mpeg2video,mjpeg,mjpegb,aac,aac_latm,atrac3,atrac3p,mp3,pcm_s16le,pcm_s8
	--enable-demuxer=h264,h263,m4v,mpegps,mpegvideo,avi,mp3,aac,pmp,oma,pcm_s16le,pcm_s8,wav
	--enable-parser=h264,mpeg4video,mpegvideo,aac,aac_latm,mpegaudio
	--enable-protocol=file
)

# Rebuilt only when the configuration or the submodule's commit changes: this is a
# several-minute build that otherwise runs on every invocation.
stamp=$(printf '%s\n' "${flags[@]}" "$(git -C "$source_dir/ffmpeg" rev-parse HEAD)" |
	sha256sum | cut -c1-16)
if [[ -f $prefix/lib/libavcodec.a && $(cat "$prefix/stamp" 2>/dev/null) == "$stamp" ]]; then
	echo "==> ffmpeg is up to date ($prefix)"
	exit 0
fi

echo "==> building FFmpeg for the PS5"
rm -rf -- "$work" "$prefix"
mkdir -p "$work"
(
	cd "$work"
	"$source_dir/ffmpeg/configure" "${flags[@]}" > configure.log 2>&1
	make -j"${JOBS:-$(nproc)}" > make.log 2>&1
	make install > install.log 2>&1
) || { echo "error: the FFmpeg build failed; see $work/*.log" >&2; exit 1; }

echo "$stamp" > "$prefix/stamp"
echo "==> built $prefix"
echo "    now: PSP5_FFMPEG_PREFIX=$prefix ./tools/build.sh"
