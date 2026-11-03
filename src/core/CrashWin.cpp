// Gridline: crash reports on Windows: the exception filter, the unwinder and the module table.
// Failures Windows ends without any handler (fast-fail heap corruption, for one) reach the sentinel,
// which reads the exit code. No forked copy here: the last periodic recovery copy is what stays.
#include "CrashInternal.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <signal.h>
#include <stdlib.h>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

namespace gl::crash::detail {

namespace {

DWORD gUiThreadId = 0;
HANDLE gUiThread = nullptr;
std::atomic<DWORD> gAudioThreadId{0};
std::atomic<LONG> gInFatal{0};
ReportInfo gFatalInfo;
HANDLE gParent = nullptr;  // sentinel: the process watched

std::wstring widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

std::string narrow(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

uintptr_t pcOf(const CONTEXT& c) {
#if defined(_M_ARM64) || defined(__aarch64__)
  return static_cast<uintptr_t>(c.Pc);
#else
  return static_cast<uintptr_t>(c.Rip);
#endif
}

void walkFrom(CONTEXT ctx, ReportInfo& info) {
  int n = 0;
  while (n < kMaxFrames) {
    const DWORD64 pc = pcOf(ctx);
    if (pc == 0) break;
    info.pcs[n] = static_cast<uintptr_t>(pc);
    info.scanned[n] = false;
    ++n;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &base, nullptr);
    if (fn) {
      void* handlerData = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, fn, &ctx, &handlerData, &establisher, nullptr);
    } else {  // a leaf function
#if defined(_M_ARM64) || defined(__aarch64__)
      ctx.Pc = ctx.Lr;
#else
      if (IsBadReadPtr(reinterpret_cast<const void*>(ctx.Rsp), sizeof(DWORD64))) break;
      ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
      ctx.Rsp += sizeof(DWORD64);
#endif
    }
    if (pcOf(ctx) == pc) break;
  }
  info.frames = n;
}

const char* exceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xE06D7363: return "C++ exception";
    default: return "EXCEPTION";
  }
}

void finishFatal(UINT exitCode) {
  ReportInfo& info = gFatalInfo;
  info.thread = plat::threadRole();
  SessionBlock* s = session();
  char name[128];
  if (s && s->frozen.load() && s->reportReady.load()) {
    char old[1200];
    joinPath(old, sizeof old, reportDirC(), s->reportName);
    DeleteFileA(old);
  }
  const bool ok = writeReportToDisk(info, name, sizeof name);
  if (s && ok) {
    copyText(s->reportName, sizeof s->reportName, name);
    s->reportReady.store(1);
    s->frozen.store(0);
  }
  TerminateProcess(GetCurrentProcess(), exitCode);
}

LONG WINAPI onException(EXCEPTION_POINTERS* ep) {
  if (InterlockedExchange(reinterpret_cast<volatile LONG*>(&gInFatal), 1) != 0) {
    Sleep(30000);
    TerminateProcess(GetCurrentProcess(), 3);
  }
  ReportInfo& info = gFatalInfo;
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  info.kind = "crash";
  info.cause = exceptionName(code);
  info.code = static_cast<int64_t>(code);
  if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && ep->ExceptionRecord->NumberParameters >= 2) {
    info.address = static_cast<uint64_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    info.hasAddress = true;
  }
  if (code == EXCEPTION_STACK_OVERFLOW) info.note = "stack overflow";
  const char* term = terminateText();
  if (term[0]) {
    info.typeName = "uncaught exception";
    info.message = term;
  }
  walkFrom(*ep->ContextRecord, info);
  finishFatal(code);
  return EXCEPTION_EXECUTE_HANDLER;
}

// abort() and the C runtime's fatal checks never reach the filter: the same report from here.
void reportHere(const char* cause, const char* note) {
  if (InterlockedExchange(reinterpret_cast<volatile LONG*>(&gInFatal), 1) != 0) {
    Sleep(30000);
    TerminateProcess(GetCurrentProcess(), 3);
  }
  ReportInfo& info = gFatalInfo;
  info.kind = "crash";
  info.cause = cause;
  info.note = note;
  const char* term = terminateText();
  if (term[0]) {
    info.typeName = "uncaught exception";
    info.message = term;
  }
  CONTEXT ctx;
  RtlCaptureContext(&ctx);
  walkFrom(ctx, info);
  finishFatal(3);
}

void onAbort(int) { reportHere("abort", "a failed check, or memory damage found by the allocator"); }
void onPurecall() { reportHere("pure virtual call", nullptr); }
void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) { reportHere("invalid parameter to a C runtime call", nullptr); }

void onTerminate() {
  char text[440];
  size_t n = 0;
  auto put = [&](const char* t) {
    for (; t && *t && n + 1 < sizeof text; ++t) text[n++] = *t;
  };
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      put(typeid(x).name());
      put(": ");
      put(x.what());
    } catch (...) {
      put("an exception of an unknown type");
    }
  } else {
    put("std::terminate called with no exception");
  }
  text[n] = 0;
  setTerminateText(text);
  reportHere("std::terminate", nullptr);
}

