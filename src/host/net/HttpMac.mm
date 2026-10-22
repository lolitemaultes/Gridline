// Gridline - J-89: HTTPS requests for the update check on macOS (NSURLSession;
// see Http.h). Ephemeral sessions: no cookies, no cache, nothing stored.
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <string>
#include "Http.h"

namespace gl::http {

namespace {

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

std::string describe(NSError* e, const char* fallback) {
  const char* t = e.localizedDescription.UTF8String;
  return t ? std::string(t) : std::string(fallback);
}

bool offlineCode(NSError* e) {
  if (![e.domain isEqualToString:NSURLErrorDomain]) return false;
  switch (e.code) {
    case NSURLErrorNotConnectedToInternet:
    case NSURLErrorCannotFindHost:
    case NSURLErrorCannotConnectToHost:
    case NSURLErrorTimedOut:
    case NSURLErrorNetworkConnectionLost:
    case NSURLErrorDNSLookupFailed:
    case NSURLErrorInternationalRoamingOff:
    case NSURLErrorDataNotAllowed:
    case NSURLErrorSecureConnectionFailed: return true;
    default: return false;
  }
}

NSURLSession* makeSession(double requestTimeout, double resourceTimeout, bool api) {
  NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
  cfg.timeoutIntervalForRequest = requestTimeout;
  cfg.timeoutIntervalForResource = resourceTimeout;
  cfg.URLCache = nil;
  cfg.HTTPCookieStorage = nil;
  cfg.HTTPShouldSetCookies = NO;
  cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  NSMutableDictionary* h = [NSMutableDictionary dictionaryWithObject:ns(update::userAgent()) forKey:@"User-Agent"];
  if (api) {
    h[@"Accept"] = @"application/vnd.github+json";
    h[@"X-GitHub-Api-Version"] = @"2022-11-28";
  }
  cfg.HTTPAdditionalHeaders = h;
  return [NSURLSession sessionWithConfiguration:cfg];
}

}  // namespace

update::HttpResult get(const std::string& url, int timeoutSeconds) {
  update::HttpResult r;
  if (!detail::httpsOnly(url)) {
    r.error = "not an https:// address";
    return r;
  }
  @autoreleasepool {
    NSURL* u = [NSURL URLWithString:ns(url)];
    if (!u) {
      r.error = "invalid address";
      return r;
    }
    NSURLSession* session = makeSession(timeoutSeconds, timeoutSeconds, true);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    update::HttpResult* out = &r;
    NSURLSessionDataTask* task = [session dataTaskWithURL:u
                                        completionHandler:^(NSData* data, NSURLResponse* resp, NSError* error) {
                                          if (error) {
                                            out->offline = offlineCode(error);
                                            out->error = out->offline ? "no connection" : describe(error, "request failed");
                                          } else if (![resp.URL.scheme isEqualToString:@"https"]) {
                                            out->error = "redirected away from HTTPS";
                                          } else {
                                            if ([resp isKindOfClass:[NSHTTPURLResponse class]])
                                              out->status = static_cast<int>(static_cast<NSHTTPURLResponse*>(resp).statusCode);
                                            if (data.length) out->body.assign(static_cast<const char*>(data.bytes), data.length);
                                          }
                                          dispatch_semaphore_signal(done);
                                        }];
    [task resume];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(timeoutSeconds + 5) * NSEC_PER_SEC)) != 0) {
      [task cancel];  // the handler still runs (cancelled): wait for it, it writes into r
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
      r.offline = true;
      r.error = "no connection";
      r.status = 0;
    }
    dispatch_release(done);
    [session finishTasksAndInvalidate];
  }
  return r;
}

bool download(const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
              const std::atomic<bool>* cancel, std::string* err) {
  if (!detail::httpsOnly(url)) {
    if (err) *err = "not an https:// address";
    return false;
  }
  bool ok = false;
  std::string error;
  @autoreleasepool {
    NSURL* u = [NSURL URLWithString:ns(url)];
    if (!u) {
      if (err) *err = "invalid address";
      return false;
    }
    NSURLSession* session = makeSession(60, 6 * 3600, false);  // a minute without data fails
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    bool* okp = &ok;
    std::string* errp = &error;
    const std::string dest = path;
    NSURLSessionDownloadTask* task =
        [session downloadTaskWithURL:u
                   completionHandler:^(NSURL* location, NSURLResponse* resp, NSError* e) {
                     const int status = [resp isKindOfClass:[NSHTTPURLResponse class]]
                                            ? static_cast<int>(static_cast<NSHTTPURLResponse*>(resp).statusCode)
                                            : 0;
                     if (e) {
                       *errp = offlineCode(e) ? "the connection was lost" : describe(e, "download failed");
                     } else if (![resp.URL.scheme isEqualToString:@"https"]) {
                       *errp = "redirected away from HTTPS";
                     } else if (status != 200) {
                       *errp = "HTTP " + std::to_string(status);
                     } else if (location) {
                       NSFileManager* fm = [NSFileManager defaultManager];
                       NSURL* to = [NSURL fileURLWithPath:ns(dest)];
                       [fm removeItemAtURL:to error:nil];
                       NSError* me = nil;
                       if ([fm moveItemAtURL:location toURL:to error:&me]) *okp = true;
                       else *errp = "cannot write " + dest;
                     }
                     dispatch_semaphore_signal(done);
                   }];
    [task resume];
    bool cancelled = false;
    while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
      if (progress) progress(task.countOfBytesReceived, task.countOfBytesExpectedToReceive > 0 ? task.countOfBytesExpectedToReceive : -1);
      if (!cancelled && cancel && cancel->load()) {
        cancelled = true;
        [task cancel];
      }
    }
    if (progress && ok) progress(task.countOfBytesReceived, task.countOfBytesReceived);
    if (cancelled) ok = false, error = "cancelled";
    dispatch_release(done);
    [session finishTasksAndInvalidate];
  }
  if (!ok && err) *err = error.empty() ? "download failed" : error;
  return ok;
}

std::string downloadsFolder() {
  @autoreleasepool {
    NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES);
    NSString* d = dirs.count ? dirs[0] : [NSHomeDirectory() stringByAppendingPathComponent:@"Downloads"];
    [[NSFileManager defaultManager] createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
    return std::string(d.fileSystemRepresentation);
  }
}

bool reveal(const std::string& path) {
  @autoreleasepool {
    NSURL* u = [NSURL fileURLWithPath:ns(path)];
    if (!u) return false;
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ u ]];
    return true;
  }
}

}  // namespace gl::http
