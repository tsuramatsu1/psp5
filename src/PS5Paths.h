// psp5 - where the title keeps its files.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace PS5Paths {

// /app0 is the title's folder while it runs, and /data/homebrew/<TITLE_ID> over
// FTP. Everything psp5 reads and writes lives under it; there is no getcwd on the
// console and no $HOME, so every path is built from here explicitly.
extern const char *const kRoot;

// Created at start-up, 0777, so the console's FTP server (a separate process) can
// read logs and configs and write saves and content into them.
std::string Assets();      // /app0/assets   - PPSSPP's own asset tree, shipped
std::string Memstick();    // /app0/memstick - the emulated memory stick: PSP/, saves
std::string Config();      // /app0/config
std::string Cache();       // /app0/cache
std::string Log();         // /app0/psp5.log

// Creates the writable folders and repairs the permissions of any that an older
// build left too strict. Returns false only if the memory stick cannot be made,
// which is fatal: PPSSPP has nowhere to put a save.
bool Prepare();

// Reports what the filesystem actually does for the title's own paths: stat,
// fopen and opendir, each with errno. For bring-up, when a read fails and the
// reason would otherwise have to be inferred from where PPSSPP gave up.
void Probe();

// The PSP folders on the drives: /mnt/usb0/PSP .. /mnt/usb7/PSP, then
// /mnt/ext0/PSP and /mnt/ext1/PSP, for every one that is there.
//
// A drive holds the same tree the title's own memory stick does, so a game
// goes in <drive>/PSP/GAME and a texture pack in
// <drive>/PSP/TEXTURES/<DISC_ID>/. Empty when the title is sandboxed: listing
// a folder is what the sandbox withholds, and a drive that cannot be listed
// has nothing psp5 can find (PS5Unlock.h).
//
// Read afresh rather than cached: a drive can be plugged in while the home
// screen is up, and the shelf rescans.
std::vector<std::string> Drives();

// <drive>/PSP/TEXTURES for the drive that has a pack for this disc id, or
// empty when none has. PPSSPP appends the disc id itself.
std::string TexturesRoot(const std::string &discId);

}  // namespace PS5Paths
