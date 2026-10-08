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
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <curl/curl.h>

#include "Common/Buffer.h"
#include "Common/File/FileUtil.h"

#include "PS5Log.h"
#include "net/console_curl.h"

// The SDK ships no headers for these. sceNetInit is declared as the payload
// SDK's own http2_get sample declares it; the resolver calls are as
// console_curl.c declares them, which is where they are already proven.
extern "C" {
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char *name, struct in_addr *address, int timeout,
                            int retries, int flags);
int sceNetResolverDestroy(int resolver);
}

namespace psp5 {
namespace {

// curl_global_init is not thread safe and must happen before any handle.
std::once_flag g_started;
CURLcode g_startResult = CURLE_FAILED_INIT;

// Where a name lookup actually fails. curl reports every one of them as
// "Could not resolve hostname", which covers the network never having been
// started, the pool not being made, and the server answering no - and those
// want different fixes. This says which, once, in the title's own log.
void ProbeResolver() {
	const int pool = sceNetPoolCreate("psp5_dns_probe", 16 * 1024, 0);
	if (pool < 0) {
		psp5::Trace("net: resolver pool failed (0x%08x) - the network is not up", pool);
		return;
	}
	const int resolver = sceNetResolverCreate("psp5_dns_probe", pool, 0);
	if (resolver < 0) {
		psp5::Trace("net: resolver failed (0x%08x)", resolver);
		sceNetPoolDestroy(pool);
		return;
	}
	struct in_addr address {};
	const int found = sceNetResolverStartNtoa(resolver, "retroachievements.org", &address, 5000000,
	                                          2, 0);
	if (found < 0) {
		psp5::Trace("net: retroachievements.org did not resolve (0x%08x) - the console's DNS "
		            "answered no",
		            found);
	} else {
		const unsigned char *o = (const unsigned char *)&address.s_addr;
		psp5::Trace("net: retroachievements.org is %u.%u.%u.%u", o[0], o[1], o[2], o[3]);
	}
	sceNetResolverDestroy(resolver);
	sceNetPoolDestroy(pool);
}

// The host of an http(s) URL, without the scheme, the port or the path.
std::string HostOf(const std::string &url) {
	const std::size_t scheme = url.find("://");
	const std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
	const std::size_t end = url.find_first_of("/:?#", start);
	return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Resolved hosts, so a run of requests to one server asks the console once.
std::mutex g_addressLock;
std::map<std::string, std::string> g_addresses;

// The console's resolver, which works - unlike curl's own, which reports
// "Could not resolve hostname" here even for a name the console resolves
// perfectly well a line earlier in the log. curl runs its resolver on a thread
// it starts itself, and this link wraps pthread_create for RADV's sake, which
// is the likeliest reason it never gets an answer. Rather than unpick that,
// psp5 looks the name up itself and hands curl the address.
std::string Resolve(const std::string &host) {
	{
		std::lock_guard<std::mutex> lock(g_addressLock);
		const auto found = g_addresses.find(host);
		if (found != g_addresses.end()) {
			return found->second;
		}
	}

	std::string text;
	const int pool = sceNetPoolCreate("psp5_dns", 16 * 1024, 0);
	if (pool >= 0) {
		const int resolver = sceNetResolverCreate("psp5_dns", pool, 0);
		if (resolver >= 0) {
			struct in_addr address {};
			if (sceNetResolverStartNtoa(resolver, host.c_str(), &address, 5000000, 2, 0) >= 0) {
				const unsigned char *o = (const unsigned char *)&address.s_addr;
				char dotted[32];
				std::snprintf(dotted, sizeof(dotted), "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
				text = dotted;
			}
			sceNetResolverDestroy(resolver);
		}
		sceNetPoolDestroy(pool);
	}
	if (!text.empty()) {
		std::lock_guard<std::mutex> lock(g_addressLock);
		g_addresses[host] = text;
	}
	return text;
}

void StartCurl() {
	std::call_once(g_started, [] {
		// Before anything that resolves a name. The console's resolver is
		// reached through sceNetPoolCreate (console_curl.c), and that fails
		// until the network has been brought up - which showed up only as
		// curl's "Could not resolve hostname" on every request, with no sign
		// that the lookup had never had a pool to work in.
		const int net = sceNetInit();
		if (net < 0) {
			psp5::Trace("net: sceNetInit failed (0x%08x); names will not resolve", net);
		}
		g_startResult = curl_global_init(CURL_GLOBAL_DEFAULT);
		psp5::Trace("net: curl %s", g_startResult == CURLE_OK ? curl_version() : "failed to start");
		ProbeResolver();
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

		// The address, found by the console rather than by curl.
		curl_slist *resolved = nullptr;
		const std::string host = HostOf(url_);
		const std::string address = host.empty() ? std::string() : Resolve(host);
		if (!address.empty()) {
			for (const char *port : {"443", "80"}) {
				resolved = curl_slist_append(
				    resolved, (host + ":" + port + ":" + address).c_str());
			}
			curl_easy_setopt(easy, CURLOPT_RESOLVE, resolved);
		} else if (!host.empty()) {
			psp5::Trace("net: %s did not resolve", host.c_str());
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
		if (resolved) {
			curl_slist_free_all(resolved);
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
