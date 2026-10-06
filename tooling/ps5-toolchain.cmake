# Cross-compile PPSSPP for the PS5 (x86_64-sie-ps5) with the payload SDK fork.
#
# This file configures the pinned PPSSPP tree directly as the top-level project:
# upstream resolves module paths and source lists through ${CMAKE_SOURCE_DIR}, so a
# wrapper project would break them. Everything psp5 adds to that build lives here
# and in patches/ppsspp/.
#
# Unlike the libretro core build this is derived from, psp5 is a standalone title:
# CMake produces static archives only, and tools/link-title.sh performs the final
# link into an executable payload ELF. So there is no shared-object link contract
# here - CMAKE_SHARED_LINKER_FLAGS is deliberately not set.
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-clang")
set(CMAKE_CXX_COMPILER "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-clang++")
set(CMAKE_AR           "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-ar")
set(CMAKE_RANLIB       "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-ranlib")

# zlib comes from PS5_Vulkan, not from PPSSPP's own ext/zlib.
#
# RADV's archive is whole-archived into the title and carries the zlib it was built
# with (a meson subproject), so a second copy from PPSSPP's bundle is a duplicate
# of every deflate* and inflate* symbol at link time. Pointing find_package(ZLIB)
# at the very library RADV used means PPSSPP does not build its own, and its
# references resolve against RADV's objects.
set(PSP5_FIND_ROOTS "$ENV{PS5_PAYLOAD_SDK}")
if(DEFINED ENV{PS5_ZLIB})
	list(APPEND PSP5_FIND_ROOTS "$ENV{PS5_ZLIB}")
	set(ZLIB_ROOT "$ENV{PS5_ZLIB}" CACHE PATH "" FORCE)
endif()

set(CMAKE_FIND_ROOT_PATH ${PSP5_FIND_ROOTS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Read by the standalone patch's guards in the pinned tree's CMakeLists.
set(PPSSPP_PS5 ON CACHE BOOL "" FORCE)

# No window system in the sysroot. The title owns presentation through
# VK_KHR_display (src/PS5VulkanContext.cpp), so the tree must not look for X11.
set(USING_X11_VULKAN OFF CACHE BOOL "" FORCE)

# CMake's compiler probes must link something. Nothing built here runs on the host,
# so the probe is given a bare entry point and the SDK's own libraries rather than a
# host CRT; -nodefaultlibs stops the SDK wrapper adding its own -lc -lSceNet.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -nostartfiles -nodefaultlibs -Wl,-e,0 -lkernel_web -lSceLibcInternal -lScePosixForWebKit")

# -Dstatic_assert=_Static_assert is a shim for the SDK's headers, not a preference:
# PPSSPP's C flags include -D_XOPEN_SOURCE=700, which on this FreeBSD-derived libc
# pins __ISO_C_VISIBLE to 1990, so <assert.h> never declares the C11 static_assert
# that vendored C libraries (ext/xxhash.h among them) assume. _Static_assert is a
# keyword in C11 and later, so the rename is always available. C only: C++ has
# static_assert as a keyword.
#
# -DZSTD_TRACE=0: zstd emits weak tracing hooks whenever it sees GNUC+ELF+x86-64,
# and no public SDK stub exports them.
#
# The RADV headers come in through these flags rather than -DCMAKE_C_FLAGS on the
# command line, because that would replace these INIT flags rather than add to them
# and take the static_assert shim with it.
set(PSP5_EXTRA_INCLUDES "")
if(DEFINED ENV{PS5_RADV})
	set(PSP5_EXTRA_INCLUDES "-I$ENV{PS5_RADV}/include")
endif()

set(CMAKE_C_FLAGS_INIT   "-O2 -fPIC -w -Dstatic_assert=_Static_assert -DZSTD_TRACE=0 ${PSP5_EXTRA_INCLUDES}")
set(CMAKE_CXX_FLAGS_INIT "-O2 -fPIC -w -DZSTD_TRACE=0 ${PSP5_EXTRA_INCLUDES}")
