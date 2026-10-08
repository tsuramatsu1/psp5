// psp5 - where the title keeps its files.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PS5Paths.h"

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

#include "platform/platform.h"

namespace PS5Paths {

const char *const kRoot = "/app0";

std::string Assets() { return std::string(kRoot) + "/assets"; }
std::string Memstick() { return std::string(kRoot) + "/memstick"; }
std::string Config() { return std::string(kRoot) + "/config"; }
std::string Cache() { return std::string(kRoot) + "/cache"; }
std::string Log() { return std::string(kRoot) + "/psp5.log"; }

namespace {

// Everything a title creates has to be reachable over FTP, and the FTP server runs
// as another process: folders 0777, whatever the umask gave. chmod is applied even
// when the folder already existed, to repair modes left by an older build.
bool EnsureDir(const std::string &path) {
	if (mkdir(path.c_str(), 0777) != 0) {
		struct stat st {};
		if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
			say("paths: cannot create %s", path.c_str());
			return false;
		}
	}
	chmod(path.c_str(), 0777);
	return true;
}

}  // namespace

void Probe() {
	// Asset reads failed on the console while writes under /app0 worked, so this
	// reports what each filesystem call actually does rather than leaving it to be
	// inferred from where PPSSPP gave up. Cheap, and it runs once.
	auto report = [](const char *what, const std::string &path) {
		struct stat st {};
		if (stat(path.c_str(), &st) != 0) {
			say("probe: %-10s %-44s stat failed, errno %d", what, path.c_str(), errno);
			return;
		}
		say("probe: %-10s %-44s %s, %lld bytes, mode %o", what, path.c_str(),
		    S_ISDIR(st.st_mode) ? "dir" : "file", (long long)st.st_size,
		    (unsigned)(st.st_mode & 07777));
	};

	report("root", kRoot);
	report("assets", Assets());
	report("memstick", Memstick());

	const std::string atlas = Assets() + "/font_atlas.zim";
	report("atlas", atlas);

	// The read itself, which is what PPSSPP could not do.
	if (FILE *fh = fopen(atlas.c_str(), "rb")) {
		unsigned char head[4] = {};
		const size_t got = fread(head, 1, sizeof(head), fh);
		say("probe: fopen ok, read %zu bytes: %02x %02x %02x %02x", got, head[0], head[1],
		    head[2], head[3]);
		fclose(fh);
	} else {
		say("probe: fopen failed on %s, errno %d", atlas.c_str(), errno);
	}

	// The clock. PPSSPP times frames and paces emulation with
	// clock_gettime(CLOCK_MONOTONIC), and its first console run reported elapsed
	// times near -1.79e9 seconds - the size of a Unix epoch, which is what a
	// monotonic clock must never return. So check what each clock actually gives,
	// and whether two reads of the same one advance sensibly.
	auto clock_report = [](const char *name, clockid_t id) {
		struct timespec a {}, b {};
		const int rc_a = clock_gettime(id, &a);
		for (volatile int spin = 0; spin < 2000000; spin++) {
		}
		const int rc_b = clock_gettime(id, &b);
		const double delta = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
		say("probe: clock %-10s rc=%d/%d  tv_sec=%lld -> %lld  delta=%.6fs", name, rc_a, rc_b,
		    (long long)a.tv_sec, (long long)b.tv_sec, delta);
	};
	clock_report("MONOTONIC", CLOCK_MONOTONIC);
	clock_report("REALTIME", CLOCK_REALTIME);

	// And the directory walk, which is how PPSSPP's VFS finds anything at all.
	if (DIR *dir = opendir(Assets().c_str())) {
		int entries = 0;
		while (readdir(dir) != nullptr) {
			entries++;
		}
		closedir(dir);
		say("probe: opendir(%s) listed %d entries", Assets().c_str(), entries);
	} else {
		say("probe: opendir failed on %s, errno %d", Assets().c_str(), errno);
	}
}

bool Prepare() {
	// The memory stick is the one psp5 cannot run without: PPSSPP puts saves, save
	// states and its own configuration under it.
	if (!EnsureDir(Memstick()) || !EnsureDir(Memstick() + "/PSP") ||
	    !EnsureDir(Memstick() + "/PSP/GAME") || !EnsureDir(Memstick() + "/PSP/SAVEDATA")) {
		return false;
	}
	// Not fatal: PPSSPP falls back to defaults without them.
	EnsureDir(Config());
	EnsureDir(Cache());
	// Cheats are read from here, one <DISC_ID>.ini per game. Made up front so
	// there is somewhere obvious to copy a cheat file to over FTP, rather than a
	// folder that only appears once a game has already looked for it.
	EnsureDir(Memstick() + "/PSP/Cheats");
	return true;
}

}  // namespace PS5Paths
