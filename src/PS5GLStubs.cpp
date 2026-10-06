// psp5 - the OpenGL feature set, for a title that has no OpenGL.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The port patch turns PPSSPP_API_ANY_GL off, so none of the GL backend is built,
// but two things still reference its feature set:
//
//   Common/GPU/ShaderTranslation.cpp  the shared shader generators read the GL
//                                     feature set whenever they are asked for a
//                                     GLSL target. psp5 only ever asks for Vulkan.
//   UI/SystemInfoScreen.cpp           the OpenGL tab of the system information
//                                     screen lists the driver's extensions.
//
// PS5_RetroArch's port has the first of these as libretro/ps5_gl_stubs.cpp. The
// second is new here: a libretro core does not build PPSSPP's UI, and a standalone
// title does.
//
// Empty is the honest answer rather than a placeholder: there is no GL driver on
// the console, so the extension lists really are empty and the GLSL version really
// is none. The system information screen shows an empty OpenGL tab, which is
// correct.

#include "ppsspp_config.h"

#include <string>

#include "Common/GPU/OpenGL/GLFeatures.h"

GLExtensions gl_extensions;

std::string g_all_gl_extensions;
std::string g_all_egl_extensions;

int GLExtensions::GLSLVersion() {
	return 0;
}

bool GLExtensions::VersionGEThan(int major, int minor, int sub) {
	// Asked by the graphics settings screen, which gates a few GL-only options on
	// the driver's version. There is no GL driver, so no version is ever reached
	// and those options stay hidden.
	return false;
}
