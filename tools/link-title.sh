#!/usr/bin/env bash
# Link psp5's objects into a title and stage its folder.
#
#   PS5_PAYLOAD_SDK=... PS5_VULKAN=... ./tools/link-title.sh
#
# CMake does not link this title. The console's link is prospero-lld driven by
# PS5_Vulkan's own recipe - its linker script, RADV whole-archived, the heap and
# thread wraps, libc's names bound to the platform layer's ps5_* - followed by a
# native tool that turns the ELF into the console's format and signs it. So
# tools/build.sh produces static libraries and this script does the rest, the way
# PS5_VulkanTemplate's ps5/tools/link-title.sh does for its own titles.
#
# PS5_VULKAN must be a built PS5_Vulkan checkout: this needs its RADV archive, its
# recipe, its CRT, its native tool and its libc.prx, not just the headers.
set -euo pipefail
[[ -n ${LINK_TRACE:-} ]] && set -x

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the payload SDK fork}"
: "${PS5_VULKAN:?set PS5_VULKAN to a built PS5_Vulkan checkout}"
sdk=$PS5_PAYLOAD_SDK
vulkan=$PS5_VULKAN

build="$root/build/ps5"
work="$build/link"
param="$root/sce_sys/param.json"
archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
tool="$vulkan/build/host/ps5-native-tool"
native="$vulkan/tooling/native"
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

for file in "$archive" "$tool" "$param" "$sdk/bin/prospero-lld" \
	"$vulkan/tools/radv-link.sh" "$vulkan/runtime/libc.prx" \
	"$native/app_crt.cpp" "$native/app-symbols.map" \
	"$build/lib/libpsp5_platform.a"; do
	[[ -e $file ]] || { echo "missing: $file" >&2; exit 2; }
done

mkdir -p "$work/obj" "$work/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

# The CRT. PPSSPP throws, so this is the exception-capable one.
cc -std=c++20 -O2 -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"

