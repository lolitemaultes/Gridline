// Gridline - J-89: HTTPS requests for the update check on macOS (NSURLSession;
// see Http.h). Ephemeral sessions: no cookies, no cache, nothing stored.
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include "Http.h"
#include "../ui/ObjcClass.h"

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

}  // namespace gl::http

namespace gl::http {
namespace {

// Writes a download as it arrives, so a broken connection keeps what came (it is resumed).
struct Sink {
  std::string path;
  int64_t have = 0;
  bool plain = false;
  int status = 0;
  bool dropped = false;
  std::string error;
  FILE* file = nullptr;
  std::atomic<int64_t> received{0};
  std::atomic<int64_t> total{-1};
  dispatch_semaphore_t done = nullptr;
};

Sink* sinkOf(id self) {
  void** p = objc::pointerIvar(self, "sink");
  return p ? static_cast<Sink*>(*p) : nullptr;
}

void sinkResponse(id self, SEL, NSURLSession*, NSURLSessionDataTask*, NSURLResponse* response,
                  void (^handler)(NSURLSessionResponseDisposition)) {
  Sink& s = *sinkOf(self);
  NSHTTPURLResponse* h = [response isKindOfClass:[NSHTTPURLResponse class]] ? static_cast<NSHTTPURLResponse*>(response) : nil;
  s.status = h ? static_cast<int>(h.statusCode) : 0;
  if (![response.URL.scheme isEqualToString:@"https"] && !s.plain) {
    s.error = "redirected away from HTTPS";
    handler(NSURLSessionResponseCancel);
    return;
  }
  if (s.status == 206 || s.status == 200) {
    const bool append = s.status == 206 && s.have > 0;
    s.file = std::fopen(s.path.c_str(), append ? "ab" : "wb");
    if (!s.file) {
      s.error = "cannot write " + s.path;
      handler(NSURLSessionResponseCancel);
      return;
    }
    s.received = append ? s.have : 0;
    const int64_t expected = response.expectedContentLength;
    s.total = expected > 0 ? expected + (append ? s.have : 0) : -1;
    handler(NSURLSessionResponseAllow);
    return;
  }
  handler(NSURLSessionResponseCancel);  // an error status: the caller reports it
}

void sinkData(id self, SEL, NSURLSession*, NSURLSessionDataTask* task, NSData* data) {
  Sink& s = *sinkOf(self);
  if (!s.file) return;
  FILE* file = s.file;
  __block bool ok = true;
  [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
    if (std::fwrite(bytes, 1, range.length, file) != range.length) {
      ok = false;
      *stop = YES;
    }
  }];
  if (!ok) {
    s.error = "could not write the file (disk full?)";
    [task cancel];
    return;
  }
  s.received += static_cast<int64_t>(data.length);
}

void sinkComplete(id self, SEL, NSURLSession*, NSURLSessionTask*, NSError* e) {
  Sink& s = *sinkOf(self);
  if (s.file) {
    if (std::fflush(s.file) != 0 && s.error.empty()) s.error = "could not write the file (disk full?)";
    std::fclose(s.file);
    s.file = nullptr;
  }
  if (e && s.error.empty() && !(s.status != 200 && s.status != 206 && s.status != 0 && e.code == NSURLErrorCancelled)) {
    if (e.code == NSURLErrorTimedOut) s.error = "stalled";
    else if (e.code == NSURLErrorCancelled) s.error = "cancelled";
    else if (offlineCode(e)) s.error = "no connection";
    else s.error = describe(e, "download failed");
    if (e.code == NSURLErrorNetworkConnectionLost) s.dropped = true;
  }
  dispatch_semaphore_signal(s.done);
}

// The session's delegate, made at run time (ObjcClass.h): the plugin bundles carry this code too, and
// a compiled-in class would clash between two of them loaded in one host.
id newSinkDelegate(Sink* s) {
  static objc::RuntimeClass rc("GLUpdateSink", [NSObject class]);
  static Class c = rc.pointer("sink")
                       .conforms(@protocol(NSURLSessionDataDelegate))
                       .method(@selector(URLSession:dataTask:didReceiveResponse:completionHandler:), &sinkResponse)
                       .method(@selector(URLSession:dataTask:didReceiveData:), &sinkData)
                       .method(@selector(URLSession:task:didCompleteWithError:), &sinkComplete)
                       .done();
  if (!c) return nil;
  id d = [[c alloc] init];
  if (void** p = objc::pointerIvar(d, "sink")) *p = s;
  return d;
}

}  // namespace
}  // namespace gl::http

