// Gridline — Slicer editor in the reference beat-slicer layout (J-2).
// Reference form 714 × 625 (four bands):
//   top (0..99): master sliders level / … / pitch (22, 46, 71, 95; 20×75),
//     X/Y modulation pad (132, 9, 77×77) + X / Y wheels (215, 33 / 62),
//     mode selector (424, 9, 132×19), options (345..563, 35..86), logo (589, 12)
//   region panel (0, 99, 714×198): region selector (14, 131), cut group (121, 157),
//     region wheels pan / volume (46 / 72, 185), cutoff / resonance (213),
//     pitch (72, 241), delay / start (46 / 72, 269), filter type (244, 183),
//     cutoff (231, 232, 46) · resonance (281, 242, 36) · amount (191, 242, 36),
//     envelope editor (338, 139, 367×153)
//   deck bar (0, 296, 714×66): transport (9 / 38 / 67, 297), tools (160..707, 298),
//     sample info (5 / 263 / 547, 345)
//   waveform with the region markers (0, 362, 714×145), region strip (0, 507, 714×56),
//   keyboard (0, 571, 714×54) with the base note selector (3, 574).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include "../engine/SlicexEngine.h"
#include "../core/PluginParams.h"
#include "../engine/Slicing.h"
#include "InstrumentSkin.h"
#include "PluginEditorsCommon.h"
#include "../core/Platform.h"

namespace gl {

bool drawSlicerEditorJ2(App& a, const PluginTarget& t, float width);

namespace {

using namespace pp;
using iskin::lerp;
using iskin::rgb;
namespace fs = std::filesystem;

constexpr float kSxW = 714, kSxH = 625;

iskin::Palette slicexPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x18232F);
  p.bgBottom = rgb(0x111A23);
  p.panel = rgb(0x41474B);
  p.panelLine = rgb(0x0E1419);
  p.rail = rgb(0x3F484D);
  p.fill = rgb(0xFFA830);
  p.fill2 = rgb(0x989EA7);
  p.fill3 = rgb(0xF5781B);
  p.cap = rgb(0x4A5257);
  p.handle = rgb(0x30373B);
  p.label = rgb(0xA7B0B9);
  p.text = rgb(0xE9EDFA);
  p.textDim = rgb(0xACB4BA);
  p.lcdBg = rgb(0x6E7186);
  p.lcdText = rgb(0xE9EDFA);
  p.led = rgb(0xFFA830);
  p.ledOff = rgb(0x2A3136);
  return p;
}

struct SlicerUi {
  int slice = 0;
  int drag = -1;   // marker being dragged (slice index)
  double viewFrom = 0, viewTo = 1;
};
std::map<uint64_t, SlicerUi> slicerUi;

// Slice starts (frames) as the engine computes them.
std::vector<int64_t> sliceStarts(const PluginState& st, const Sample* smp) {
  std::vector<int64_t> out;
  if (!smp || smp->frames() == 0) return out;
  if (ed::ival(st, SlMode) == SlModeManual) {
    for (int k = 0; k < kMaxSlices; ++k) {
      float m = ed::val(st, slMarker(k), -1);
      if (m >= 0) out.push_back(static_cast<int64_t>(m * static_cast<double>(smp->frames())));
    }
    std::sort(out.begin(), out.end());
  } else {
    static std::map<std::string, std::pair<std::pair<int, float>, std::vector<int64_t>>> cache;
    auto& c = cache[smp->path];
    std::pair<int, float> key{ed::ival(st, SlMode), ed::val(st, SlSensitivity, 0.5f)};
    if (c.first != key || c.second.empty()) c.first = key, c.second = computeSlices(*smp, key.first, key.second);
    out = c.second;
    if (out.size() > static_cast<size_t>(kMaxSlices)) out.resize(kMaxSlices);
  }
  return out;
}