# RADV calls AGC, which the SDK ships no stubs for: these name its imports.
stub() {
	cc -std=c11 -O2 -fPIC -c "$vulkan/$2" -o "$work/obj/$1_stub.o"
	"$sdk/bin/prospero-lld" --shared -soname "$1.prx" -o "$work/stubs/$1.so" "$work/obj/$1_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2

# psp5's own archives, plus everything PPSSPP's build produced. The platform
# archive is whole-archived because main() and the System_* definitions are only
# referenced from the CRT and from PPSSPP's own code respectively, and an archive
# member nothing references strongly is dropped.
psp5_libs=(--whole-archive "$build/lib/libpsp5_platform.a" --no-whole-archive
	"$build/lib/libpsp5_app.a" "$build/lib/libCore.a" "$build/lib/libCommon.a")
mapfile -t extra_libs < <(find "$build/lib" -name '*.a' \
	! -name 'libpsp5_platform.a' ! -name 'libpsp5_app.a' \
	! -name 'libCore.a' ! -name 'libCommon.a' | sort)

# FFmpeg, when tools/build-ffmpeg.sh has run: it builds outside the CMake tree, so
# its archives are not under build/lib and have to be named here. Without them the
# link fails on avcodec_*, and the PSP's video and Atrac3 audio are the reason to
# have built it.
ffmpeg_prefix=${PSP5_FFMPEG_PREFIX:-$root/build/ffmpeg}
ffmpeg_libs=()
if [[ -f $ffmpeg_prefix/lib/libavcodec.a ]]; then
	# avformat before avcodec before avutil: each uses the next.
	for name in libavformat libavcodec libswscale libswresample libavutil; do
		[[ -f $ffmpeg_prefix/lib/$name.a ]] && ffmpeg_libs+=("$ffmpeg_prefix/lib/$name.a")
	done
	printf '==> linking with FFmpeg from %s\n' "$ffmpeg_prefix"
else
	echo "==> no FFmpeg (tools/build-ffmpeg.sh); PSP video and Atrac3 audio will be missing"
fi

# --no-dynamic-linker is what keeps this title convertible.
#
# Mesa names every Vulkan entry point in its dispatch tables through a weak
# reference and leaves the ones this driver does not implement undefined on
# purpose: at run time they read as null, and Mesa checks for null before calling.
# About 6,400 radv_* and annotate_* names arrive that way. Ordinarily lld keeps an
# undefined weak symbol in .dynsym so a dynamic linker could still resolve it, and
# the converter requires a stub for every entry in .dynsym
# (tooling/native/sce_module_writer.cpp) - so it stops at the first one,
# "no public SDK stub exports required symbol radv_EnumeratePhysicalDevices".
#
# There is no dynamic linker here: the console's loader binds this title's imports
# itself. Saying so lets lld bind those weak references to zero at link time and
# leave them out of the dynamic table, which is exactly the null Mesa expects.
#
# Two things that do not work instead: a version script cannot localise a symbol
# that is not defined (lld refuses), and -z nodynamic-undefined-weak is not in this
# lld (18.1.3 warns "unknown -z value" and carries on).
echo "==> linking"
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
	--version-script "$native/app-symbols.map" --exclude-libs=ALL \
	--no-dynamic-linker \
	-e _start -o "$work/llvm-pie.elf" \
	"$work/obj/app_crt.o" \
	--start-group "${psp5_libs[@]}" "${extra_libs[@]}" ${ffmpeg_libs[@]+"${ffmpeg_libs[@]}"} --end-group \
	"$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
	"${radv_link_inputs[@]}" \
	--as-needed "$sdk"/target/lib/*.so

# A title loads neither libkernel_sys's exports nor libScePosixForWebKit's: an
# import only their stubs define links, but is null at run time, so its first call
# jumps to address 0. Refuse that here rather than on the console.
null_imports=$(comm -23 \
	<("$sdk/bin/llvm-nm" -D --undefined-only "$work/llvm-pie.elf" |
		awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
	<(for library in "$sdk"/target/lib/*.so "$work/stubs"/*.so; do
		case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
		"$sdk/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
	done | sort -u))
if [[ -n $null_imports ]]; then
	echo "link-title.sh: imports no module a title loads exports (null at run time): ${null_imports//$'\n'/ }" >&2
	echo "link-title.sh: define them in src/PS5LibcShims.cpp, or bind them to the platform layer" >&2
	exit 1
fi

"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
	--stub-dir "$sdk/target/lib" --stub "$work/stubs/libSceAgc.so" \
	--stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
	--companion-sdk 0x08050001 --file-name eboot.elf

# ---------------------------------------------------------------------------
# The title folder
# ---------------------------------------------------------------------------
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
app="$root/dist/$title_id"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"

# eboot.bin is the converted module ELF, not a signed fake-SELF.
#
# Measured, not assumed: psp5 launches on the console with the raw module ELF, and
# every other homebrew title there carries one too (PS5SX2, Vita3K, PS5X360 - all
# ELF magic at the head of eboot.bin). The launch failures that looked like a
# signing problem were a registration problem; see docs/porting-notes.md.
#
# PSP5_SIGN_EBOOT=1 produces the signed fake-SELF instead, for a loader that wants
# one. Both forms are staged in dist/alternatives either way.
if [[ -n ${PSP5_SIGN_EBOOT:-} ]]; then
	"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
	"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
	echo "==> eboot.bin is a signed fake-SELF (PSP5_SIGN_EBOOT)"
else
	cp -- "$work/eboot.elf" "$app/eboot.bin"
fi

# Both forms staged beside the title, so either can be swapped in without a relink.
mkdir -p "$root/dist/alternatives"
cp -- "$work/eboot.elf" "$root/dist/alternatives/eboot.elf.bin"
"$tool" self --sign --in "$work/eboot.elf" --out "$root/dist/alternatives/eboot.signed.bin" \
	--magic 0x1D3D154F

cp "$param" "$app/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
	[[ -f $root/sce_sys/$asset ]] && cp "$root/sce_sys/$asset" "$app/sce_sys/$asset"
done
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$app/sce_module/libc.prx"

# PPSSPP's asset tree: the flash0 fonts, the languages, the shaders, the themes and
# the compatibility tables. Without it a game boots complaining that the core system
# files are missing, and text and effects are gone. The source checkout is the
# canonical copy - CMake's install rules into the build directory omit a few files.
source_dir="${PSP5_SOURCE_DIR:-$root/.deps/ppsspp-src}"
assets="$source_dir/assets"
[[ -d $assets ]] || assets="$build/assets"
[[ -d $assets ]] || { echo "error: no PPSSPP asset tree to stage" >&2; exit 2; }
cp -a -- "$assets" "$app/assets"

# Everything a title creates must be reachable over FTP, which runs as another
# process: folders 0777, files 0666, whatever the umask gave.
find "$app" -type d -exec chmod 0777 {} +
find "$app" -type f -exec chmod 0666 {} +
chmod 0777 "$app/eboot.bin"

printf '==> %s: %s (eboot.bin %s bytes, %s asset files)\n' \
	"$title_id" "$app" "$(stat -c %s "$app/eboot.bin")" \
	"$(find "$app/assets" -type f | wc -l)"

# A second copy of the finished title, for when the build tree is not where the
# title is wanted - building on a Linux filesystem from a working copy on another
# one, which is what WSL is. Set PSP5_DIST_MIRROR to the dist/ to keep in step.
#
# Mirrored after the title folder is complete and never before, so a mirror is
# never a half-written title somebody might deploy. The eboot is replaced through a
# temporary name for the same reason.
if [[ -n ${PSP5_DIST_MIRROR:-} ]]; then
	mirror="$PSP5_DIST_MIRROR/$title_id"
	mkdir -p "$mirror"
	# Assets are ~190 files that change only when PPSSPP's tree does; copy them
	# only when the count or the total size differs, so an ordinary relink moves
	# the eboot alone.
	if [[ $(find "$mirror/assets" -type f 2>/dev/null | wc -l) != $(find "$app/assets" -type f | wc -l) ]] ||
		[[ $(du -sb "$mirror/assets" 2>/dev/null | cut -f1) != $(du -sb "$app/assets" | cut -f1) ]]; then
		rm -rf -- "$mirror/assets"
		cp -a -- "$app/assets" "$mirror/assets"
		mirrored_assets=" and the assets"
	fi
	mkdir -p "$mirror/sce_sys" "$mirror/sce_module"
	cp -a -- "$app/sce_sys/." "$mirror/sce_sys/"
	cp -a -- "$app/sce_module/." "$mirror/sce_module/"
	cp -a -- "$app/eboot.bin" "$mirror/.eboot.bin.new"
	mv -- "$mirror/.eboot.bin.new" "$mirror/eboot.bin"
	printf '==> mirrored the eboot%s to %s\n' "${mirrored_assets:-}" "$mirror"
fi