// The PDB's id: GUID and age from the CodeView record of the debug directory.
void readPdbId(HMODULE mod, Module& m) {
  const auto* base = reinterpret_cast<const uint8_t*>(mod);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return;
  const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (!dd.VirtualAddress || !dd.Size) return;
  const auto* dbg = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dd.VirtualAddress);
  const DWORD count = dd.Size / static_cast<DWORD>(sizeof(IMAGE_DEBUG_DIRECTORY));
  for (DWORD i = 0; i < count; ++i) {
    if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !dbg[i].AddressOfRawData || dbg[i].SizeOfData < 24) continue;
    const uint8_t* cv = base + dbg[i].AddressOfRawData;
    if (std::memcmp(cv, "RSDS", 4) == 0) {
      std::memcpy(m.idBytes, cv + 4, 20);  // GUID + age
      m.idLen = 20;
    }
    return;
  }
}

}  // namespace

namespace plat {

const char* threadRole() noexcept {
  const DWORD self = GetCurrentThreadId();
  if (self == gUiThreadId) return "ui";
  if (self == gAudioThreadId.load(std::memory_order_relaxed)) return "audio";
  return "worker";
}

void noteAudioThread() noexcept { gAudioThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed); }

void refreshModules() {
  HMODULE mods[kMaxModules];
  DWORD needed = 0;
  HANDLE self = GetCurrentProcess();
  if (!EnumProcessModules(self, mods, sizeof mods, &needed)) return;
  Module* t = beginModules();
  int n = 0;
  const int count = static_cast<int>(needed / sizeof(HMODULE));
  for (int i = 0; i < count && i < kMaxModules; ++i) {
    MODULEINFO mi;
    if (!GetModuleInformation(self, mods[i], &mi, sizeof mi)) continue;
    Module& m = t[n];
    m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    m.execStart = m.base;
    m.execEnd = m.base + mi.SizeOfImage;
    m.idLen = 0;
    wchar_t path[1024];
    path[0] = 0;
    GetModuleFileNameExW(self, mods[i], path, 1024);
    const std::string utf8 = narrow(path);
    copyText(m.name, sizeof m.name, baseName(utf8.c_str()));
    readPdbId(mods[i], m);
    ++n;
  }
  endModules(n);
}

void installHandlers(bool devBacktrace) {
  gUiThreadId = GetCurrentThreadId();
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &gUiThread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
  ULONG guarantee = 128 * 1024;  // room to report a stack overflow on the UI thread
  SetThreadStackGuarantee(&guarantee);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  SetUnhandledExceptionFilter(onException);
  _set_invalid_parameter_handler(onInvalidParameter);
  _set_purecall_handler(onPurecall);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  signal(SIGABRT, onAbort);
  if (!devBacktrace) std::set_terminate(onTerminate);
  // A plugin's own dialog runs a nested message loop on the UI thread: messages still being served
  // mean nothing is frozen. What is really stuck takes no more messages.
  SetWindowsHookExW(WH_GETMESSAGE, [](int code, WPARAM w, LPARAM l) -> LRESULT {
    uiHeartbeat();
    return CallNextHookEx(nullptr, code, w, l);
  }, nullptr, GetCurrentThreadId());
}

void sampleUiStack(ReportInfo& out) {
  if (!gUiThread || SuspendThread(gUiThread) == static_cast<DWORD>(-1)) {
    out.note = "the thread did not answer the look at its stack";
    return;
  }
  CONTEXT ctx;
  std::memset(&ctx, 0, sizeof ctx);
  ctx.ContextFlags = CONTEXT_FULL;
  const BOOL got = GetThreadContext(gUiThread, &ctx);
  ResumeThread(gUiThread);  // never walk with it stopped: it may hold the loader's lock
  if (got) walkFrom(ctx, out);
}

void currentStack(ReportInfo& out) {
  CONTEXT ctx;
  RtlCaptureContext(&ctx);
  walkFrom(ctx, out);
}

void currentThreadName(char* out, size_t cap) { copyText(out, cap, threadRole()); }

void* mapFile(const std::string& path, size_t size, bool create) {
  HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(size), nullptr);
  void* mem = map ? MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, size) : nullptr;
  if (map) CloseHandle(map);
  CloseHandle(f);
  return mem;
}

void unmapFile(void* mem, size_t) {
  if (mem) UnmapViewOfFile(mem);
}

