// psp5 - HTTPS on the console, for PPSSPP's request manager.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One libcurl handle per request on a thread of its own, which is how naett's
// own libcurl backend works and what http::Request's Start/Join/Done contract
// expects.
//
// Everything the console needs beyond ordinary curl is in console_curl.c, which
// is vendored beside this from ps5-native-app-boilerplate: the certificate list
// the console keeps, the non-blocking socket option it uses instead of the
// usual one, and an fcntl wrapper, because a sandboxed title's libc refuses
// fcntl on sockets and curl fails every connect without it.

#include "net/PS5HttpRequest.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <curl/curl.h>

#include "Common/Buffer.h"
#include "Common/File/FileUtil.h"

#include "PS5Log.h"
#include "net/console_curl.h"

namespace psp5 {
namespace {

// curl_global_init is not thread safe and must happen before any handle.
std::once_flag g_started;
CURLcode g_startResult = CURLE_FAILED_INIT;

void StartCurl() {
	std::call_once(g_started, [] {
		g_startResult = curl_global_init(CURL_GLOBAL_DEFAULT);
		psp5::Trace("net: curl %s", g_startResult == CURLE_OK ? curl_version() : "failed to start");
	});
}

class PS5HttpsRequest : public http::Request {
public:
	PS5HttpsRequest(http::RequestMethod method, std::string_view url, std::string_view postData,
	                std::string_view postMime, const Path &outfile, http::RequestFlags flags,
	                std::string_view name)
	    : Request(method, url, name, &cancelled_, flags),
	      postData_(postData),
	      postMime_(postMime) {
		outfile_ = outfile;
	}

	~PS5HttpsRequest() override {
		Join();
	}

	void Start() override {
		if (thread_.joinable()) {
			return;
		}
		thread_ = std::thread([this] { Run(); });
	}

	void Join() override {
		if (thread_.joinable()) {
			thread_.join();
		}
	}

	bool Done() override { return done_.load(std::memory_order_acquire); }
	bool Failed() const override { return failed_.load(std::memory_order_acquire); }

private:
	static size_t OnData(char *data, size_t size, size_t count, void *user) {
		auto *self = static_cast<PS5HttpsRequest *>(user);
		const size_t bytes = size * count;
		if (self->IsCancelled()) {
			return 0;  // anything short of the whole block stops the transfer
		}
		self->buffer_.Append(std::string_view(data, bytes));
		return bytes;
	}

	void Finish(bool failed) {
		progress_.Update(0, 0, true);
		failed_.store(failed, std::memory_order_release);
		done_.store(true, std::memory_order_release);
	}

	void Run() {
		StartCurl();
		if (g_startResult != CURLE_OK) {
			Finish(true);
			return;
		}

		CURL *easy = curl_easy_init();
		if (!easy) {
			Finish(true);
			return;
		}
		// The console's certificate list, its own socket option, and no signals.
		console_curl_setup(easy);

		curl_easy_setopt(easy, CURLOPT_URL, url_.c_str());
		curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
		curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
		curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &PS5HttpsRequest::OnData);
		curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
		if (!userAgent_.empty()) {
			curl_easy_setopt(easy, CURLOPT_USERAGENT, userAgent_.c_str());
		}

		curl_slist *headers = nullptr;
		if (acceptMime_) {
			headers = curl_slist_append(headers, (std::string("Accept: ") + acceptMime_).c_str());
		}
		if (method_ == http::RequestMethod::POST) {
			curl_easy_setopt(easy, CURLOPT_POST, 1L);
			curl_easy_setopt(easy, CURLOPT_POSTFIELDS, postData_.data());
			curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)postData_.size());
			if (!postMime_.empty()) {
				headers =
				    curl_slist_append(headers, (std::string("Content-Type: ") + postMime_).c_str());
			}
		}
		if (headers) {
			curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
		}

		const CURLcode code = curl_easy_perform(easy);
		long status = 0;
		curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
		resultCode_ = (int)status;

		if (code != CURLE_OK) {
			psp5::Trace("net: %s failed: %s", url_.c_str(), curl_easy_strerror(code));
		} else if (!outfile_.empty()) {
			// A download to a file still arrives in the buffer first; it is one
			// write at the end rather than a file handle held open across a
			// transfer that may be cancelled half way.
			std::string body;
			buffer_.PeekAll(&body);
			if (!File::WriteDataToFile(false, body.data(), body.size(), outfile_)) {
				psp5::Trace("net: cannot write %s", outfile_.c_str());
			}
		}

		if (headers) {
			curl_slist_free_all(headers);
		}
		curl_easy_cleanup(easy);
		Finish(code != CURLE_OK || status >= 400);
	}

	std::string postData_;
	std::string postMime_;
	std::thread thread_;
	std::atomic<bool> done_{false};
	std::atomic<bool> failed_{false};
	bool cancelled_ = false;
};

}  // namespace

std::shared_ptr<http::Request> CreateHttpsRequest(http::RequestMethod method, std::string_view url,
                                                  std::string_view postData,
                                                  std::string_view postMime, const Path &outfile,
                                                  http::RequestFlags flags,
                                                  std::string_view name) {
	return std::make_shared<PS5HttpsRequest>(method, url, postData, postMime, outfile, flags, name);
}

}  // namespace psp5
