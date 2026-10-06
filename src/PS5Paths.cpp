// psp5 - where the title keeps its files.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PS5Paths.h"

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
	return true;
}

}  // namespace PS5Paths
