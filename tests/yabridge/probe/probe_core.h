// Gridline yabridge test: the part of the Windows probe plugin every format shares.
//
// The probe is a small Win32 plugin. Its window logs every mouse, wheel, key and focus event with
// client coordinates to C:\gridline-probe\log.txt inside the Wine prefix, so the test can prove
// where a click landed, not only that something happened. It also paints a known pattern (coloured
// corners, a slider, the last click) so a screenshot proves the editor is drawn at the right size.
//
// Client layout at the default size (640 x 400):
//   slider    x 20..220,  y 40..240   vertical drag or wheel: parameter 0, reported to the host
//   grow      x 240..340, y 40..90    click: asks the host for 800 x 500
//   shrink    x 240..340, y 100..150  click: asks the host for 500 x 320
//   corners   12 x 12 px              red top left, green top right, blue bottom left, yellow bottom right
#pragma once
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace probe {

constexpr int kParams = 3;
constexpr int kDefaultW = 640, kDefaultH = 400;
constexpr int kLatency = 64;
inline const char* const kParamNames[kParams] = {"Level", "Tone", "Switch"};
inline const double kParamDefaults[kParams] = {0.5, 0.25, 0.0};

// Instance numbers across every process of this Wine prefix (VST2 hosts run one plugin per process).
inline int nextInstance() {
  HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), "GridlineProbeInstances");
  auto* count = map ? static_cast<volatile LONG*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG))) : nullptr;
  return count ? static_cast<int>(InterlockedIncrement(count)) : 1;  // the mapping stays open with the process
}