bool writeFileAtomic(const char* tmp, const char* final, const ReportInfo& info) noexcept {
  wchar_t wtmp[1200], wfinal[1200];
  if (!MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wtmp, 1200) || !MultiByteToWideChar(CP_UTF8, 0, final, -1, wfinal, 1200)) return false;
  HANDLE f = CreateFileW(wtmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  {
    Writer w(
        [](void* ctx, const char* d, size_t n) {
          DWORD written = 0;
          WriteFile(static_cast<HANDLE>(ctx), d, static_cast<DWORD>(n), &written, nullptr);
        },
        f);
    writeReport(info, w);
  }
  FlushFileBuffers(f);
  CloseHandle(f);
  return MoveFileExW(wtmp, wfinal, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int64_t unixTime() noexcept {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return static_cast<int64_t>((t - 116444736000000000ull) / 10000000ull);
}

static std::wstring quoted(const std::string& arg) {
  std::wstring w = L"\"";
  const std::wstring a = widen(arg);
  size_t slashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++slashes;
    } else {
      if (c == L'"') w.append(slashes + 1, L'\\');
      slashes = 0;
    }
    w.push_back(c);
  }
  w.append(slashes, L'\\');
  w.push_back(L'"');
  return w;
}

bool spawnDetached(const std::string& exe, const std::vector<std::string>& args) {
  std::wstring cmd = quoted(exe);
  for (const std::string& a : args) cmd += L" " + quoted(a);
  STARTUPINFOW si;
  std::memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  PROCESS_INFORMATION pi;
  std::memset(&pi, 0, sizeof pi);
  if (!CreateProcessW(widen(exe).c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

void spawnSentinel(const std::string& self, const std::string& sessionFile, const std::string& testDir) {
  std::vector<std::string> args = {"--crash-sentinel", sessionFile, std::to_string(GetCurrentProcessId())};
  if (!testDir.empty()) args.push_back(testDir);
  spawnDetached(self, args);
}

void closeSentinelLink() {}

bool waitParent(const char* link, int pollMs) {
  if (!gParent) {
    gParent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(std::strtoul(link, nullptr, 10)));
    if (!gParent) return true;
  }
  return WaitForSingleObject(gParent, static_cast<DWORD>(pollMs)) != WAIT_TIMEOUT;
}

uint32_t parentExitCode(const char*) {
  DWORD code = 0;
  if (gParent) GetExitCodeProcess(gParent, &code);
  return code;
}

void detachSentinel() {}

void endProcess(int64_t pid) {
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (h) {
    TerminateProcess(h, 1);
    CloseHandle(h);
  }
}

bool alive(int64_t pid) {
  if (pid <= 0) return false;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!h) return false;
  DWORD code = 0;
  const BOOL ok = GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return ok && code == STILL_ACTIVE;
}

std::string selfPath() {
  wchar_t buf[2048];
  const DWORD n = GetModuleFileNameW(nullptr, buf, 2048);
  if (n == 0 || n >= 2048) return {};
  return narrow(buf);
}

std::string osDescription() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW v;
  std::memset(&v, 0, sizeof v);
  v.dwOSVersionInfoSize = sizeof v;
  if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
    auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
    if (fn) fn(&v);
  }
  char b[64];
  std::snprintf(b, sizeof b, "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
  return b;
}

const char* archName() noexcept {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#else
  return "x86_64";
#endif
}

int64_t currentPid() noexcept { return static_cast<int64_t>(GetCurrentProcessId()); }

void rescueNow() {}  // no fork here: the last periodic copy stays

void sleepMs(int ms) { Sleep(static_cast<DWORD>(ms)); }

namespace {
volatile int gZero = 0;
volatile int gSink = 0;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4717)  // the recursion is the point
#endif
int deep(int depth) {
  volatile char pad[4096];
  pad[0] = static_cast<char>(depth);
  if (gZero == 0) gSink = deep(depth + 1) + pad[0];
  return gSink;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
void segv() { *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(gZero + 8)) = 1; }
}  // namespace

void forceCrash(const std::string& kind) {
  if (kind == "segv") segv();
  if (kind == "ill") RaiseException(EXCEPTION_ILLEGAL_INSTRUCTION, 0, 0, nullptr);
  if (kind == "fpe") RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr);
  if (kind == "bus") RaiseException(EXCEPTION_IN_PAGE_ERROR, 0, 0, nullptr);
  if (kind == "abort") std::abort();
  if (kind == "terminate") std::thread([] { throw std::runtime_error("forced uncaught failure in C:\\Users\\someone\\Secret Songs\\x.flp: test"); }).join();
  if (kind == "overflow") gSink = deep(0);
  if (kind == "heap") {
    char* volatile p = static_cast<char*>(std::malloc(64));
    std::free(p);
    std::free(p);
    std::abort();  // the allocator did not notice: the abort stands for it
  }
  if (kind == "thread") std::thread([] { segv(); }).join();
  if (kind == "freeze") {
    for (;;) Sleep(1000);
  }
  if (kind.rfind("stall:", 0) == 0) Sleep(static_cast<DWORD>(std::atoi(kind.c_str() + 6)) * 1000);
}

}  // namespace plat

}  // namespace gl::crash::detail
#endif  // _WIN32
