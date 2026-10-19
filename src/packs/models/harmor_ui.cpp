// Gridline — Resynth editor on the reference resynthesis synth's form (1024 ×
// 640 form px, keyboard row dropped):
//   top        the current part: timbre (phase, two timbre views, mix, blend),
//              sub / protection / clipping rails, part volumes, blur and
//              tremolo, prism, harmonizer, pitch, unison, two filters with
//              resonance, pluck, phaser
//   lower left global: legato / glide, strum, volumes, pitch, LFO, part
//              select + enables + mix, X / Y / Z
//   lower right page selector: ENV (articulator / shaping curve editor),
//              IMAGE (image layer view + playback; audio / picture import),
//              FX (distortion, chorus, delay, reverb, compressor), ADVANCED.
// Drawn data edits go through setPluginState (undoable, merged while dragging).
#include <cstdlib>
#include <filesystem>
#include <map>
#include "../../app/ContextMenus.h"
#include "../../engine/Sample.h"
#include "../flc_kit.h"
#include "harmor.h"
#include "models.h"

namespace gl::packs::models {

namespace {

using namespace hm;
constexpr float kW = 1024, kH = 640;

struct HmUi {
  int page = 0;    // 0 env, 1 image, 2 fx, 3 advanced
  int part = 0;
  int target = ArVol;  // < kArts: articulator; else shape kArts + s
  int curve = 0;       // 0 envelope, 1 LFO, 2.. maps
  int sel = -1;
};
HmUi& ui(const PluginTarget& t) {
  static std::map<uint64_t, HmUi> m;
  auto it = m.find(t.key());
  if (it != m.end()) return it->second;
  HmUi u;
  if (const char* e = std::getenv("GL_HARMOR_TAB")) u.page = std::clamp(std::atoi(e), 0, 3);  // dev: screenshots
  return m[t.key()] = u;
}

const skin::Skin& hmSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x34393D, 0x2A2E31, 0x484E53, 0x66C2E0, 0x8E979D, 0xD3D9DD);
    s.handle = skin::hex(0x5C6369);
    s.select = skin::hex(0x66C2E0);
    s.accent2 = skin::hex(0xF2A53A);
    s.lcdBg = skin::hex(0x1A1D20);
    s.lcdText = skin::hex(0x9FD8EA);
    s.pillBg = skin::hex(0x24282B);
    s.graphBg = skin::hex(0x15181A);
    s.graphGrid = skin::hex(0x23282B);
    s.graphGridStrong = skin::hex(0x30363A);
    s.graphCurve = skin::hex(0x66C2E0);
    return s;
  }();
  return k;
}
skin::Skin tinted(const skin::Skin& k, uint32_t fill) {
  skin::Skin s = k;
  s.fill = skin::hex(fill);
  s.fillPressed = skin::lighten(s.fill, 0.18f);
  return s;
}

uint64_t mkey(const PluginTarget& t, int what) { return (t.key() * 1099511628211ull) ^ (0x484D0000ull + static_cast<uint64_t>(what)); }
void commit(App& a, const PluginTarget& t, const PluginState& st, const PatchData& d, const char* label, int what) {
  PluginState ns = st;
  ns.data = serializePatch(d);
  if (!(ns == st)) setPluginState(a, t, ns, label, mkey(t, what));
}

// Timbre / shape bars over the first harmonics.
void harmonicBars(const flk::Ctx& c, const Curve& cv, float neutral, float x, float y, float w, float h, bool level) {
  ImDrawList* dl = c.dl;
  skin::graphWell(dl, c.F.p(x, y), c.F.p(x + w, y + h), c.K);
  const int n = 24;
  const float bw = (w - 4) / n;
  for (int k = 1; k <= n; ++k) {
    double v = shapeAtHarmonic(cv, k, kHarm, neutral);
    if (level) v = std::min(1.0, (1.0 / k) * (v / 0.25) * (v / 0.25));
    const float hh = static_cast<float>(std::clamp(level ? std::sqrt(v) : v, 0.0, 1.0)) * (h - 4);
    dl->AddRectFilled(c.F.p(x + 2 + (k - 1) * bw, y + h - 2 - hh), c.F.p(x + 2 + k * bw - 1, y + h - 2), c.K.fill);
  }
}

