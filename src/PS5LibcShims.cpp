// psp5 - the libc entry points the console does not provide.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two kinds of gap are filled here, and they fail differently.
//
// 1. Declared by the SDK but not exported, so the link leaves them undefined:
//
//      swab     ext/libpng17/pngtrans.c, for 16-bit row transforms
//      tmpfile  ext/lua/liolib.c, io.tmpfile()
//      tmpnam   ext/lua/loslib.c, os.tmpname()
//
// 2. Listed only by the stubs for libkernel_sys and libScePosixForWebKit, which a
//    title does not load. These *link* - the stub satisfies the reference - but are
//    null at run time, so the first call jumps to address 0. tools/link-title.sh
//    refuses a title that still imports one, which is how this list was found:
//
//      fork                               ext/imgui
//      link, symlink, readlink, pathconf  ext/armips, the MIPS assembler
//
//    PS5_RetroArch's PPSSPP core met the same class of problem on its first console
//    run ("unresolved native runtime import: gai_strerror") and solves it the same
//    way: define them where the reference is, rather than teaching the title's
//    import table about a module it never loads.
//
// Only swab is a real implementation. The rest refuse in the way their callers
// already handle, and none of them is on a path psp5 needs: there is no process to
// fork, no symbolic links in a title's sandbox, no temporary directory, no
// terminal, and no resolver.

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <sys/types.h>

extern "C" {

void swab(const void *from, void *to, ssize_t count) {
	if (count <= 0) {
		return;
	}
	const auto *source = static_cast<const unsigned char *>(from);
	auto *destination = static_cast<unsigned char *>(to);
	// POSIX: swap every adjacent pair; a trailing odd byte is left alone.
	for (ssize_t index = 0; index + 1 < count; index += 2) {
		destination[index] = source[index + 1];
		destination[index + 1] = source[index];
	}
	if (count & 1) {
		destination[count - 1] = source[count - 1];
	}
}

// There is no temporary directory on the console: a title writes under /app0, and
// psp5 hands PPSSPP its own cache folder. Lua reports the failure to the script.
FILE *tmpfile(void) { return nullptr; }
char *tmpnam(char *) { return nullptr; }

// A title is one process and cannot start another this way. (A second executable
// is launched with sceSystemServiceLoadExec, which replaces the process.)
pid_t fork(void) {
	errno = ENOSYS;
	return -1;
}

// The sandbox has no symbolic or hard links. readlink reports "not a link", which
// is what a caller walking a path expects for an ordinary file.
int link(const char *, const char *) {
	errno = ENOSYS;
	return -1;
}

int symlink(const char *, const char *) {
	errno = ENOSYS;
	return -1;
}

ssize_t readlink(const char *, char *, size_t) {
	errno = EINVAL;
	return -1;
}

long pathconf(const char *, int) {
	// -1 with errno unchanged is POSIX for "no limit, and no error": callers treat
	// it as "use your own default".
	errno = 0;
	return -1;
}

// isatty, mkstemp and gai_strerror were here too, and are now src/net/console_curl.c's:
// libcurl needs working versions of all three, not the refusals these were, and
// that file is where the console's networking quirks are kept. Two definitions
// of each is a duplicate symbol at link time, so these gave way.

}  // extern "C"
