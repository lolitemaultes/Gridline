// Gridline - J-89: the few HTTPS requests the update check makes, with the
// system's own network stack (no extra library):
//   macOS          NSURLSession (HttpMac.mm)
//   Windows        WinHTTP (Http.cpp)
//   Linux / BSD    the curl or wget program (Http.cpp), whichever is installed
// HTTPS only (redirects too), with timeouts and the User-Agent "Gridline/<version>";
// nothing else identifies the computer or the user. Blocking: call from a worker thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include "../../core/Update.h"

namespace gl::http {

// GET `url` (GitHub API headers added). `timeoutSeconds` bounds the whole request.
update::HttpResult get(const std::string& url, int timeoutSeconds = 15);
// Writes `url` to `path`. progress(received, total) is called from this thread (total < 0:
// unknown); `cancel` is polled. A download that stalls for a minute fails.
bool download(const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
              const std::atomic<bool>* cancel, std::string* err);
update::Fetcher fetcher();
update::Downloader downloader();

// The user's Downloads folder (created when missing; the home folder when there is none).
std::string downloadsFolder();
// Shows `path` selected in Finder / Explorer, or opens its folder in the file manager.
bool reveal(const std::string& path);

// `gridline --check-updates` / `gridline-app --check-updates`: one check (settings' beta
// choice, or `beta`), the result on stdout. Exit code: 0 up to date / no release /
// turned off, 1 failed, 2 an update is available.
int checkCommand(bool beta);

namespace detail {  // shared by the platform files
bool httpsOnly(const std::string& url);
}

}  // namespace gl::http
