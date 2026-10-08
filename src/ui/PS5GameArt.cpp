// psp5 - the artwork a PSP game carries inside it.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PPSSPP's own game list shows a title's ICON0.PNG, and it gets there by opening
// the image and reading the file out of it. psp5 links all of PPSSPP, so this
// does the same with the same code - a block device and ISO9660 for an ISO or
// CSO, the container reader for a PBP - rather than carrying its own ISO9660.
//
// This is deliberately not GameInfoCache. That cache is asynchronous, keyed on
// PPSSPP's own Path handling, and wants a draw context to make its textures;
// psp5 needs the pixels once, synchronously, before PPSSPP's renderer exists.
// What is left when all of that is removed is the twenty lines below.

#include "ui/PS5GameArt.h"

#include <cstdlib>
#include <memory>

#include "Common/Data/Format/PNGLoad.h"
#include "Common/File/Path.h"
#include "Core/ELF/PBPReader.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/FileSystems/BlockDevices.h"
#include "Core/FileSystems/ISOFileSystem.h"
#include "Core/Loaders.h"

#include "PS5Log.h"

namespace psp5 {

// PNG bytes to RGBA. pngLoadPtr allocates with malloc and leaves the caller to
// free it, so the copy into the vector is also what bounds the lifetime.
bool DecodePng(const std::string &bytes, Artwork *out) {
	if (bytes.size() < 8) {
		return false;
	}
	int width = 0;
	int height = 0;
	unsigned char *pixels = nullptr;
	if (pngLoadPtr((const unsigned char *)bytes.data(), bytes.size(), &width, &height, &pixels) !=
	        1 ||
	    !pixels) {
		return false;
	}
	if (width > 0 && height > 0) {
		out->width = width;
		out->height = height;
		out->rgba.assign(pixels, pixels + (std::size_t)width * height * 4);
	}
	free(pixels);
	return out->valid();
}

namespace {

bool ReadFromFileSystem(IFileSystem *fs, const std::string &name, std::string *contents) {
	const PSPFileInfo info = fs->GetFileInfo(name);
	if (!info.exists || info.size == 0) {
		return false;
	}
	const int handle = fs->OpenFile(name, FILEACCESS_READ);
	if (handle < 0) {
		return false;
	}
	contents->resize((std::size_t)info.size);
	const size_t read = fs->ReadFile(handle, (u8 *)contents->data(), info.size);
	fs->CloseFile(handle);
	if (read != (size_t)info.size) {
		contents->clear();
		return false;
	}
	return true;
}

void TakeTitle(const std::string &sfo, GameArt *out) {
	if (sfo.empty()) {
		return;
	}
	ParamSFOData param;
	if (!param.ReadSFO((const u8 *)sfo.data(), sfo.size())) {
		return;
	}
	out->title = param.GetValueString("TITLE");
	// What PPSSPP names a game's cheat file and its save folder after.
	out->discId = param.GetValueString("DISC_ID");
}

bool LoadFromIso(FileLoader *loader, const char *root, GameArt *out) {
	std::string error;
	std::shared_ptr<BlockDevice> device(ConstructBlockDevice(loader, &error));
	if (!device) {
		psp5::Trace("art: no block device: %s", error.c_str());
		return false;
	}
	SequentialHandleAllocator handles;
	ISOFileSystem umd(&handles, device);

	const std::string prefix = root;
	std::string bytes;
	if (ReadFromFileSystem(&umd, prefix + "PARAM.SFO", &bytes)) {
		TakeTitle(bytes, out);
	}
	if (ReadFromFileSystem(&umd, prefix + "ICON0.PNG", &bytes)) {
		DecodePng(bytes, &out->icon);
	}
	if (ReadFromFileSystem(&umd, prefix + "PIC1.PNG", &bytes)) {
		DecodePng(bytes, &out->background);
	}
	ReadFromFileSystem(&umd, prefix + "SND0.AT3", &out->sound);
	return true;
}

bool LoadFromPbp(FileLoader *loader, GameArt *out) {
	PBPReader pbp(loader);
	if (!pbp.IsValid()) {
		return false;
	}
	std::vector<u8> sfo;
	if (pbp.GetSubFile(PBP_PARAM_SFO, &sfo) && !sfo.empty()) {
		TakeTitle(std::string((const char *)sfo.data(), sfo.size()), out);
	}
	std::string bytes;
	if (pbp.GetSubFileSize(PBP_ICON0_PNG) > 0) {
		pbp.GetSubFileAsString(PBP_ICON0_PNG, &bytes);
		DecodePng(bytes, &out->icon);
	}
	if (pbp.GetSubFileSize(PBP_PIC1_PNG) > 0) {
		pbp.GetSubFileAsString(PBP_PIC1_PNG, &bytes);
		DecodePng(bytes, &out->background);
	}
	if (pbp.GetSubFileSize(PBP_SND0_AT3) > 0) {
		pbp.GetSubFileAsString(PBP_SND0_AT3, &out->sound);
	}
	return true;
}

}  // namespace

bool LoadGameArt(const std::string &path, GameArt *out) {
	std::unique_ptr<FileLoader> loader(ConstructFileLoader(Path(path)));
	if (!loader || !loader->Exists()) {
		return false;
	}

	std::string error;
	const IdentifiedFileType type = Identify_File(loader.get(), &error);
	switch (type) {
		case IdentifiedFileType::PSP_ISO:
		case IdentifiedFileType::PSP_ISO_NP:
			return LoadFromIso(loader.get(), "/PSP_GAME/", out);
		case IdentifiedFileType::PSP_UMD_VIDEO_ISO:
			return LoadFromIso(loader.get(), "/UMD_VIDEO/", out);
		case IdentifiedFileType::PSP_PBP:
		case IdentifiedFileType::PSP_PS1_PBP:
			return LoadFromPbp(loader.get(), out);
		default:
			// A homebrew ISO with no PSP_GAME still identifies as an ISO of some
			// kind; one last try at the root, where a few of them put their files.
			if (type == IdentifiedFileType::UNKNOWN_ISO) {
				return LoadFromIso(loader.get(), "/", out);
			}
			psp5::Trace("art: %s is not a PSP title psp5 reads (type %d)", path.c_str(), (int)type);
			return false;
	}
}

}  // namespace psp5