// One line per event: milliseconds, instance, event, values. Opened per line so that the test can
// delete the log between cases while a host process keeps running.
inline void logf(int inst, const char* fmt, ...) {
  char body[400];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  char line[460];
  const int n = std::snprintf(line, sizeof line, "%llu\t%d\t%s\r\n", static_cast<unsigned long long>(GetTickCount64()), inst, body);
  CreateDirectoryA("C:\\gridline-probe", nullptr);
  HANDLE h = CreateFileA("C:\\gridline-probe\\log.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD done = 0;
  WriteFile(h, line, static_cast<DWORD>(n), &done, nullptr);
  CloseHandle(h);
}

// Sound and parameters. The synth is one sine voice whose level is parameter 0; the effect
// multiplies by 2 x parameter 0 and delays by kLatency samples, so a render can be measured.
struct Core {
  const int inst = nextInstance();
  const bool synth;
  std::atomic<double> p[kParams];
  double sr = 48000;
  double phase = 0, amp = 0;
  int key = 60;
  float delay[2][kLatency] = {};
  int delayPos = 0;
  // Written on the audio thread, logged on the interface thread (no I/O while processing).
  std::atomic<uint64_t> blocks{0}, notesOn{0}, notesOff{0};
  std::atomic<int> lastKey{-1};
  std::atomic<float> inPeak{0}, outPeak{0};

  explicit Core(bool isSynth) : synth(isSynth) {
    for (int i = 0; i < kParams; ++i) p[i].store(kParamDefaults[i]);
    logf(inst, "create %s", synth ? "synth" : "fx");
  }
  void noteOn(int k, double velocity) {
    key = k;
    amp = velocity;
    phase = 0;
    lastKey.store(k);
    notesOn.fetch_add(1);
    logf(inst, "note on %d", k);  // I/O on the audio thread: this is a test tool, notes are rare
  }
  void noteOff(int k) {
    if (k == key) amp = 0;
    notesOff.fetch_add(1);
  }
  void render(const float* const* in, float* const* out, int32_t n) {
    float ip = 0, op = 0;
    const double level = p[0].load();
    for (int32_t i = 0; i < n; ++i) {
      float l = 0, r = 0;
      if (synth) {
        if (amp > 0) {
          phase += 440.0 * std::pow(2.0, (key - 69) / 12.0) / sr;
          phase -= std::floor(phase);
          l = r = static_cast<float>(std::sin(6.283185307179586 * phase) * amp * level);
        }
      } else {
        const float a = in ? in[0][i] : 0.0f, b = in ? in[1][i] : 0.0f;
        ip = std::fmax(ip, std::fmax(std::fabs(a), std::fabs(b)));
        l = delay[0][delayPos];
        r = delay[1][delayPos];
        delay[0][delayPos] = static_cast<float>(a * level * 2.0);
        delay[1][delayPos] = static_cast<float>(b * level * 2.0);
        delayPos = (delayPos + 1) % kLatency;
      }
      out[0][i] = l;
      out[1][i] = r;
      op = std::fmax(op, std::fmax(std::fabs(l), std::fabs(r)));
    }
    if (ip > inPeak.load(std::memory_order_relaxed)) inPeak.store(ip, std::memory_order_relaxed);
    if (op > outPeak.load(std::memory_order_relaxed)) outPeak.store(op, std::memory_order_relaxed);
    blocks.fetch_add(1, std::memory_order_relaxed);
  }
  void logAudio() const {
    logf(inst, "audio blocks %llu in %.6f out %.6f noteson %llu notesoff %llu lastkey %d", static_cast<unsigned long long>(blocks.load()), static_cast<double>(inPeak.load()),
         static_cast<double>(outPeak.load()), static_cast<unsigned long long>(notesOn.load()), static_cast<unsigned long long>(notesOff.load()), lastKey.load());
  }
  // State: a magic number and the parameters.
  static constexpr size_t kStateSize = 4 + sizeof(double) * kParams;
  void saveState(char* to) const {
    std::memcpy(to, "GLPB", 4);
    for (int i = 0; i < kParams; ++i) {
      const double v = p[i].load();
      std::memcpy(to + 4 + sizeof(double) * static_cast<size_t>(i), &v, sizeof v);
    }
    logf(inst, "state save %.6f %.6f %.6f", p[0].load(), p[1].load(), p[2].load());
  }
  bool loadState(const char* from, size_t size) {
    if (size < kStateSize || std::memcmp(from, "GLPB", 4) != 0) {
      logf(inst, "state load refused size %llu", static_cast<unsigned long long>(size));
      return false;
    }
    for (int i = 0; i < kParams; ++i) {
      double v = 0;
      std::memcpy(&v, from + 4 + sizeof(double) * static_cast<size_t>(i), sizeof v);
      p[i].store(v);
    }
    logf(inst, "state load %.6f %.6f %.6f", p[0].load(), p[1].load(), p[2].load());
    return true;
  }
};

// What the window asks of the format's wrapper.
struct Host {
  virtual ~Host() = default;
  virtual void beginEdit(int param) = 0;
  virtual void performEdit(int param, double value) = 0;
  virtual void endEdit(int param) = 0;
  virtual bool requestSize(int w, int h) = 0;
};

class Window {
 public:
  Window(Core& core, Host& host) : core_(core), host_(host) {}
  ~Window() { close(); }
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  bool open(HWND parent, int w, int h) {
    static const ATOM cls = [] {
      WNDCLASSEXA c{};
      c.cbSize = sizeof c;
      c.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
      c.lpfnWndProc = &Window::proc;
      c.hInstance = GetModuleHandleA(nullptr);
      c.hCursor = LoadCursor(nullptr, IDC_ARROW);
      c.lpszClassName = "GridlineYabridgeProbe";
      return RegisterClassExA(&c);
    }();
    (void)cls;
    w_ = w, h_ = h;
    hwnd_ = CreateWindowExA(0, "GridlineYabridgeProbe", "Gridline probe", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, w, h, parent, nullptr,
                            GetModuleHandleA(nullptr), this);
    logf(core_.inst, "attached %d %d hwnd %s", w, h, hwnd_ ? "ok" : "failed");
    if (hwnd_) SetTimer(hwnd_, 1, 200, nullptr);
    return hwnd_ != nullptr;
  }
  void close() {
    if (!hwnd_) return;
    KillTimer(hwnd_, 1);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    logf(core_.inst, "removed");
  }
  void resize(int w, int h) {
    w_ = w, h_ = h;
    logf(core_.inst, "size %d %d", w, h);
    if (hwnd_) {
      SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }
  int width() const { return w_; }
  int height() const { return h_; }
  bool isOpen() const { return hwnd_ != nullptr; }

 private:
  static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
      SetWindowLongPtrA(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTA*>(lp)->lpCreateParams));
      return DefWindowProcA(h, msg, wp, lp);
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrA(h, GWLP_USERDATA));
    return self ? self->handle(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);
  }
  static int mx(LPARAM lp) { return static_cast<short>(LOWORD(lp)); }
  static int my(LPARAM lp) { return static_cast<short>(HIWORD(lp)); }
  static bool inRect(int x, int y, int l, int t, int r, int b) { return x >= l && x < r && y >= t && y < b; }

  void press(HWND h, const char* button, int x, int y, bool dbl) {
    logf(core_.inst, "%s %s %d %d", dbl ? "dbl" : "down", button, x, y);
    SetFocus(h);
    SetCapture(h);
    lastX_ = x, lastY_ = y;
    ++clicks_;
    if (!std::strcmp(button, "L")) {
      if (inRect(x, y, 20, 40, 220, 240)) {
        dragging_ = true;
        dragY_ = y;
        dragStart_ = core_.p[0].load();
        host_.beginEdit(0);
      } else if (inRect(x, y, 240, 40, 340, 90)) {
        logf(core_.inst, "request %d %d: %s", 800, 500, host_.requestSize(800, 500) ? "accepted" : "refused");
      } else if (inRect(x, y, 240, 100, 340, 150)) {
        logf(core_.inst, "request %d %d: %s", 500, 320, host_.requestSize(500, 320) ? "accepted" : "refused");
      }
    }
    InvalidateRect(h, nullptr, FALSE);
  }
  void release(HWND h, const char* button, int x, int y) {
    logf(core_.inst, "up %s %d %d", button, x, y);
    ReleaseCapture();
    if (dragging_) {
      dragging_ = false;
      host_.endEdit(0);
    }
    InvalidateRect(h, nullptr, FALSE);
  }
  void setLevel(double v) {
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    core_.p[0].store(v);
    host_.performEdit(0, v);
    logf(core_.inst, "edit 0 %.6f", v);
  }
  void timer(HWND h) {
    for (int i = 0; i < kParams; ++i) {
      const double v = core_.p[i].load();
      if (v != shown_[i]) {
        shown_[i] = v;
        logf(core_.inst, "param %d %.6f", i, v);
        InvalidateRect(h, nullptr, FALSE);
      }
    }
    const uint64_t b = core_.blocks.load();
    if (b != loggedBlocks_ && GetTickCount64() - loggedAt_ > 1000) {
      loggedBlocks_ = b;
      loggedAt_ = GetTickCount64();
      core_.logAudio();
    }
  }
  void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc{l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
  }
  void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    fill(dc, 0, 0, w_, h_, (clicks_ & 1) ? RGB(70, 90, 70) : RGB(40, 60, 90));
    fill(dc, 20, 40, 220, 240, RGB(20, 20, 20));
    const int fillTop = 240 - static_cast<int>(200.0 * core_.p[0].load());
    fill(dc, 20, fillTop, 220, 240, RGB(230, 140, 40));
    fill(dc, 240, 40, 340, 90, RGB(60, 160, 60));
    fill(dc, 240, 100, 340, 150, RGB(160, 60, 60));
    fill(dc, 0, 0, 12, 12, RGB(255, 0, 0));
    fill(dc, w_ - 12, 0, w_, 12, RGB(0, 255, 0));
    fill(dc, 0, h_ - 12, 12, h_, RGB(0, 0, 255));
    fill(dc, w_ - 12, h_ - 12, w_, h_, RGB(255, 255, 0));
    if (clicks_ > 0) {
      fill(dc, lastX_ - 10, lastY_, lastX_ + 11, lastY_ + 1, RGB(255, 255, 255));
      fill(dc, lastX_, lastY_ - 10, lastX_ + 1, lastY_ + 11, RGB(255, 255, 255));
    }
    EndPaint(h, &ps);
  }
  LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_MOUSEMOVE: {
        const int x = mx(lp), y = my(lp);
        if (x == moveX_ && y == moveY_) return 0;
        moveX_ = x, moveY_ = y;
        logf(core_.inst, "move %d %d", x, y);
        if (!tracking_) {
          TRACKMOUSEEVENT t{sizeof t, TME_HOVER | TME_LEAVE, h, 200};
          tracking_ = TrackMouseEvent(&t) != 0;
        }
        if (dragging_) {
          setLevel(dragStart_ + (dragY_ - y) / 200.0);
          InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
      }
      case WM_MOUSEHOVER: logf(core_.inst, "hover %d %d", mx(lp), my(lp)); tracking_ = false; return 0;
      case WM_MOUSELEAVE: logf(core_.inst, "leave"); tracking_ = false; moveX_ = moveY_ = -100000; return 0;
      case WM_LBUTTONDOWN: press(h, "L", mx(lp), my(lp), false); return 0;
      case WM_RBUTTONDOWN: press(h, "R", mx(lp), my(lp), false); return 0;
      case WM_MBUTTONDOWN: press(h, "M", mx(lp), my(lp), false); return 0;
      case WM_LBUTTONDBLCLK: press(h, "L", mx(lp), my(lp), true); return 0;
      case WM_RBUTTONDBLCLK: press(h, "R", mx(lp), my(lp), true); return 0;
      case WM_LBUTTONUP: release(h, "L", mx(lp), my(lp)); return 0;
      case WM_RBUTTONUP: release(h, "R", mx(lp), my(lp)); return 0;
      case WM_MBUTTONUP: release(h, "M", mx(lp), my(lp)); return 0;
      case WM_MOUSEWHEEL: {
        POINT pt{mx(lp), my(lp)};  // screen coordinates
        ScreenToClient(h, &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        logf(core_.inst, "wheel %d %ld %ld", delta, pt.x, pt.y);
        if (inRect(pt.x, pt.y, 20, 40, 220, 240)) {
          host_.beginEdit(0);
          setLevel(core_.p[0].load() + (delta > 0 ? 0.05 : -0.05));
          host_.endEdit(0);
          InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
      }
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
        logf(core_.inst, "keydown %u shift %d ctrl %d alt %d", static_cast<unsigned>(wp), GetKeyState(VK_SHIFT) < 0 ? 1 : 0, GetKeyState(VK_CONTROL) < 0 ? 1 : 0,
             GetKeyState(VK_MENU) < 0 ? 1 : 0);
        return 0;
      case WM_KEYUP:
      case WM_SYSKEYUP: logf(core_.inst, "keyup %u", static_cast<unsigned>(wp)); return 0;
      case WM_CHAR: logf(core_.inst, "char %u", static_cast<unsigned>(wp)); return 0;
      case WM_SETFOCUS: logf(core_.inst, "focus in"); return 0;
      case WM_KILLFOCUS: logf(core_.inst, "focus out"); return 0;
      case WM_TIMER: timer(h); return 0;
      case WM_ERASEBKGND: return 1;
      case WM_PAINT: paint(h); return 0;
      default: return DefWindowProcA(h, msg, wp, lp);
    }
  }
  Core& core_;
  Host& host_;
  HWND hwnd_ = nullptr;
  int w_ = kDefaultW, h_ = kDefaultH;
  int clicks_ = 0, lastX_ = 0, lastY_ = 0, moveX_ = -100000, moveY_ = -100000;
  bool dragging_ = false, tracking_ = false;
  int dragY_ = 0;
  double dragStart_ = 0;
  double shown_[kParams] = {-1, -1, -1};
  uint64_t loggedBlocks_ = 0;
  ULONGLONG loggedAt_ = 0;
};

}  // namespace probe
