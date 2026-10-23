// Gridline tests — J-91 (GitHub issue #1, UI bugs on a Zorin OS laptop):
//  * the interface scale follows the desktop's scale setting, never the
//    panel's physical DPI (1920x1080 laptop at 100 % -> 100 %);
//  * keys typed into a plugin's own X11 / Windows interface: Gridline's global
//    shortcuts (Space ...) are forwarded, everything else stays with the plugin;
//  * the Auto Pan pad: its live dot and its handle in one coordinate space,
//    always inside the pad (one mapping for drawing and the pointer);
//  * Interface animations (master switch) and the metronome swing;
//  * key / pad lights without animations switch instead of fading.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include "Check.h"
#include "app/KeyLightState.h"
#include "app/XyPadMath.h"
#include "core/Keymap.h"
#include "core/Motion.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/UiScale.h"
#include "engine/Effects.h"
#include "host/KeyForward.h"

using namespace gl;

namespace {

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// ------------------------------------------------------------------ interface scale
void testUiScale() {
  using namespace uiscale;
  Inputs laptop;  // the reporter's laptop: 1920x1080 15" panel (~141 physical dpi), GNOME at 100 %
  laptop.desktop = Desktop::X11;
  laptop.displayDpi = 141;  // what SDL_GetDisplayDPI reports from the panel's size (ignored on Linux now)
  laptop.xftDpi = 96;
  laptop.windowW = laptop.drawableW = 1920;
  expect(systemScale(laptop) == 1.0f, "1920x1080 laptop at 100 % (Xft.dpi 96, 141 physical dpi): 100 %");
  laptop.xftDpi = 0;
  expect(systemScale(laptop) == 1.0f, "no Xft.dpi at all: 100 %, the panel's physical DPI is not used");
  Inputs x = laptop;
  x.xftDpi = 192;
  expect(systemScale(x) == 2.0f, "Xft.dpi 192 (GNOME scaling 200 %): 200 %");
  x.xftDpi = 120;
  expect(systemScale(x) == 1.25f, "Xft.dpi 120 (text scaling 1.25): 125 %");
  x.xftDpi = 144;
  expect(systemScale(x) == 1.5f && systemScaleSource(x) == "Xft.dpi 144", "Xft.dpi 144: 150 %, reported as such");
  x.xftDpi = 72;
  expect(systemScale(x) == 1.0f, "a smaller Xft.dpi never shrinks the interface below 100 %");
  // Environment overrides (GTK / Qt conventions).
  Inputs e = laptop;
  e.xftDpi = 96;
  e.gdkScale = "2";
  expect(systemScale(e) == 2.0f, "GDK_SCALE=2: 200 %");
  e.gdkDpiScale = "0.75";
  expect(systemScale(e) == 1.5f, "GDK_SCALE=2 GDK_DPI_SCALE=0.75: 150 %");
  e.gdkScale = nullptr;
  e.gdkDpiScale = "1.25";
  expect(systemScale(e) == 1.25f, "GDK_DPI_SCALE=1.25 alone: 125 %");
  e.gdkDpiScale = "";
  e.gdkScale = "abc";
  expect(systemScale(e) == 1.0f, "empty / malformed GDK values are ignored (Xft.dpi 96 -> 100 %)");
  Inputs q = laptop;
  q.qtScaleFactor = "1.5";
  expect(systemScale(q) == 1.5f, "QT_SCALE_FACTOR=1.5: 150 %");
  // Wayland: SDL already scales the window (drawable larger than the window): nothing on top.
  Inputs w;
  w.desktop = Desktop::Wayland;
  w.windowW = 1280, w.drawableW = 2560;
  expect(systemScale(w) == 1.0f && systemScaleSource(w) == "compositor (window pixel density)", "Wayland output scale 2 (drawable 2x window): SDL scales, 100 % here");
  w.drawableW = 1280;
  expect(systemScale(w) == 1.0f, "Wayland at 100 %: 100 %");
  // macOS and Windows keep their behaviour.
  Inputs mac;
  mac.desktop = Desktop::MacOS;
  mac.xftDpi = 192, mac.displayDpi = 220, mac.windowW = 1440, mac.drawableW = 2880, mac.gdkScale = "2";
  expect(systemScale(mac) == 1.0f, "macOS: always 100 % here (Retina is the backing scale)");
  Inputs win;
  win.desktop = Desktop::Windows;
  win.windowW = win.drawableW = 2560;
  win.displayDpi = 144;
  expect(systemScale(win) == 1.5f, "Windows at 150 % (144 dpi): 150 % as before");
  win.displayDpi = 96;
  expect(systemScale(win) == 1.0f, "Windows at 100 %: 100 %");
  win.displayDpi = 120;
  expect(systemScale(win) == 1.0f, "Windows at 125 % (120 dpi): 100 % as before (only above 120 dpi)");
  // Xft.dpi parsing (XResourceManagerString).
  expect(parseXftDpi("Xft.antialias:\t1\nXft.dpi:\t96\nXft.hinting:\t1\n") == 96.0f, "Xft.dpi parsed among other resources");
  expect(parseXftDpi("Xft.dpi: 144.5") == 144.5f, "Xft.dpi without a trailing newline, fractional");
  expect(parseXftDpi("  Xft*dpi :\t192\n") == 192.0f, "Xft*dpi binding, spaces around the name");
  expect(parseXftDpi("Xcursor.size:\t24\nXft.dpix:\t300\n") == 0.0f, "no Xft.dpi: 0");
  expect(parseXftDpi(nullptr) == 0.0f && parseXftDpi("") == 0.0f, "no resources: 0");
  expect(parseXftDpi("Xft.dpi:\tabc\n") == 0.0f, "malformed value: 0");
  // The user's setting: multiplied by the system scale when following it, used as it is otherwise.
  expect(effectiveScale(1.0, true, 1.0f, 1920, 1000) == 1.0f, "first run on the 100 % laptop: 100 %");
  expect(effectiveScale(1.25, true, 1.0f, 1920, 1000) == 1.25f, "user 125 % on a 100 % desktop: 125 %");
  expect(effectiveScale(1.0, true, 2.0f, 3840, 2160) == 2.0f, "200 % desktop: 200 %");
  expect(effectiveScale(1.0, false, 2.0f, 3840, 2160) == 1.0f, "not following the system: the user's 100 % exactly");
  expect(effectiveScale(1.5, false, 2.0f, 3840, 2160) == 1.5f, "not following the system: the user's 150 % exactly");
  expect(near(effectiveScale(2.0, true, 1.5f, 1920, 1080), 1.8), "never larger than fits the 960 x 600 minimum layout");
  expect(effectiveScale(0.5, true, 1.0f, 1920, 1080) == 0.75f, "the user's scale is clamped to 75 % .. 200 %");
}

// ------------------------------------------------------------------ plugin keys (X11 / Windows)
bool isCommand(const keys::Chord& c, const char* id) {
  for (const keys::Command* cmd : keys::keymap().lookup(keys::Context::Global, c))
    if (std::string(cmd->id) == id) return true;
  return false;
}

void testPluginKeys() {
  using namespace host;
  // X11 keysyms -> HID usages.
  expect(hidUsageOfX11Keysym(0x20) == 44, "XK_space -> Space");
  expect(hidUsageOfX11Keysym(0x73) == 22 && hidUsageOfX11Keysym(0x53) == 22, "XK_s / XK_S -> S");
  expect(hidUsageOfX11Keysym(0x31) == 30 && hidUsageOfX11Keysym(0x30) == 39, "XK_1 / XK_0");
  expect(hidUsageOfX11Keysym(0xFFBE) == 58 && hidUsageOfX11Keysym(0xFFC9) == 69, "XK_F1 / XK_F12");
  expect(hidUsageOfX11Keysym(0xFF0D) == 40 && hidUsageOfX11Keysym(0xFF1B) == 41, "Return / Escape");
  expect(hidUsageOfX11Keysym(0xFFB0) == 98 && hidUsageOfX11Keysym(0xFFB5) == 93, "keypad 0 / 5");
  expect(hidUsageOfX11Keysym(0x1234567) == 0, "unknown keysym -> 0");
  for (unsigned long ks = 0x20; ks < 0x7F; ++ks) {  // every printable ASCII keysym maps to the key that types it (unshifted)
    const int u = hidUsageOfX11Keysym(ks);
    if (u) expect(macKeyOfHidUsage(u) != 0xFFFF, ("keysym " + std::to_string(ks) + " has a forwardable key").c_str());
  }
  expect(modsOfX11State(1) == kKeyShift && modsOfX11State(4) == kKeyCtrl && modsOfX11State(8) == kKeyAlt && modsOfX11State(64) == kKeyCmd,
         "X11 state bits: Shift, Control, Mod1 = Alt, Mod4 = Super");
  expect(modsOfX11State(2 | 16) == 0, "Caps Lock and Num Lock are not modifiers of a chord");
  // What reaches Gridline from a plugin interface that also sees the key.
  expect(forwardsToHost(44, 0), "Space (play / pause) is forwarded");
  expect(forwardsToHost(44, kKeyShift), "Shift+Space too");
  expect(forwardsToHost(58, 0) && forwardsToHost(69, 0), "F1-F12 (Gridline's windows) are forwarded");
  expect(forwardsToHost(22, kKeyCtrl), "Ctrl+S (save) is forwarded");
  expect(forwardsToHost(29, kKeyCtrl | kKeyShift), "Ctrl+Shift+Z is forwarded");
  expect(forwardsToHost(4, kKeyAlt), "Alt chords are forwarded");
  expect(!forwardsToHost(22, 0) && !forwardsToHost(29, kKeyShift), "plain letters stay with the plugin (its keyboard notes, its text)");
  expect(!forwardsToHost(30, 0) && !forwardsToHost(40, 0) && !forwardsToHost(42, 0), "digits, Return and Backspace stay with the plugin");
  expect(!forwardsToHost(0, kKeyCtrl), "unknown keys are never forwarded");
  // The forwarded event is Gridline's play / stop command: an X11 Space from Sausage Fattener's window.
  KeyEvent k;
  k.code = macKeyOfHidUsage(hidUsageOfX11Keysym(0x20));
  k.mods = modsOfX11State(0);
  expect(forwardsToHost(hidUsageOfMacKey(k.code), k.mods) && isCommand(chordOf(k), "transport.play"),
         "Space typed into a plugin's X11 window toggles Gridline's transport");
  KeyEvent save;
  save.code = macKeyOfHidUsage(hidUsageOfX11Keysym(0x73));
  save.mods = modsOfX11State(4);
#if !defined(__APPLE__)
  expect(isCommand(chordOf(save), "file.save"), "Ctrl+S typed into a plugin's X11 window saves");
#endif
  // Windows virtual keys through the same rule.
  expect(forwardsToHost(hidUsageOfWinVk(0x20), 0) && !forwardsToHost(hidUsageOfWinVk('A'), 0), "Windows: VK_SPACE forwarded, plain A not");
  // Which windows of the interface Gridline watches (other clients' event masks).
  expect(x11WatchesKeys(1L | (1L << 1)), "a window the plugin takes key presses on is watched");
  expect(!x11WatchesKeys(1L << 2) && !x11WatchesKeys(0), "a window nobody takes keys on is not (its keys reach Gridline's window)");
}

// ------------------------------------------------------------------ Auto Pan pad
void testAutoPanPad() {
  // The live dot comes from the effect's meters: pan -1..1, volume on the volume knob's scale.
  PluginState s = defaultPluginState("gl.autopan");
  s.params[static_cast<size_t>(pp::ApStaticPan)] = 0.04f;
  s.params[static_cast<size_t>(pp::ApStaticVol)] = 1.1f;  // the reporter's settings: 4 % right, 110 %
  auto fx = createEffect("gl.autopan");
  ProcessContext c;
  c.sampleRate = 44100;
  c.tempo = 140;
  std::vector<float> l(512, 0.3f), r(512, 0.3f);
  fx->process(s, c, l.data(), r.data(), 512);
  float m[2] = {0, 0};
  expect(fx->meters(m, 2) == 2, "auto pan reports pan and volume");
  expect(near(m[0], 0.04, 1e-6), "LFO off: the live pan is the knob's pan");
  char label[160];
  std::snprintf(label, sizeof label, "LFO off: the live volume is the knob's 110 %% (%.4f; 0.6.0 reported the gain 1.29, drawn above the pad)",
                static_cast<double>(m[1]));
  expect(near(m[1], 1.1, 1e-4), label);
  // With a volume LFO the live volume stays on the knob's scale and inside the pad's range.
  s.params[static_cast<size_t>(pp::ApTarget)] = 2;
  s.params[static_cast<size_t>(pp::ApAmount)] = 1;
  float lo = 9, hi = -9;
  for (int b = 0; b < 400; ++b) {
    fx->process(s, c, l.data(), r.data(), 512);
    fx->meters(m, 2);
    lo = std::min(lo, m[1]), hi = std::max(hi, m[1]);
  }
  std::snprintf(label, sizeof label, "volume LFO: live volume moves on the knob's scale (%.3f .. %.3f)", static_cast<double>(lo), static_cast<double>(hi));
  expect(lo < 1.05f && hi > 1.15f && lo >= 0.0f && hi < 1.6f, label);

  // One mapping for the handle and the pointer, inside the pad, at any scale.
  for (float scale : {1.0f, 1.5f, 2.0f}) {
    const xypad::Rect pad{100 * scale, 50 * scale, 560 * scale, 510 * scale};
    const float inset = 6.2f * scale;
    bool round = true, inside = true;
    for (float nx = 0; nx <= 1.0001f; nx += 0.125f)
      for (float ny = 0; ny <= 1.0001f; ny += 0.125f) {
        float sx = 0, sy = 0, bx = 0, by = 0;
        xypad::toScreen(pad, nx, ny, inset, sx, sy);
        xypad::fromScreen(pad, sx, sy, inset, bx, by);
        round = round && std::fabs(bx - nx) < 1e-4f && std::fabs(by - ny) < 1e-4f;
        inside = inside && sx - inset >= pad.x0 - 1e-3f && sx + inset <= pad.x1 + 1e-3f && sy - inset >= pad.y0 - 1e-3f && sy + inset <= pad.y1 + 1e-3f;
      }
    std::snprintf(label, sizeof label, "scale %.1f: a handle dragged to a point is drawn exactly under the pointer", static_cast<double>(scale));
    expect(round, label);
    std::snprintf(label, sizeof label, "scale %.1f: the handle (with its radius) stays inside the pad", static_cast<double>(scale));
    expect(inside, label);
    float sx = 0, sy = 0;
    xypad::toScreen(pad, 0.516f, 1.32f, inset, sx, sy);  // 0.6.0's live dot: 129 % volume on a 0..125 % pad
    std::snprintf(label, sizeof label, "scale %.1f: a value past the pad's range is drawn on its edge, not outside", static_cast<double>(scale));
    expect(near(sy, pad.y0 + inset, 1e-3), label);
    float nx = 0, ny = 0;
    xypad::fromScreen(pad, pad.x0 - 40, pad.y1 + 40, inset, nx, ny);
    std::snprintf(label, sizeof label, "scale %.1f: dragging past the pad's corner gives its corner value", static_cast<double>(scale));
    expect(nx == 0.0f && ny == 0.0f, label);
  }
}

// ------------------------------------------------------------------ motion
void testMotion() {
  using namespace motion;
  Switches s = effective(true, true, true, true, false);
  expect(s.animations && s.smoothSnap && s.smoothScroll && s.elastic, "defaults: everything moves");
  s = effective(false, true, true, true, false);
  expect(!s.animations && !s.smoothSnap && !s.smoothScroll && !s.elastic, "Interface animations off: no decorative motion at all");
  s = effective(true, true, true, true, true);
  expect(!s.animations && !s.smoothSnap && !s.smoothScroll && !s.elastic, "the system's reduced motion turns everything off");
  s = effective(true, false, true, false, false);
  expect(s.animations && !s.smoothSnap && s.smoothScroll && !s.elastic, "the single switches still apply under the master");

  // Metronome swing at 140 BPM: right on even beats, left on odd beats, a short eased turn on each beat.
  expect(metronomeSwing(0.5, 140, true) == 1.0 && metronomeSwing(1.5, 140, true) == -1.0 && metronomeSwing(2.6, 140, true) == 1.0,
         "faces right on even beats, left on odd beats");
  expect(metronomeSwing(1.0, 140, true) == 1.0, "a beat starts from the previous side");
  const double turn = kSwingSeconds * 140 / 60.0;
  expect(std::fabs(metronomeSwing(1.0 + turn * 0.5, 140, true)) < 1e-9, "half way through the turn: edge on (0)");
  expect(metronomeSwing(1.0 + turn, 140, true) == -1.0, "the turn ends after kSwingSeconds");
  bool monotone = true;
  double prev = 2;
  for (int i = 0; i <= 50; ++i) {
    const double v = metronomeSwing(3.0 + turn * i / 50.0, 140, true);  // beat 3 (odd): from +1 over to -1
    monotone = monotone && v <= prev + 1e-12;
    prev = v;
  }
  expect(monotone, "the turn is smooth (no overshoot, no flicker)");
  expect(kSwingSeconds * 400 / 60.0 > 0.45 && metronomeSwing(1.0 + 0.45, 400, true) == -1.0, "fast tempos: the turn takes at most 45 % of a beat");
  expect(metronomeSwing(1.5, 140, false) == 1.0 && metronomeSwing(-1, 140, true) == 1.0 && metronomeSwing(NAN, 140, true) == 1.0,
         "not swinging (off, stopped, animations off): at rest facing right");
}

// ------------------------------------------------------------------ lights without animations
void testLightsWithoutAnimations() {
  using namespace keylight;
  auto na = std::make_unique<NoteActivity>();
  auto L = std::make_unique<ChannelLights>();
  L->update(*na, 0.016f, false);
  na->on(1, 36, 1.0f);
  L->update(*na, 0.016f, false);
  expect(L->key(36) == 1.0f && L->flash(36) == flashPeak(1.0f), "animations off: a hit lights the key and the pad at once");
  for (int i = 0; i < 20; ++i) L->update(*na, 0.016f, false);
  expect(L->flash(36) == flashPeak(1.0f), "held: the pad stays lit (no fade)");
  na->off(1);
  L->update(*na, 0.016f, false);
  expect(L->key(36) == 0.0f && L->flash(36) == 0.0f, "released: key and pad go dark at once");
  na->on(2, 40, 0.5f);
  L->update(*na, 0.016f, true);
  na->off(2);
  L->update(*na, 0.016f, true);
  expect(L->key(40) > 0.0f && L->key(40) < 1.0f, "animations on: the key fades as before");
}

}  // namespace

int main() {
  testUiScale();
  testPluginKeys();
  testAutoPanPad();
  testMotion();
  testLightsWithoutAnimations();
  return finish();
}