// Point editor for one curve. Returns true when the curve changed.
bool curveEdit(const flk::Ctx& c, const char* id, float x, float y, float w, float h, Curve& cv, bool timeAxis, float neutral, int& sel) {
  ImDrawList* dl = c.dl;
  const ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::graphWell(dl, p0, p1, c.K);
  const ImVec2 q0(p0.x + 6, p0.y + 6), q1(p1.x - 6, p1.y - 6);
  const float W = q1.x - q0.x, H = q1.y - q0.y;
  for (int i = 1; i < 4; ++i) dl->AddLine(ImVec2(q0.x, q0.y + H * i / 4), ImVec2(q1.x, q0.y + H * i / 4), c.K.graphGrid, 1);
  const double len = cv.pts.empty() ? 1.0 : std::max(1e-6, curveLength(cv));
  const double view = timeAxis ? std::max(0.05, len * 1.15) : len;
  auto sx = [&](double xv) { return q0.x + static_cast<float>(xv / view) * W; };
  auto sy = [&](double yv) { return q1.y - static_cast<float>(std::clamp(yv, 0.0, 1.0)) * H; };
  dl->AddLine(ImVec2(q0.x, sy(neutral)), ImVec2(q1.x, sy(neutral)), c.K.graphGridStrong, 1);
  ImGui::SetCursorScreenPos(q0);
  ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, W), std::max(1.0f, H)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const ImVec2 m = ImGui::GetIO().MousePos;
  std::vector<double> ax(cv.pts.size(), 0.0);
  for (size_t i = 1; i < cv.pts.size(); ++i) ax[i] = ax[i - 1] + cv.pts[i].dt;
  int hot = -1;
  for (size_t i = 0; i < cv.pts.size(); ++i)
    if (std::fabs(m.x - sx(ax[i])) < 6 && std::fabs(m.y - sy(cv.pts[i].y)) < 6) hot = static_cast<int>(i);
  bool changed = false;
  if (cv.pts.empty() && hov && ImGui::IsMouseClicked(0)) {
    cv.pts = {{0, neutral, 0, 0}, {1, neutral, 0, 0}};
    changed = true;
  }
  if (hov && ImGui::IsMouseClicked(0)) sel = hot;
  if (hov && ImGui::IsMouseDoubleClicked(0) && hot < 0 && !cv.pts.empty()) {
    const double xv = std::clamp((m.x - q0.x) / W * view, 0.0, view);
    const float yv = std::clamp((q1.y - m.y) / H, 0.0f, 1.0f);
    size_t i = 1;
    while (i < cv.pts.size() && ax[i] < xv) ++i;
    const double prevX = ax[i - 1];
    Pt p{static_cast<float>(xv - prevX), yv, 0, 0};
    if (i < cv.pts.size()) cv.pts[i].dt = static_cast<float>(std::max(0.0, ax[i] - xv));
    cv.pts.insert(cv.pts.begin() + static_cast<long>(i), p);
    sel = static_cast<int>(i);
    changed = true;
  }
  if (hov && ImGui::IsMouseClicked(1) && hot > 0 && cv.pts.size() > 2) {
    if (static_cast<size_t>(hot) + 1 < cv.pts.size()) cv.pts[static_cast<size_t>(hot) + 1].dt += cv.pts[static_cast<size_t>(hot)].dt;
    cv.pts.erase(cv.pts.begin() + hot);
    sel = -1;
    changed = true;
  }
  if (act && sel >= 0 && sel < static_cast<int>(cv.pts.size()) && ImGui::IsMouseDragging(0, 0)) {
    Pt& p = cv.pts[static_cast<size_t>(sel)];
    p.y = std::clamp((q1.y - m.y) / H, 0.0f, 1.0f);
    if (sel > 0) {
      const double prevX = ax[static_cast<size_t>(sel) - 1];
      const double nx = std::clamp((m.x - q0.x) / W * view, prevX, view);
      const double old = p.dt;
      p.dt = static_cast<float>(nx - prevX);
      if (!timeAxis && static_cast<size_t>(sel) + 1 < cv.pts.size())
        cv.pts[static_cast<size_t>(sel) + 1].dt = static_cast<float>(std::max(0.0, cv.pts[static_cast<size_t>(sel) + 1].dt + old - p.dt));
    }
    changed = true;
  }
  // Curve.
  ImVec2 prev;
  const int steps = 160;
  for (int i = 0; i <= steps; ++i) {
    const double xv = view * i / steps;
    const ImVec2 q(sx(xv), sy(cv.pts.empty() ? neutral : curveAt(cv, std::min(xv, len))));
    if (i) dl->AddLine(prev, q, c.K.graphCurve, 2);
    prev = q;
  }
  for (size_t i = 0; i < cv.pts.size(); ++i) {
    const ImVec2 q(sx(ax[i]), sy(cv.pts[i].y));
    const bool s = static_cast<int>(i) == sel;
    dl->AddCircleFilled(q, s ? 5 : 3.5f, static_cast<int>(i) == cv.sustain ? c.K.accent2C() : s ? c.K.text : c.K.graphCurve);
  }
  if (hov) c.a.setHint("Drag points · double-click: add · right-click: delete");
  return changed;
}