// Converts the current slices to manual markers (so they can be edited).
void makeManual(App& a, const PluginTarget& t, const PluginState& st, const std::vector<int64_t>& starts, const Sample* smp) {
  if (!smp || ed::ival(st, SlMode) == SlModeManual) return;
  PluginState ns = st;
  ns.params[static_cast<size_t>(SlMode)] = SlModeManual;
  for (int k = 0; k < kMaxSlices; ++k)
    ns.params[static_cast<size_t>(slMarker(k))] =
        k < static_cast<int>(starts.size()) ? static_cast<float>(static_cast<double>(starts[static_cast<size_t>(k)]) / static_cast<double>(smp->frames())) : -1.f;
  setPluginState(a, t, ns, "edit slices by hand");
}

int markerParamAt(const PluginState& st, double pos) {
  for (int m = 0; m < kMaxSlices; ++m)
    if (std::fabs(ed::val(st, slMarker(m), -1) - pos) < 1e-6) return m;
  return -1;
}

void editorSlicex(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  SlicerUi& ui = slicerUi[t.key()];
  const std::string file = slicex::sampleOf(st.data);  // J-41: region-model data starts with the sample
  SamplePtr smp = file.empty() ? nullptr : sampleCache().get(file);
  const std::vector<int64_t> starts = sliceStarts(st, smp.get());
  const int nSl = static_cast<int>(starts.size());
  ui.slice = std::clamp(ui.slice, 0, std::max(0, std::min(nSl, kMaxSlices) - 1));
  const int k = ui.slice;
  const int base = ed::ival(st, SlBaseNote, 60);
  auto sliceKey = [&](int s) {
    int kk = ed::ival(st, slParam2(s, SliceKey), -1);
    return kk >= 0 ? kk : base + s;
  };
  iskin::Skin sk = iskin::begin(width, kSxW, kSxH, slicexPalette());
  ImDrawList* dl = sk.dl();
  iskin::background(sk, 0, 0, kSxW, kSxH, sk.c.bgTop, sk.c.bgBottom, 4);
  iskin::bevel(sk, 0, 0, kSxW, 3, rgb(0xFF8000), 0, 0);  // divider strip

  // ---- top band: master sliders, X/Y pad, options, logo.
  struct Sl {
    int param;
    float x;
    const char* cap;
    bool centered;
  };
  const Sl sliders[4] = {{SlVolume, 22, "LEVEL", false}, {SlFine, 46, "FINE", true}, {SlPan, 71, "PAN", true}, {SlPitch, 95, "PITCH", true}};
  for (const Sl& s : sliders) {
    iskin::slider(a, t, sk, s.param, s.x, 5, 20, 75, sk.c.fill, s.centered);
    iskin::text(sk, s.x - 6, 81, 32, 10, s.cap, 7.5f, sk.c.label, 1, false, true);
  }
  {  // X/Y modulation pad
    iskin::bevel(sk, 132, 9, 77, 77, rgb(0x33393E), sk.c.panelLine, 2);
    const ImVec2 p0 = sk.at(132, 9), p1 = sk.at(209, 86);
    for (int i = 1; i < 4; ++i) {
      float fx = p0.x + (p1.x - p0.x) * i / 4.0f, fy = p0.y + (p1.y - p0.y) * i / 4.0f;
      dl->AddLine(ImVec2(fx, p0.y), ImVec2(fx, p1.y), theme::withAlpha(IM_COL32_WHITE, 0.05f));
      dl->AddLine(ImVec2(p0.x, fy), ImVec2(p1.x, fy), theme::withAlpha(IM_COL32_WHITE, 0.05f));
    }
    const float mx = ed::val(st, SlModX, 0.5f), my = ed::val(st, SlModY, 0.5f);
    ImVec2 c(p0.x + (p1.x - p0.x) * mx, p1.y - (p1.y - p0.y) * my);
    dl->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, p1.y), rgb(0xB9BFC3, 0.5f));
    dl->AddLine(ImVec2(p0.x, c.y), ImVec2(p1.x, c.y), rgb(0xB9BFC3, 0.5f));
    dl->AddCircleFilled(c, sk.px(4), rgb(0xB9BFC3));
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##xy", ImVec2(p1.x - p0.x, p1.y - p0.y));
    if (ImGui::IsItemActive()) {
      ImVec2 m = ImGui::GetIO().MousePos;
      a.doc.setParam(t.pid(SlModX), std::clamp((m.x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f), true);
      a.doc.setParam(t.pid(SlModY), std::clamp((p1.y - m.y) / (p1.y - p0.y), 0.0f, 1.0f), true);
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemHovered()) a.setHint("Modulation X (filter cutoff) / Y (resonance): drag");
    iskin::wheel(a, t, sk, SlModX, 215, 33, 24, rgb(0x959EA6));
    iskin::text(sk, 241, 37, 17, 15, "X", 11, rgb(0x959EA6), 0, false, true);
    iskin::wheel(a, t, sk, SlModY, 215, 62, 24, rgb(0x959EA6));
    iskin::text(sk, 241, 67, 17, 15, "Y", 11, rgb(0x959EA6), 0, false, true);
  }
  // Slicing mode + options.
  iskin::text(sk, 345, 11, 76, 15, "SLICING", 8.5f, sk.c.label, 2, false, true);
  iskin::digi(a, t, sk, SlMode, 424, 9, 132, 19, {}, 10);
  iskin::check(a, t, sk, SlMono, 345, 33, 70, 17, "MONO", 8.5f);
  iskin::check(a, t, sk, SlDeclick, 420, 33, 70, 17, "DECLICK", 8.5f);
  iskin::check(a, t, sk, SlHold, 495, 33, 70, 17, "HOLD", 8.5f);
  iskin::check(a, t, sk, SlPlayToEnd, 345, 52, 90, 17, "PLAY TO END", 8.5f);
  iskin::check(a, t, sk, SlReverse, 440, 52, 70, 17, "REVERSE", 8.5f);
  iskin::check(a, t, sk, SlFitTempo, 345, 71, 90, 17, "FIT TEMPO", 8.5f);
  iskin::digi(a, t, sk, SlLoopBeats, 440, 71, 50, 15, [](double v) {
    char b[24];
    std::snprintf(b, sizeof b, "%.0f beats", v);
    return std::string(b);
  }, 8.5f);
  iskin::text(sk, 500, 71, 70, 15, "SENS / GAP", 7.5f, sk.c.label, 0, false, true);
  iskin::wheel(a, t, sk, SlSensitivity, 574, 62, 26, sk.c.fill);
  iskin::wheel(a, t, sk, SlMinGap, 606, 62, 26, sk.c.fill);
  iskin::wheel(a, t, sk, SlVelocity, 638, 62, 26, sk.c.fill);
  iskin::text(sk, 630, 88, 42, 9, "VEL", 7.5f, sk.c.label, 1, false, true);
  iskin::text(sk, 589, 12, 118, 40, "SLICER", 26, rgb(0xF5781B), 2, false, true);

  // ---- region panel.
  iskin::bevel(sk, 0, 99, kSxW, 198, rgb(0x60686C), 0, 0);
  iskin::bevel(sk, 10, 106, 158, 189, rgb(0x5A6166), rgb(0x4A5156), 3);
  {  // region selector: < Region n · key >
    char r[48];
    if (nSl > 0) std::snprintf(r, sizeof r, "Region %d  \xC2\xB7  %s", k + 1, ed::noteName(sliceKey(k)).c_str());
    else std::snprintf(r, sizeof r, "(no regions)");
    if (iskin::button(sk, "##rprev", 14, 109, 18, 19, "", false)) ui.slice = std::max(0, k - 1);
    iskin::glyphArrow(dl, sk.at(23, 118.5f), sk.px(5), sk.c.text, 0);
    iskin::lcd(sk, 33, 109, 114, 19, r, sk.c.lcdBg, sk.c.lcdText, 9);
    if (iskin::button(sk, "##rnext", 148, 109, 18, 19, "", false)) ui.slice = std::min(std::max(0, nSl - 1), k + 1);
    iskin::glyphArrow(dl, sk.at(157, 118.5f), sk.px(5), sk.c.text, 1);
  }
  // Region wheels on the reference rows (y 185 / 213 / 241 / 269); captions beside them.
  auto rw = [&](int param, float x, float y, const char* cap, bool capLeft) {
    iskin::wheel(a, t, sk, param, x, y, 23, sk.c.fill);
    if (capLeft) iskin::text(sk, 12, y, x - 14, 23, cap, 7.5f, rgb(0x2C3236), 2, false, true);
    else iskin::text(sk, x + 25, y, 26, 23, cap, 7.5f, rgb(0x2C3236), 0, false, true);
  };
  iskin::text(sk, 14, 134, 60, 10, "KEY", 7.5f, rgb(0x2C3236), 0, false, true);
  iskin::digi(a, t, sk, slParam2(k, SliceKey), 14, 145, 56, 20, [&](double v) {
    int n = static_cast<int>(std::lround(v));
    return n < 0 ? ed::noteName(base + k) + "*" : ed::noteName(n);
  }, 9);
  iskin::text(sk, 90, 134, 70, 10, "CUT GROUP", 7.5f, rgb(0x2C3236), 0, false, true);
  iskin::digi(a, t, sk, slParam2(k, SliceCut), 121, 145, 27, 20, [](double v) {
    int n = static_cast<int>(std::lround(v));
    return n <= 0 ? std::string("-") : std::to_string(n);
  }, 9);
  rw(slParam(k, SlicePan), 46, 185, "PAN", true);
  rw(slParam(k, SliceGain), 72, 185, "VOL", false);
  rw(slParam(k, SliceCutoff), 46, 213, "CUT", true);
  rw(slParam2(k, SliceReso), 72, 213, "RES", false);
  rw(slParam(k, SlicePitch), 72, 241, "PITCH", false);
  rw(slParam2(k, SliceDelay), 46, 269, "DELAY", true);
  rw(slParam(k, SliceStart), 72, 269, "START", false);
  iskin::check(a, t, sk, slParam(k, SliceReverse), 122, 185, 46, 20, "REV", 8);
  iskin::check(a, t, sk, slParam(k, SliceMute), 122, 207, 46, 20, "MUTE", 8);
  iskin::text(sk, 124, 232, 40, 9, "END", 7.5f, rgb(0x2C3236), 0, false, true);
  iskin::digi(a, t, sk, slParam2(k, SliceEnd), 122, 243, 44, 18, [](double v) {
    if (v < 0) return std::string("next");
    char b[16];
    std::snprintf(b, sizeof b, "%.1f%%", v * 100);
    return std::string(b);
  }, 8);
  // Filter.
  iskin::bevel(sk, 175, 131, 156, 24, rgb(0x4E555A), 0, 3);
  iskin::text(sk, 175, 131, 156, 24, "FILTER", 11, rgb(0xA7B0B9), 1, false, true);
  iskin::text(sk, 180, 183, 60, 18, "TYPE", 8, rgb(0x2C3236), 0, false, true);
  iskin::digi(a, t, sk, SlFilterType, 244, 183, 84, 18, {}, 9);
  iskin::wheel(a, t, sk, SlCutoff, 231, 222, 46, sk.c.fill2);
  iskin::text(sk, 226, 268, 56, 10, "CUTOFF", 7.5f, rgb(0x2C3236), 1, false, true);
  iskin::wheel(a, t, sk, SlReso, 281, 232, 36, sk.c.fill2);
  iskin::text(sk, 275, 268, 48, 10, "RES", 7.5f, rgb(0x2C3236), 1, false, true);
  iskin::wheel(a, t, sk, SlAttack, 185, 232, 36, sk.c.fill2);
  iskin::text(sk, 179, 268, 48, 10, "ATTACK", 7.5f, rgb(0x2C3236), 1, false, true);
  // Envelope view (amp): attack · slice · release, with fades; wheels below.
  {
    const float ex = 338, ey = 107, ew = 367, eh = 150;
    iskin::bevel(sk, ex, ey, ew, 16, rgb(0x3A4146), 0, 2);
    iskin::text(sk, ex + 4, ey, ew - 8, 16, "AMP ENVELOPE  \xC2\xB7  REGION FADES", 8.5f, sk.c.text, 0, false, true);
    iskin::bevel(sk, ex, ey + 18, ew, eh - 18 - 30, rgb(0x0E1215), sk.c.panelLine, 2);
    const ImVec2 p0 = sk.at(ex + 6, ey + 24), p1 = sk.at(ex + ew - 6, ey + eh - 36);
    const double at = ed::val(st, SlAttack, 0.001f), re = ed::val(st, SlRelease, 0.01f), fi = ed::val(st, SlFadeIn), fo = ed::val(st, SlFadeOut);
    const double body = 1.0;  // the slice
    const double span = at + body + re;
    auto X = [&](double s) { return p0.x + static_cast<float>(s / span) * (p1.x - p0.x); };
    auto Y = [&](double v) { return p1.y - static_cast<float>(v) * (p1.y - p0.y); };
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 60; ++i) {
      double u = i / 60.0 * span, v;
      if (u < at) v = u / std::max(1e-6, at);
      else if (u < at + body) {
        double rel = (u - at) / body;
        v = 1;
        if (fi > 0 && rel < fi) v *= rel / fi;
        if (fo > 0 && rel > 1 - fo) v *= (1 - rel) / fo;
      } else v = std::max(0.0, 1.0 - (u - at - body) / std::max(1e-6, re)) * (fo > 0 ? 0 : 1);
      pts.push_back(ImVec2(X(u), Y(v)));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), sk.c.fill, 0, std::max(1.2f, sk.px(1.6f)));
    dl->AddLine(ImVec2(X(at), p0.y), ImVec2(X(at), p1.y), theme::withAlpha(IM_COL32_WHITE, 0.12f));
    dl->AddLine(ImVec2(X(at + body), p0.y), ImVec2(X(at + body), p1.y), theme::withAlpha(IM_COL32_WHITE, 0.12f));
    const struct {
      int p;
      const char* c;
    } ws[4] = {{SlRelease, "RELEASE"}, {SlFadeIn, "FADE IN"}, {SlFadeOut, "FADE OUT"}, {SlChromatic, "CHROMATIC"}};
    for (int i = 0; i < 4; ++i) {
      iskin::wheel(a, t, sk, ws[i].p, ex + 10 + i * 92, ey + eh - 28, 22, sk.c.fill);
      iskin::text(sk, ex + 36 + i * 92, ey + eh - 28, 60, 22, ws[i].c, 7.5f, rgb(0x2C3236), 0, false, true);
    }
  }

  // ---- deck bar.
  iskin::bevel(sk, 0, 296, kSxW, 66, rgb(0x293136), 0, 0);
  {
    // Transport: play the selected region / stop.
    static int playing = -1;
    if (iskin::button(sk, "##play", 38, 299, 31, 30, "", playing >= 0, 9) && t.isChannel() && nSl > 0) {
      if (playing >= 0) a.engine.noteOff(t.owner, playing), playing = -1;
      else playing = sliceKey(k), a.engine.noteOn(t.owner, playing, 0.9f);
    }
    {
      ImVec2 c = sk.at(53.5f, 314);
      float r = sk.px(6);
      dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x - r * 0.6f, c.y + r), ImVec2(c.x + r, c.y), playing >= 0 ? rgb(0x293136) : sk.c.text);
    }
    if (iskin::button(sk, "##stop", 69, 299, 31, 30, "", false, 9) && playing >= 0 && t.isChannel()) a.engine.noteOff(t.owner, playing), playing = -1;
    dl->AddRectFilled(sk.at(79, 309), sk.at(89, 319), sk.c.text);
    // Load / auto-slice / even slices / manual / clear markers.
    if (iskin::button(sk, "##load", 160, 300, 50, 30, "LOAD", false, 8.5f)) {
      PluginTarget tt = t;
      a.fileDialog = {};
      a.fileDialog.open = true;
      a.fileDialog.title = "Load loop";
      const char* h = gl::platform::homeEnv();
      a.fileDialog.dir = st.data.empty() ? (h ? h : ".") : fs::path(slicex::sampleOf(st.data)).parent_path().string();
      a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3", ".ogg", ".wv"};
      a.fileDialog.onOk = [&a, tt](const std::string& p) {
        if (const PluginState* cur = tt.state(a.doc.song())) {
          PluginState ns = *cur;
          ns.data = p;
          setPluginState(a, tt, ns, "load loop");
        }
      };
    }
    struct Tool {
      const char* id;
      const char* cap;
      float x;
      int mode;
    };
    const Tool tools[5] = {{"##auto", "AUTO", 427, 0}, {"##ev8", "8", 475, 2}, {"##ev16", "16", 499, 3}, {"##ev32", "32", 523, 4}, {"##hand", "EDIT", 579, -1}};
    for (const Tool& tl : tools) {
      const bool lit = tl.mode >= 0 ? ed::ival(st, SlMode) == tl.mode : ed::ival(st, SlMode) == SlModeManual;
      if (iskin::button(sk, tl.id, tl.x, 300, tl.mode == 0 || tl.mode < 0 ? 46 : 22, 30, tl.cap, lit, 8.5f)) {
        if (tl.mode >= 0) a.doc.setParam(t.pid(SlMode), tl.mode, false);
        else makeManual(a, t, st, starts, smp.get());
      }
    }
    if (iskin::button(sk, "##clear", 627, 300, 50, 30, "CLEAR", false, 8.5f) && ed::ival(st, SlMode) == SlModeManual) {
      PluginState ns = st;
      for (int m = 0; m < kMaxSlices; ++m) ns.params[static_cast<size_t>(slMarker(m))] = m == 0 ? 0.f : -1.f;
      setPluginState(a, t, ns, "clear slice markers");
    }
    char info[96];
    std::snprintf(info, sizeof info, "%s", smp ? (std::to_string(static_cast<int>(smp->sampleRate)) + "Hz").c_str() : "--");
    iskin::text(sk, 5, 345, 70, 15, info, 8.5f, sk.c.textDim, 0);
    std::string nm = smp ? fs::path(slicex::sampleOf(st.data)).filename().string() : std::string("Drop a loop here or press LOAD");
    iskin::text(sk, 160, 345, 380, 15, nm.c_str(), 9, sk.c.text, 0);
    std::snprintf(info, sizeof info, "%d regions", nSl);
    iskin::text(sk, 547, 345, 160, 15, info, 8.5f, sk.c.textDim, 2);
  }

  // ---- waveform with markers.
  {
    const float wx = 0, wy = 362, ww = kSxW, wh = 145;
    iskin::bevel(sk, wx, wy, ww, wh, rgb(0x0B1014), 0, 0);
    const ImVec2 p0 = sk.at(wx + 2, wy + 14), p1 = sk.at(wx + ww - 2, wy + wh - 2);
    const double frames = smp ? static_cast<double>(smp->frames()) : 1.0;
    auto fx = [&](double f) { return p0.x + static_cast<float>(f / frames) * (p1.x - p0.x); };
    for (int s = 0; s < nSl; ++s) {  // region shading
      double s0 = static_cast<double>(starts[static_cast<size_t>(s)]);
      double s1 = s + 1 < nSl ? static_cast<double>(starts[static_cast<size_t>(s + 1)]) : frames;
      float e = ed::val(st, slParam2(s, SliceEnd), -1);
      if (e >= 0) s1 = std::min(s1, e * frames);
      ImU32 c = s == k ? rgb(0x3A2A12) : (s % 2 ? rgb(0x141C24) : rgb(0x101820));
      dl->AddRectFilled(ImVec2(fx(s0), sk.at(0, wy).y), ImVec2(fx(s1), p1.y), c);
      char lbl[16];
      std::snprintf(lbl, sizeof lbl, "%d", s + 1);
      dl->AddText(a.fontMono, std::max(8.0f, sk.px(9)), ImVec2(fx(s0) + 3, sk.at(0, wy + 1).y), s == k ? sk.c.fill : sk.c.textDim, lbl);
    }
    iskin::wave(dl, smp.get(), p0, p1, rgb(0x7FA6C7), 0, 1, theme::withAlpha(IM_COL32_WHITE, 0.08f));
    const bool manual = ed::ival(st, SlMode) == SlModeManual;
    ImGui::SetCursorScreenPos(sk.at(wx, wy));
    ImGui::InvisibleButton("##wave", ImVec2(sk.px(ww), sk.px(wh)));
    const bool hov = ImGui::IsItemHovered();
    ImVec2 m = ImGui::GetIO().MousePos;
    int hoverMarker = -1;
    for (int s = 0; s < nSl; ++s) {
      float x = fx(static_cast<double>(starts[static_cast<size_t>(s)]));
      bool near = hov && std::fabs(m.x - x) < 4 && s > 0;
      if (near) hoverMarker = s;
      dl->AddLine(ImVec2(x, sk.at(0, wy).y), ImVec2(x, p1.y), s == k ? sk.c.fill : near ? sk.c.text : rgb(0xF5781B, 0.7f), near ? 2.0f : 1.0f);
      dl->AddTriangleFilled(ImVec2(x, sk.at(0, wy).y), ImVec2(x + sk.px(6), sk.at(0, wy).y), ImVec2(x, sk.at(0, wy + 6).y), rgb(0xF5781B));
    }
    if (ImGui::IsItemActivated() && smp) {
      ui.drag = hoverMarker;
      if (ui.drag < 0) {
        double f = (m.x - p0.x) / (p1.x - p0.x) * frames;
        int s = 0;
        while (s + 1 < nSl && static_cast<double>(starts[static_cast<size_t>(s + 1)]) <= f) ++s;
        ui.slice = s;
        if (t.isChannel() && nSl > 0) a.engine.noteOn(t.owner, sliceKey(s), 0.9f), a.engine.noteOff(t.owner, sliceKey(s));
      } else if (!manual) {
        makeManual(a, t, st, starts, smp.get());
      }
    }
    if (ui.drag > 0 && manual && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1) && ui.drag < nSl) {
      int mp = markerParamAt(st, static_cast<double>(starts[static_cast<size_t>(ui.drag)]) / frames);
      if (mp < 0) {  // positions are stored as fractions: match the nearest marker
        double best = 1e9;
        for (int q = 0; q < kMaxSlices; ++q) {
          double v = ed::val(st, slMarker(q), -1);
          double d = std::fabs(v - static_cast<double>(starts[static_cast<size_t>(ui.drag)]) / frames);
          if (v >= 0 && d < best) best = d, mp = q;
        }
      }
      if (mp >= 0) a.doc.setParam(t.pid(slMarker(mp)), std::clamp((m.x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f), true);
    }
    if (ImGui::IsItemDeactivated()) {
      ui.drag = -1;
      a.doc.endGesture();
    }
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && smp) {
      if (!manual) makeManual(a, t, st, starts, smp.get());
      for (int q = 0; q < kMaxSlices; ++q)
        if (ed::val(st, slMarker(q), -1) < 0 || (!manual && q >= nSl)) {
          a.doc.setParam(t.pid(slMarker(q)), std::clamp((m.x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f), false);
          break;
        }
    }
    if (hoverMarker > 0 && manual && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      int mp = -1;
      double best = 1e9;
      for (int q = 0; q < kMaxSlices; ++q) {
        double v = ed::val(st, slMarker(q), -1);
        double d = std::fabs(v - static_cast<double>(starts[static_cast<size_t>(hoverMarker)]) / frames);
        if (v >= 0 && d < best) best = d, mp = q;
      }
      if (mp >= 0) a.doc.setParam(t.pid(slMarker(mp)), -1, false);
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
        std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        if (isAudioFile(path)) {
          PluginState ns = st;
          ns.data = path;
          setPluginState(a, t, ns, "load loop");
        }
      }
      ImGui::EndDragDropTarget();
    }
    if (hov)
      a.setHint(std::to_string(nSl) + " regions · click: select + play · drag a marker · double-click: add marker · right-click a marker: remove · drop a loop");
  }
  // ---- region strip: one block per region with its key.
  {
    iskin::bevel(sk, 0, 507, kSxW, 56, rgb(0x161E26), 0, 0);
    const int shown = std::max(1, nSl);
    const float bw = (kSxW - 8) / static_cast<float>(shown);
    for (int s = 0; s < nSl; ++s) {
      const float x = 4 + s * bw;
      ImGui::PushID(s);
      char cap[24];
      std::snprintf(cap, sizeof cap, "%s", ed::noteName(sliceKey(s)).c_str());
      const bool muted = ed::bval(st, slParam(s, SliceMute));
      if (iskin::button(sk, "##rg", x + 1, 511, bw - 2, 22, bw > 26 ? cap : "", s == k, 8, muted ? rgb(0x6A3030) : 0)) ui.slice = s;
      char num[32];
      std::snprintf(num, sizeof num, "%d", s + 1);
      if (bw > 14) iskin::text(sk, x, 536, bw, 12, num, 7.5f, s == k ? sk.c.fill : sk.c.textDim, 1, true, false);
      ImGui::PopID();
    }
    if (nSl == 0) iskin::text(sk, 0, 507, kSxW, 56, "No regions", 9, sk.c.textDim, 1);
  }
  // ---- keyboard with the base note selector.
  {
    iskin::bevel(sk, 0, 571, kSxW, 54, rgb(0x12191E), 0, 0);
    iskin::digi(a, t, sk, SlBaseNote, 3, 574, 22, 48, [](double v) { return ed::noteName(static_cast<int>(std::lround(v))); }, 7.5f);
    bool lit[128] = {};
    for (int s = 0; s < nSl; ++s) {
      int kk = sliceKey(s);
      if (kk >= 0 && kk < 128) lit[kk] = true;
    }
    iskin::KeyColors kc;
    kc.white = rgb(0xD9DEE2), kc.black = rgb(0x242C32), kc.pressed = rgb(0xFFA830), kc.mark = rgb(0xF5781B), kc.sep = rgb(0x0E1419);
    const int low = std::max(0, (base / 12) * 12 - 12);
    int clicked = iskin::keyboard(a, t, sk, 27, 571, 676, 54, low, 35, kc, sliceKey(k), lit);
    if (clicked >= 0)
      for (int s = 0; s < nSl; ++s)
        if (sliceKey(s) == clicked) ui.slice = s;
  }
  iskin::end(sk, kSxH);
}

}  // namespace

bool drawSlicerEditorJ2(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->type != "gl.slicer") return false;
  editorSlicex(a, t, width);
  return true;
}

}  // namespace gl