namespace gl::http {

update::HttpResult getConditional(const std::string& url, const std::string& etag, int timeoutSeconds) {
  update::HttpResult r;
  if (!detail::httpsOnly(url)) {
    r.error = "not an https:// address";
    return r;
  }
  const bool plain = detail::loopbackTest(url);
  @autoreleasepool {
    NSURL* u = [NSURL URLWithString:ns(url)];
    if (!u) {
      r.error = "invalid address";
      return r;
    }
    NSURLSession* session = makeSession(timeoutSeconds, timeoutSeconds, true);
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
    if (!etag.empty()) [req setValue:ns(etag) forHTTPHeaderField:@"If-None-Match"];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    update::HttpResult* out = &r;
    NSURLSessionDataTask* task = [session dataTaskWithRequest:req
                                            completionHandler:^(NSData* data, NSURLResponse* resp, NSError* error) {
                                              if (error) {
                                                out->offline = offlineCode(error);
                                                out->error = out->offline ? "no connection" : describe(error, "request failed");
                                              } else if (![resp.URL.scheme isEqualToString:@"https"] && !plain) {
                                                out->error = "redirected away from HTTPS";
                                              } else {
                                                if ([resp isKindOfClass:[NSHTTPURLResponse class]]) {
                                                  NSHTTPURLResponse* h = static_cast<NSHTTPURLResponse*>(resp);
                                                  out->status = static_cast<int>(h.statusCode);
                                                  NSString* tag = [h valueForHTTPHeaderField:@"ETag"];
                                                  if (tag.UTF8String) out->etag = tag.UTF8String;
                                                }
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

update::HttpResult get(const std::string& url, int timeoutSeconds) { return getConditional(url, std::string(), timeoutSeconds); }

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
                     } else if (![resp.URL.scheme isEqualToString:@"https"] && !detail::loopbackTest(url)) {
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

bool downloadContinue(const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
                      const std::atomic<bool>* cancel, int stallSeconds, std::string* err) {
  if (!detail::httpsOnly(url)) {
    if (err) *err = "not an https:// address";
    return false;
  }
  for (int pass = 0; pass < 2; ++pass) {
    int64_t have = 0;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      have = static_cast<int64_t>(std::ftell(f));
      std::fclose(f);
    }
    Sink state;
    Sink* sink = &state;
    sink->path = path;
    sink->have = have;
    sink->plain = detail::loopbackTest(url);
    sink->done = dispatch_semaphore_create(0);
    id delegate = newSinkDelegate(sink);
    bool ok = false, retry = false;
    std::string error;
    @autoreleasepool {
      NSURL* u = [NSURL URLWithString:ns(url)];
      if (!u || !delegate) {
        if (err) *err = u ? "cannot start the download" : "invalid address";
        dispatch_release(sink->done);
        [delegate release];
        return false;
      }
      NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
      cfg.timeoutIntervalForRequest = std::max(1, stallSeconds);  // no data for this long: fails
      cfg.timeoutIntervalForResource = 6 * 3600;
      cfg.URLCache = nil;
      cfg.HTTPCookieStorage = nil;
      cfg.HTTPShouldSetCookies = NO;
      cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
      cfg.HTTPAdditionalHeaders = @{@"User-Agent" : ns(update::userAgent())};
      NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg delegate:delegate delegateQueue:nil];
      NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
      if (have > 0) [req setValue:ns("bytes=" + std::to_string(have) + "-") forHTTPHeaderField:@"Range"];
      NSURLSessionDataTask* task = [session dataTaskWithRequest:req];
      [task resume];
      bool cancelled = false;
      while (dispatch_semaphore_wait(sink->done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
        if (progress) progress(sink->received.load(), sink->total.load());
        if (!cancelled && cancel && cancel->load()) {
          cancelled = true;
          [task cancel];
        }
      }
      [session finishTasksAndInvalidate];
      if (progress) progress(sink->received.load(), sink->total.load());
      const int status = sink->status;
      if (cancelled) {
        error = "cancelled";
      } else if (status == 416) {
        ok = true;  // already whole; the caller checks it
      } else if (!sink->error.empty()) {
        error = sink->error;
        retry = sink->dropped && pass == 0;
      } else if (status == 200 || status == 206) {
        ok = true;
      } else {
        error = "the server refused the download (HTTP " + std::to_string(status) + ")";
      }
    }
    dispatch_release(sink->done);
    [delegate release];  // (the session let go of it when it finished: nothing calls it after this)
    if (ok) return true;
    if (retry) continue;
    if (err) *err = error.empty() ? "the download failed" : error;
    return false;
  }
  if (err) *err = "the download failed";
  return false;
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