void topSection(App& a, const PluginTarget& t, const PluginState& st, const PatchData& d, const flk::Ctx& c, const flk::Ctx& cO,
                const flk::Ctx& cG, const flk::Ctx& cV, int p) {
  (void)a, (void)t, (void)st;
  auto P = [&](int f) { return pf(p, f); };
  // Timbre.
  flk::section(c, 4, 4, 280, 104, "TIMBRE");
  flk::wheel(c, P(PfPhase), 22, 24, 28, "Phase");
  flk::wheel(c, P(PfPhaseRand), 22, 66, 28, "Rand", skin::WheelBigLine | skin::WheelCentered);
  harmonicBars(c, d.part[static_cast<size_t>(p)].shapes[ShTimbre1], 0.25f, 60, 22, 70, 46, true);
  flk::wheel(c, P(PfTimbreMix), 140, 22, 36, "Mix");
  harmonicBars(c, d.part[static_cast<size_t>(p)].shapes[ShTimbre2], 0.25f, 186, 22, 70, 46, true);
  flk::text(c, 60, 74, "T1");
  flk::text(c, 186, 74, "T2");
  flk::menu(c, P(PfBlend), 140, 80, 120, 20);
  // Rails.
  flk::section(c, 288, 4, 238, 104, "");
  flk::menu(c, P(PfSubMode), 292, 8, 64, 16);
  flk::vsl(cO, P(PfSub1), 300, 28, 15, 56, "S1");
  flk::vsl(cO, P(PfSub3), 318, 28, 15, 56, "S3");
  flk::vsl(cO, P(PfSub4), 336, 28, 15, 56, "S5");
  flk::vsl(c, P(PfProtect), 360, 28, 15, 56, "PROT");
  flk::vsl(c, P(PfClip), 384, 28, 15, 56, "CLIP");
  flk::menu(c, P(PfClipMode), 360, 8, 62, 16);
  flk::vsl(c, P(PfFxMix), 412, 12, 15, 72, "FX");
  flk::vsl(c, P(PfVolume), 434, 12, 15, 72, "VOL");
  flk::vsl(c, P(PfEnvVolume), 456, 12, 15, 72, "ENV");
  flk::wheel(c, P(PfAutoGain), 484, 14, 28, "Auto");
  flk::chk(c, P(PfVelAttTime), 478, 58, 46, 12, "V>A");
  flk::chk(c, P(PfVelAttLen), 478, 72, 46, 12, "V>AS");
  flk::chk(c, P(PfVelRelLen), 478, 86, 46, 12, "V>R");
  // Blur + tremolo.
  flk::section(c, 4, 112, 380, 72, "BLUR · TREMOLO");
  flk::wheel(cV, P(PfBlurMix), 14, 130, 28, "Mix");
  flk::wheel(cV, P(PfTBlurAtt), 52, 128, 30, "T att");
  flk::wheel(cV, P(PfTBlurDec), 92, 128, 30, "T dec");
  flk::wheel(cV, P(PfHBlur), 134, 128, 30, "Harm");
  flk::wheel(cV, P(PfHBlurLo), 174, 128, 22, "Lo");
  flk::wheel(cV, P(PfHBlurHi), 204, 128, 22, "Hi");
  flk::wheel(cO, P(PfTremolo), 252, 126, 32, "Trem");
  flk::wheel(cO, P(PfTremHz), 294, 128, 28, "Speed");
  flk::wheel(cO, P(PfTremGap), 334, 128, 28, "Gap");
  // Prism + harmonizer.
  flk::section(c, 4, 188, 376, 70, "PRISM · HARMONIZER");
  flk::wheel(c, P(PfPrism), 16, 204, 34, "Prism", skin::WheelBigLine | skin::WheelCentered);
  flk::chk(c, P(PfPrismMul), 58, 214, 54, 12, "MULT");
  flk::wheel(cG, P(PfHzMix), 128, 200, 40, "Mix");
  flk::wheel(cG, P(PfHzWidth), 178, 204, 32, "Width");
  flk::wheel(cG, P(PfHzStrength), 220, 206, 28, "Str");
  flk::digi(c, P(PfHzShift), 262, 206, 26, 18, "SH");
  flk::digi(c, P(PfHzShiftStep), 292, 206, 26, 18);
  flk::digi(c, P(PfHzGap), 322, 206, 26, 18, "GP");
  flk::digi(c, P(PfHzGapStep), 352, 206, 24, 18);
  // Pitch.
  flk::section(c, 4, 262, 540, 70, "PITCH");
  flk::digi(c, P(PfFreqMul), 16, 280, 46, 18);
  flk::text(c, 66, 284, "/");
  flk::digi(c, P(PfFreqDiv), 76, 280, 22, 18);
  flk::text(c, 16, 304, "FREQ");
  flk::digi(c, P(PfDetMul), 116, 280, 46, 18);
  flk::text(c, 166, 284, "/");
  flk::digi(c, P(PfDetDiv), 176, 280, 22, 18);
  flk::text(c, 116, 304, "DETUNE");
  flk::wheel(c, P(PfPitchArt), 214, 274, 28, "Env rng", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, P(PfVibrato), 262, 272, 32, "Vib");
  flk::wheel(cO, P(PfVibHz), 304, 274, 28, "Speed");
  flk::chk(c, P(PfPortaLog), 346, 276, 60, 12, "LOG");
  flk::chk(c, P(PfPortaVar), 346, 292, 60, 12, "VAR");
  flk::wheel(c, P(PfPortaTime), 414, 274, 28, "Glide");
  flk::wheel(c, P(PfPortaLimit), 454, 274, 28, "Limit");
  flk::wheel(c, P(PfPredelay), 496, 274, 28, "Delay");
  // Unison.
  flk::section(c, 388, 112, 156, 146, "UNISON");
  flk::digi(cG, P(PfUniOrder), 398, 134, 24, 22);
  flk::menu(c, P(PfUniType), 398, 186, 68, 18);
  flk::chk(c, P(PfUniAlt), 398, 212, 62, 12, "ALT");
  flk::vsl(cG, P(PfUniPan), 476, 128, 17, 92, "PAN");
  flk::vsl(cG, P(PfUniPitch), 498, 128, 17, 92, "PITCH");
  flk::vsl(cG, P(PfUniPhase), 520, 128, 17, 92, "PHASE");
  // Filters.
  for (int f = 0; f < 2; ++f) {
    const float y0 = 4 + f * 128.0f;
    auto F = [&](int field) { return flt(p, f, field); };
    flk::section(c, 550, y0, 470, 124, f == 0 ? "FILTER 1" : "FILTER 2");
    flk::chk(c, F(PfF1Adapt), 556, y0 + 20, 52, 12, "ADAPT");
    flk::menu(c, F(PfF1Type), 650, y0 + 14, 108, 20);
    flk::menu(c, F(PfF1ResType), 813, y0 + 14, 108, 20);
    flk::wheel(cG, F(PfF1Env), 566, y0 + 44, 44, "Env", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(cG, F(PfF1Key), 626, y0 + 54, 28, "Key", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(cG, F(PfF1Width), 662, y0 + 52, 32, "Width");
    flk::wheel(cG, F(PfF1Cut), 706, y0 + 44, 46, "Freq");
    flk::wheel(cO, F(PfF1Res), 814, y0 + 48, 38, "Reso");
    flk::wheel(cO, F(PfF1ResWidth), 864, y0 + 52, 30, "R width");
    flk::chk(c, F(PfF1ResAdapt), 900, y0 + 88, 56, 12, "ADAPT");
    flk::wheel(cO, F(PfF1ResOfs), 932, y0 + 52, 28, "Offset", skin::WheelBigLine | skin::WheelCentered);
    if (f == 0) flk::wheel(cO, F(PfF1Osc), 972, y0 + 52, 28, "Osc");
  }
  flk::wheel(c, P(PfFMix), 772, 88, 24, "Mix", skin::WheelBigLine | skin::WheelCentered);
  // Pluck + phaser.
  flk::section(c, 550, 260, 470, 72, "PLUCK · PHASER");
  flk::wheel(cV, P(PfPluck), 566, 270, 40, "Pluck");
  flk::menu(c, P(PfPhType), 620, 268, 66, 18);
  static const char* kSc[3] = {"OCT", "HZ", "HARM"};
  flk::sw(c, P(PfPhScale), 620, 292, 66, 14, kSc, 3, true);
  flk::wheel(cG, P(PfPhMix), 700, 268, 42, "Phaser");
  flk::wheel(cG, P(PfPhWidth), 752, 272, 32, "Width");
  flk::wheel(cG, P(PfPhOfs), 794, 272, 32, "Offset");
  flk::wheel(cG, P(PfPhMotion), 836, 272, 30, "Motion", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cG, P(PfPhKey), 876, 272, 28, "Key", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, FxEqMix, 936, 268, 40, "EQ mix");
}

void globalSection(const flk::Ctx& c, const flk::Ctx& cO, HmUi& u) {
  flk::section(c, 4, 336, 380, 300, "GLOBAL");
  flk::wheel(c, GmLegatoLimit, 16, 360, 28, "Limit");
  flk::chk(c, GmLegato, 54, 362, 62, 12, "LEGATO");
  flk::chk(c, GmPorta, 54, 380, 62, 12, "GLIDE");
  flk::chk(c, GmVelPorta, 54, 398, 70, 12, "VEL>GLIDE");
  flk::menu(c, GmStrumMode, 16, 446, 70, 18);
  flk::wheel(c, GmStrumTime, 96, 440, 28, "Strum");
  flk::wheel(c, GmStrumTension, 132, 440, 28, "Tension", skin::WheelBigLine | skin::WheelCentered);
  flk::vsl(c, GmVolume, 178, 380, 18, 110, "MAIN");
  flk::vsl(c, GmPostVolume, 202, 380, 18, 110, "POST");
  flk::vsl(c, GmPitch, 226, 380, 18, 110, "PITCH");
  flk::chk(c, GmVelVol, 172, 356, 70, 12, "VEL>VOL");
  flk::wheel(cO, GmLfoAmt, 132, 372, 28, "LFO", skin::WheelBigLine | skin::WheelCentered);
  // Part select + enables + mix.
  for (int p = 0; p < 2; ++p)
    if (flk::btn(c, p == 0 ? "partA" : "partB", 266 + p * 56.0f, 356, 52, 18, p == 0 ? "PART A" : "PART B", u.part == p)) u.part = p;
  flk::chk(c, pf(0, PfEnabled), 266, 380, 52, 12, "ON");
  flk::chk(c, pf(1, PfEnabled), 322, 380, 52, 12, "ON");
  flk::hsl(c, GmPartMix, 266, 398, 108, 14);
  flk::text(c, 266, 416, "A  ·  MIX  ·  B");
  ImGui::SetCursorScreenPos(c.F.p(276, 436));
  skin::xyPad(c.a, c.t, GmModX, GmModY, c.F.p(276, 436), c.F.sz(90, 90), c.K, "X / Y");
  flk::wheel(c, GmModX, 266, 548, 24, "X");
  flk::wheel(c, GmModY, 302, 548, 24, "Y");
  flk::wheel(c, GmModZ, 338, 548, 24, "Z");
  flk::menu(c, GmMono, 16, 500, 90, 18);
  flk::digi(c, GmVoices, 120, 500, 30, 18);
  flk::text(c, 16, 524, "MONO  ·  VOICES");
}

void envPage(App& a, const PluginTarget& t, const PluginState& st, PatchData& d, const flk::Ctx& c, HmUi& u) {
  // Target list: articulators then shaping curves.
  if (flk::btn(c, "tgtPrev", 396, 342, 18, 20, "<", false)) u.target = (u.target + kArts + kShapes - 1) % (kArts + kShapes);
  if (flk::btn(c, "tgtNext", 572, 342, 18, 20, ">", false)) u.target = (u.target + 1) % (kArts + kShapes);
  const bool isShape = u.target >= kArts;
  char name[64];
  std::snprintf(name, sizeof name, "%s%s", isShape ? "SHAPE: " : "", isShape ? kShapeNames[u.target - kArts] : kArtNames[u.target]);
  flk::lcd(c, 418, 342, 150, 20, name);
  PartData& P = d.part[static_cast<size_t>(u.part)];
  Curve* cv = nullptr;
  float neutral = 0.5f;
  bool timeAxis = false;
  if (isShape) {
    cv = &P.shapes[static_cast<size_t>(u.target - kArts)];
    neutral = neutralShape(u.target - kArts);
  } else {
    ArtData& A = P.arts[static_cast<size_t>(u.target)];
    static const char* kParts[11] = {"ENV", "LFO", "KEY", "VEL", "X", "Y", "Z", "RAND", "UNI", "HELD", "XSPD"};
    for (int i = 0; i < 11; ++i)
      if (flk::btn(c, kParts[i], 395 + i * 44.0f, 574, 42, 20, kParts[i], u.curve == i)) u.curve = i, u.sel = -1;
    neutral = A.bipolar ? 0.5f : 1.0f;
    if (u.curve == 0) cv = &A.env, timeAxis = true;
    else if (u.curve == 1) cv = &A.lfo;
    else cv = &A.maps[static_cast<size_t>(u.curve - 2)];
  }
  if (curveEdit(c, "hmcurve", 395, 372, 617, 196, *cv, timeAxis, neutral, u.sel)) {
    if (!isShape && u.curve <= 1) cv->enabled = true;
    commit(a, t, st, d, "Edit curve", 1 + u.target * 16 + u.curve);
  }
  if (!isShape && u.curve <= 1) {
    bool on = cv->enabled;
    if (flk::btn(c, "curveOn", 395, 604, 70, 20, on ? "ENABLED" : "DISABLED", on)) {
      cv->enabled = !on;
      commit(a, t, st, d, "Toggle articulator", 0x7000 + u.target);
    }
    if (u.curve == 0 && u.sel >= 0 && u.sel < static_cast<int>(cv->pts.size())) {
      if (flk::btn(c, "susPt", 471, 604, 90, 20, cv->sustain == u.sel ? "SUSTAIN ON" : "SET SUSTAIN", cv->sustain == u.sel)) {
        cv->sustain = cv->sustain == u.sel ? -1 : u.sel;
        commit(a, t, st, d, "Sustain point", 0x7100 + u.target);
      }
    }
    char info[96];
    const ArtData& A = P.arts[static_cast<size_t>(u.target)];
    std::snprintf(info, sizeof info, "%s  ·  A %.2f D %.2f R %.2f  ·  LFO %.2f /s", A.bipolar ? "BIPOLAR" : "UNIPOLAR", static_cast<double>(A.att),
                  static_cast<double>(A.dec), static_cast<double>(A.rel), static_cast<double>(A.lfoSpeed));
    flk::text(c, 570, 610, info);
  } else {
    flk::text(c, 395, 610, isShape ? "SHAPING CURVE OVER THE PARTIALS (LEFT = FUNDAMENTAL)" : "MAPPING CURVE: INPUT LEFT TO RIGHT");
  }
  if (flk::btn(c, "curveReset", 940, 604, 72, 20, "RESET", false)) {
    if (isShape) *cv = Curve{};
    else if (u.curve >= 2) *cv = Curve{};
    else *cv = u.curve == 0 ? PatchData().part[0].arts[static_cast<size_t>(u.target)].env : PatchData().part[0].arts[static_cast<size_t>(u.target)].lfo;
    u.sel = -1;
    commit(a, t, st, d, "Reset curve", 0x7200 + u.target);
  }
}

void imagePage(App& a, const PluginTarget& t, const PluginState& st, PatchData& d, const flk::Ctx& c, const flk::Ctx& cO, HmUi& u) {
  const int p = u.part;
  ImageData& im = d.part[static_cast<size_t>(p)].image;
  const ImVec2 p0 = c.F.p(393, 406), p1 = c.F.p(813, 627);
  skin::graphWell(c.dl, p0, p1, c.K);
  if (!im.gain.empty() && im.cols > 0 && im.rows > 0) {
    const int vc = std::min(im.cols, 210), vr = std::min(im.rows, 110);
    const float cw = (p1.x - p0.x - 4) / vc, rh = (p1.y - p0.y - 4) / vr;
    for (int x = 0; x < vc; ++x)
      for (int y = 0; y < vr; ++y) {
        const int sc = x * im.cols / vc, sr = y * im.rows / vr;
        const float g = im.gain[static_cast<size_t>(sc) * static_cast<size_t>(im.rows) + static_cast<size_t>(sr)] / 255.0f;
        if (g < 0.02f) continue;
        c.dl->AddRectFilled(ImVec2(p0.x + 2 + x * cw, p1.y - 2 - (y + 1) * rh), ImVec2(p0.x + 2 + (x + 1) * cw, p1.y - 2 - y * rh),
                            theme::withAlpha(c.K.fill, std::sqrt(g)));
      }
  } else {
    flk::text(c, 400, 506, im.source.empty() ? "NO IMAGE  ·  LOAD AN AUDIO FILE OR A PICTURE" : "IMAGE IS ANALYSED FROM THE SOURCE FILE");
  }
  if (!im.source.empty()) flk::text(c, 400, 410, std::filesystem::path(im.source).filename().string().c_str());
  const PluginTarget tc = t;
  if (flk::btn(c, "loadAudio", 527, 378, 90, 22, "LOAD AUDIO", false))
    menus::fileDialog(a, "Resynthesize audio", false, {".wav", ".flac", ".ogg", ".mp3", ".wv"}, "", [&a, tc, p](const std::string& path) {
      const PluginState* s0 = tc.state(a.doc.song());
      if (!s0) return;
      SamplePtr smp = sampleCache().get(path);
      if (!smp) return;
      std::vector<float> mono(smp->left);
      for (size_t i = 0; i < mono.size() && i < smp->right.size(); ++i) mono[i] = 0.5f * (mono[i] + smp->right[i]);
      PatchData dd;
      parsePatch(s0->data, dd);
      ImageData out;
      if (!analyzeAudio(mono, smp->sampleRate, 255, 0.01, out)) return;
      out.source = path;
      dd.part[static_cast<size_t>(p)].image = out;
      PluginState ns = *s0;
      ns.data = serializePatch(dd);
      ns.params[static_cast<size_t>(pf(p, PfImgGainMix))] = 1;
      setPluginState(a, tc, ns, "Resynthesize audio");
    });
  if (flk::btn(c, "loadPic", 621, 378, 90, 22, "LOAD PICTURE", false))
    menus::fileDialog(a, "Load picture", false, {".bmp", ".ppm", ".pgm", ".tga"}, "", [&a, tc, p](const std::string& path) {
      const PluginState* s0 = tc.state(a.doc.song());
      if (!s0) return;
      PatchData dd;
      parsePatch(s0->data, dd);
      ImageData out;
      if (!imageFromPicture(path, 128, out)) return;
      dd.part[static_cast<size_t>(p)].image = out;
      PluginState ns = *s0;
      ns.data = serializePatch(dd);
      setPluginState(a, tc, ns, "Load picture");
    });
  if (flk::btn(c, "clearImg", 715, 378, 60, 22, "CLEAR", false) && (!im.gain.empty() || !im.source.empty())) {
    im = ImageData{};
    commit(a, t, st, d, "Clear image", 0x7300);
  }
  auto P = [&](int f) { return pf(p, f); };
  flk::wheel(cO, P(PfImgTime), 822, 452, 42, "Time");
  flk::wheel(cO, P(PfImgTimeSmooth), 830, 412, 24, "Smth");
  flk::wheel(cO, P(PfImgSpeedOct), 878, 412, 32, "Coarse", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, P(PfImgSpeed), 878, 462, 32, "Speed", skin::WheelBigLine | skin::WheelCentered);
  flk::menu(c, P(PfImgLoop), 926, 414, 86, 18);
  flk::wheel(c, P(PfImgSharp), 936, 462, 28, "Sharp", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, P(PfImgGainScale), 884, 520, 32, "Gain");
  flk::wheel(c, P(PfImgGainMix), 934, 520, 32, "Mix");
  flk::menu(c, P(PfImgFreqMode), 820, 590, 60, 18);
  flk::wheel(c, P(PfImgFreqScale), 890, 582, 30, "Freq", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, P(PfImgFormant), 934, 582, 30, "Form", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, P(PfImgFormantMix), 978, 582, 26, "F mix");
}

void fxPage(const flk::Ctx& c, const flk::Ctx& cO, const flk::Ctx& cG, const flk::Ctx& cV) {
  flk::section(c, 393, 372, 214, 132, "DRIVE");
  flk::menu(c, FxDistType, 498, 392, 100, 18);
  flk::vsl(cO, FxDistDrive, 404, 392, 17, 84, "IN");
  flk::vsl(cO, FxDistAsym, 426, 392, 17, 84, "ASYM");
  flk::vsl(cO, FxDistOut, 448, 392, 17, 84, "OUT");
  flk::vsl(cO, FxDistMix, 470, 392, 17, 84, "MIX");
  flk::wheel(cO, FxDistTone, 520, 428, 34, "Tone");
  flk::section(c, 611, 372, 404, 132, "CHORUS");
  flk::digi(cV, FxChOrder, 620, 400, 26, 22);
  flk::textC(c, 633, 426, "ORDER");
  const int ch[6] = {FxChDepth, FxChSpeed, FxChDelay, FxChSpread, FxChCross, FxChMix};
  const char* chn[6] = {"DEPTH", "SPEED", "DELAY", "SPREAD", "CROSS", "MIX"};
  for (int i = 0; i < 6; ++i) flk::vsl(cV, ch[i], 664 + i * 24.0f, 392, 17, 84, chn[i]);
  flk::section(c, 393, 508, 214, 128, "DELAY");
  flk::chk(c, FxDlOn, 400, 528, 46, 12, "ON");
  flk::menu(c, FxDlMode, 400, 546, 60, 18);
  flk::chk(c, FxDlDamp, 400, 570, 56, 12, "DAMP");
  const int dl[5] = {FxDlVol, FxDlFeedback, FxDlTime, FxDlOffset, FxDlPan};
  const char* dln[5] = {"VOL", "FB", "TIME", "OFS", "PAN"};
  for (int i = 0; i < 5; ++i) flk::vsl(cG, dl[i], 470 + i * 26.0f, 524, 17, 80, dln[i]);
  flk::section(c, 611, 508, 220, 128, "REVERB");
  flk::chk(c, FxRvOn, 618, 528, 46, 12, "ON");
  const int rv[7] = {FxRvLowCut, FxRvHighCut, FxRvPredelay, FxRvRoom, FxRvDecay, FxRvDamp, FxRvMix};
  const char* rvn[7] = {"LC", "HC", "PRE", "ROOM", "DEC", "DAMP", "WET"};
  for (int i = 0; i < 7; ++i) flk::vsl(cV, rv[i], 662 + i * 23.0f, 524, 16, 80, rvn[i]);
  flk::section(c, 835, 508, 180, 128, "COMPRESSOR");
  flk::menu(c, FxCompType, 842, 528, 76, 18);
  const int cp[4] = {FxCompAmount, FxCompLow, FxCompMid, FxCompHigh};
  const char* cpn[4] = {"AMT", "LOW", "MID", "HIGH"};
  for (int i = 0; i < 4; ++i) flk::vsl(cO, cp[i], 922 + i * 22.0f, 524, 16, 80, cpn[i]);
}

void advPage(const flk::Ctx& c, HmUi& u) {
  const int p = u.part;
  flk::section(c, 393, 372, 620, 264, "ADVANCED");
  flk::wheel(c, pf(p, PfPredelay), 410, 396, 34, "Predelay");
  flk::wheel(c, pf(p, PfRelMul), 470, 396, 34, "Poly rel");
  flk::menu(c, GmMono, 540, 400, 100, 18);
  flk::digi(c, GmVoices, 660, 400, 30, 18);
  flk::text(c, 410, 470, "PARTIAL BUDGET: 256 SINES PER PART AND VOICE (SHARED BY THE UNISON COPIES)");
  flk::text(c, 410, 488, "CURVES / IMAGES ARE STORED IN THE PATCH; AUDIO SOURCES ARE RE-ANALYSED ON LOAD");
  flk::text(c, 410, 506, "ENVELOPE TIMES IN SECONDS (TEMPO CURVES: BEATS AT 120 BPM)");
}

}  // namespace

void drawHarmor(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = hmSkin();
  static const skin::Skin kO = tinted(K, 0xF2A53A), kG = tinted(K, 0x7BCB5C), kV = tinted(K, 0xB085E0);
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl}, cO{a, t, F, kO, dl}, cG{a, t, F, kG, dl}, cV{a, t, F, kV, dl};
  ImGui::PushID("gl.harmor");
  HmUi& u = ui(t);
  PatchData d;
  parsePatch(st.data, d);
  topSection(a, t, st, d, c, cO, cG, cV, u.part);
  globalSection(c, cO, u);
  static const char* kPages[4] = {"ENV", "IMAGE", "FX", "ADV"};
  for (int i = 0; i < 4; ++i)
    if (flk::btn(c, kPages[i], 829 + i * 46.0f, 340, 44, 22, kPages[i], u.page == i)) u.page = i;
  switch (u.page) {
    case 0: envPage(a, t, st, d, c, u); break;
    case 1: imagePage(a, t, st, d, c, cO, u); break;
    case 2: fxPage(c, cO, cG, cV); break;
    default: advPage(c, u); break;
  }
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::models
