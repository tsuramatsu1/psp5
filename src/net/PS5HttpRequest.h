// psp5 - HTTPS on the console, for PPSSPP's request manager.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string_view>

#include "Common/File/Path.h"
#include "Common/Net/HTTPRequest.h"

namespace psp5 {

// PPSSPP's own HTTPS is naett, whose four backends are WinHTTP, NSURLSession,
// libcurl and Java - none of which exist here, so PPSSPP's CMakeLists sets
// HTTPS_NOT_AVAILABLE for this platform and every https request returns a null
// handle and is never sent. That is why RetroAchievements did nothing: its API
// is https only.
//
// This is the console's answer: PacBrew's libcurl and OpenSSL, which are built
// for this target already and proven in native titles, driven directly rather
// than through naett - naett is a git submodule of PPSSPP, so a patch cannot
// reach it, and its backend choice is made by platform macros this console does
// not set.
//
// Returns null if the transport could not start, which the caller already
// handles: it is what every https request returned before.
std::shared_ptr<http::Request> CreateHttpsRequest(http::RequestMethod method,
                                                  std::string_view url,
                                                  std::string_view postData,
                                                  std::string_view postMime, const Path &outfile,
                                                  http::RequestFlags flags, std::string_view name);

}  // namespace psp5
