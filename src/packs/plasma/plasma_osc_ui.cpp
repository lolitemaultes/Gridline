// Gridline — "plasma" pack: the OSC page (y 68..388): SUB | OSC A | OSC B |
// OSC C | NOISE | FILTER 1 / 2. Each oscillator column has a header (power,
// name, type, output route), a display (table / sample / grain / spectrum
// view, or the unison or options panel) and two knob rows.
#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include "../../core/I18n.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {

constexpr float kHeadY = 70, kHeadHgt = 20, kDispY = 90, kDispB = 265, kRow1 = 292, kRow2 = 347, kKnobR = 15.0f;
constexpr float kColH = kLowerY - kPageY, kHeadB = kHeadY + kHeadHgt - kPageY;  // frame height, header bottom (frame-relative)
constexpr float kColX[] = {0, 76, 345, 614, 884, 986, 1190};  // SUB, A, B, C, NOISE, FILTER, end
const char* const kOscName[kOscs] = {"OSC A", "OSC B", "OSC C"};

// ------------------------------------------------------------------ routing button
// Output of a source: F1 / F2 / F1+2 (filter balance), MAIN, DIRECT, OFF.
std::string routeLabel(const Ctx& c, int r) {
  const int d = vali(c, routeP(r, R_Dest));
  if (d == RouteFilter) {
    const double b = val(c, routeP(r, R_Balance));
    return b <= -0.99 ? "F1" : b >= 0.99 ? "F2" : "F1+2";
  }
  return d == RouteMain ? "M" : d == RouteDirect ? "D" : "\xE2\x80\x94";
}
void routeButton(Ctx& c, int r, float x, float y, float w) {
  ImGui::PushID(r + 500);
  const std::string lab = routeLabel(c, r);
  if (button(c, "rt", x, y + 3, w, kHeadHgt - 5, lab.c_str(), false, 8.0f)) ImGui::OpenPopup("##route");
  if (ImGui::IsItemHovered())
    c.a.setHint("Output: F1 / F2 / F1+2 = into the filters, M = main, D = direct out (click to choose; the mix page has the balance)");
  const int d = vali(c, routeP(r, R_Dest));
  if (d == RouteFilter) {  // filter balance bar over the button
    const double b = (val(c, routeP(r, R_Balance)) + 1.0) * 0.5;
    const float bx = x + 2 + static_cast<float>(b) * (w - 4 - 6);
    c.dl->AddRectFilled(c.p(bx, y + 0.5f), c.p(bx + 6, y + 2.5f), c.P.mod);
  }
  if (ImGui::BeginPopup("##route")) {
    auto item = [&](const char* name, int dest, double bal) {
      const bool sel = d == dest && (dest != RouteFilter || std::fabs(val(c, routeP(r, R_Balance)) - bal) < 0.01);
      if (ImGui::MenuItem(name, nullptr, sel)) {
        setv(c, routeP(r, R_Dest), dest);
        if (dest == RouteFilter) setv(c, routeP(r, R_Balance), bal);
      }
    };
    item(i18n::tr("Filter 1"), RouteFilter, -1);
    item(i18n::tr("Filter 2"), RouteFilter, 1);
    item(i18n::tr("Filter 1 + 2"), RouteFilter, 0);
    ImGui::Separator();
    item(i18n::tr("Main"), RouteMain, 0);
    item(i18n::tr("Direct out"), RouteDirect, 0);
    item(i18n::tr("Off"), RouteNone, 0);
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

// Darkens a column whose module is off.
void dimIfOff(Ctx& c, float x0, float x1, int enable) {
  if (val(c, enable) >= 0.5) return;
  c.dl->AddRectFilled(c.p(x0 + kGutter + 1, kDispY), c.p(x1 - kGutter - 1, kLowerY - kGutter - 1), theme::withAlpha(c.P.frame, 0.42f), c.s(2.0f),
                      ImDrawFlags_RoundCornersBottom);
}

// ------------------------------------------------------------------ waveform displays
struct CycleCache {
  uint64_t key = 0;
  std::vector<float> y;
};
uint64_t hashRange(const std::vector<float>& p, int a, int b, uint64_t h) {
  for (int k = a; k < b && k < static_cast<int>(p.size()); ++k) {
    uint32_t u;
    std::memcpy(&u, &p[static_cast<size_t>(k)], 4);
    h = (h ^ u) * 0x100000001B3ull;
  }
  return h;
}

// One warped cycle of a wavetable oscillator at table position pos (runs the
// real oscillator, so the display shows every warp).
const std::vector<float>& cycleOf(const Ctx& c, const Aux& aux, int o, float pos) {
  static std::map<uint64_t, CycleCache> cache;
  CycleCache& e = cache[c.t.key() * 4 + static_cast<uint64_t>(o)];
  uint64_t h = hashRange(c.st.params, oscP(o, 0), oscP(o, O_Count), 0xcbf29ce484222325ull);
  h = (h ^ static_cast<uint64_t>(std::lround(pos * 4096))) * 0x100000001B3ull;
  h = (h ^ reinterpret_cast<uintptr_t>(&aux)) * 0x100000001B3ull;
  h = (h ^ static_cast<uint64_t>(val(c, G_S1Mode) >= 0.5)) * 0x100000001B3ull;
  if (e.key == h && !e.y.empty()) return e.y;
  e.key = h;
  constexpr int N = 1024;
  OscSet s;
  s.on = true;
  s.type = OscWavetable;
  s.src = &aux.osc[static_cast<size_t>(o)];
  s.mip = s.src->mip.get();
  if (!s.mip) {
    const auto& fm = factoryMips();
    s.mip = fm[static_cast<size_t>(std::clamp(vali(c, oscP(o, O_Table)) - 1, 0, static_cast<int>(fm.size()) - 1))].get();
  }
  s.srv = N;
  s.hz = 1;
  s.pitch = 60;
  s.unison = 1;
  s.inc[0] = 1.0 / N;
  s.pl[0] = s.pr[0] = 1;
  s.wtPos = pos;
  s.w1 = vali(c, oscP(o, O_Warp1Mode));
  s.w2 = vali(c, oscP(o, O_Warp2Mode));
  if (!warpAllowed(OscWavetable, s.w1)) s.w1 = 0;
  if (!warpAllowed(OscWavetable, s.w2)) s.w2 = 0;
  s.a1 = valf(c, oscP(o, O_Warp1));
  s.a2 = valf(c, oscP(o, O_Warp2));
  s.v1 = valf(c, oscP(o, O_Warp1Var));
  s.v2 = valf(c, oscP(o, O_Warp2Var));
  s.smooth = val(c, oscP(o, O_Smooth)) >= 0.5;
  s.s1 = val(c, G_S1Mode) >= 0.5;
  OscVoice v;
  v.start(s, 0.f, 0.f, 0.f, 0, nullptr, 1u, 60, 0.8f);
  const float fm[6] = {};
  e.y.assign(N, 0.f);
  float l = 0, r = 0, mono = 0;
  for (int i = 0; i < N * 2; ++i) {  // second cycle (filters / feedback settled)
    v.tick(s, fm, l, r, mono);
    if (i >= N) e.y[static_cast<size_t>(i - N)] = (l + r) * 0.5f;
  }
  float pk = 0;
  for (float q : e.y) pk = std::max(pk, std::fabs(q));
  if (pk > 1.0f)
    for (float& q : e.y) q /= pk;
  return e.y;
}

// Min / max overview of a sample (cached per sample).
const std::vector<std::pair<float, float>>& overview(const Sample* smp, const float* data, size_t n, int bins) {
  static std::map<std::pair<const void*, int>, std::vector<std::pair<float, float>>> cache;
  auto& v = cache[{smp ? static_cast<const void*>(smp) : static_cast<const void*>(data), bins}];
  if (!v.empty() || n == 0) return v;
  v.resize(static_cast<size_t>(bins));
  for (int b = 0; b < bins; ++b) {
    const size_t a = n * static_cast<size_t>(b) / static_cast<size_t>(bins), e = std::max(a + 1, n * static_cast<size_t>(b + 1) / static_cast<size_t>(bins));
    float lo = 0, hi = 0;
    for (size_t i = a; i < e && i < n; ++i) lo = std::min(lo, data[i]), hi = std::max(hi, data[i]);
    v[static_cast<size_t>(b)] = {lo, hi};
  }
  return v;
}
void drawOverview(const Ctx& c, const std::vector<std::pair<float, float>>& ov, float x, float y, float w, float h, ImU32 col) {
  if (ov.empty()) return;
  const float cy = y + h * 0.5f;
  const int n = static_cast<int>(ov.size());
  for (int i = 0; i < n; ++i) {
    const float px = x + w * (i + 0.5f) / n;
    const ImVec2 a = c.p(px, cy - ov[static_cast<size_t>(i)].second * h * 0.46f), b = c.p(px, cy - ov[static_cast<size_t>(i)].first * h * 0.46f);
    c.dl->AddLine(a, ImVec2(b.x, std::max(b.y, a.y + 1)), col, std::max(1.0f, c.s(w / n)));
  }
}

// The display body of a column (between the header strip and the knob deck).
void displayWell(const Ctx& c, float x, float y, float w, float h) {
  const float g = kGutter + 1;
  const ImVec2 a = c.p(x + g, y), b = c.p(x + w - g, y + h);
  skin::gradientRect(c.dl, a, b, c.P.wellHi, c.P.well);
  gridLines(c, x + g, y, w - 2 * g, h, 4, 2, true);
}

void drawCycle(const Ctx& c, const std::vector<float>& y, float x, float y0, float w, float h, ImU32 col) {
  const int n = std::max(64, static_cast<int>(c.s(w)));
  std::vector<ImVec2> pts(static_cast<size_t>(n + 1));
  for (int i = 0; i <= n; ++i) {
    const size_t k = std::min(y.size() - 1, y.size() * static_cast<size_t>(i) / static_cast<size_t>(n));
    pts[static_cast<size_t>(i)] = c.p(x + w * i / n, y0 + h * 0.5f - std::clamp(y[k], -1.f, 1.f) * h * 0.44f);
  }
  skin::curveArea(c.dl, pts.data(), static_cast<int>(pts.size()), c.p(0, y0 + h * 0.5f).y, col, std::max(1.6f, c.s(1.7f)), 0.2f);
}

// ------------------------------------------------------------------ table / source menu
void tableMenu(Ctx& c, int o) {
  const int type = vali(c, oscP(o, O_Type));
  const auto& names = factoryTableNames();
  const int cur = vali(c, oscP(o, O_Table));
  if (type == OscMultisample) {
    if (ImGui::MenuItem(i18n::tr("Load multisample (.sfz)...")))
      openFile(c, i18n::tr("Load multisample"), {".sfz"}, [o](App& a, const PluginTarget& t, const std::string& p) {
        editState(a, t, "Load multisample", [&](PluginState&, Model& m) { m.osc[static_cast<size_t>(o)].sfz = p; });
      });
    if (!c.m.osc[static_cast<size_t>(o)].sfz.empty() && ImGui::MenuItem(i18n::tr("Clear"))) {
      c.m.osc[static_cast<size_t>(o)].sfz.clear();
      commit(c, "Clear multisample");
    }
    return;
  }
  if (type != OscWavetable) {
    if (ImGui::MenuItem(i18n::tr("Load sample...")))
      openFile(c, i18n::tr("Load sample"), {".wav", ".flac", ".aif", ".aiff", ".mp3", ".ogg"},
               [o](App& a, const PluginTarget& t, const std::string& p) {
                 editState(a, t, "Load sample", [&](PluginState&, Model& m) { m.osc[static_cast<size_t>(o)].sample = p; });
               });
    if (!c.m.osc[static_cast<size_t>(o)].sample.empty() && ImGui::MenuItem(i18n::tr("Clear (use the wavetable)"))) {
      c.m.osc[static_cast<size_t>(o)].sample.clear();
      commit(c, "Clear sample");
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr("Without a sample the wavetable plays:"));
  }
  if (ImGui::BeginMenu(i18n::tr("Factory tables"))) {
    for (int i = 0; i < static_cast<int>(names.size()); ++i)
      if (ImGui::MenuItem(names[static_cast<size_t>(i)].c_str(), nullptr, cur == i + 1 && c.m.osc[static_cast<size_t>(o)].table.empty())) {
        c.m.osc[static_cast<size_t>(o)].table.clear();
        c.m.osc[static_cast<size_t>(o)].frames.clear();
        commit(c, "Choose table");
        setv(c, oscP(o, O_Table), i + 1);
      }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Load wavetable (.wav)...")))
    openFile(c, i18n::tr("Load wavetable"), {".wav"}, [o](App& a, const PluginTarget& t, const std::string& p) {
      editState(a, t, "Load wavetable", [&](PluginState& st, Model& m) {
        auto& d = m.osc[static_cast<size_t>(o)];
        d.table = "file:" + p;
        d.tableName = fs_stem(p);
        d.frames.clear();
        st.params[static_cast<size_t>(oscP(o, O_Table))] = 0;
      });
    });
  if (ImGui::MenuItem(i18n::tr("Edit wavetable..."))) c.u.wtEdit = true, c.u.wtOsc = o, c.u.clip = false;
}

// ------------------------------------------------------------------ panels inside the display
void unisonPanel(Ctx& c, int o, float x, float y, float w) {
  const float cw = (w - 16) / 2;
  textC(c, x + w * 0.5f, y + 4, "UNISON", c.P.textDim, 9, true);
  const float rx = x + 8;
  float ry = y + 22;
  auto pair = [&](int ka, const char* la, int kb, const char* lb, bool choiceA = false, bool choiceB = false) {
    if (choiceA) {
      text(c, rx, ry + 4, la, c.P.textFaint, 8, true);
      choiceBox(c, ka, rx + 34, ry, cw - 38, 17);
    } else {
      valueBox(c, ka, rx, ry, cw - 4, 17, la);
    }
    if (kb >= 0) {
      if (choiceB) {
        text(c, rx + cw + 4, ry + 4, lb, c.P.textFaint, 8, true);
        choiceBox(c, kb, rx + cw + 40, ry, cw - 44, 17);
      } else {
        valueBox(c, kb, rx + cw + 4, ry, cw - 4, 17, lb);
      }
    }
    ry += 21;
  };
  pair(oscP(o, O_DetuneMode), "MODE", oscP(o, O_UniStack), "STACK", true, true);
  pair(oscP(o, O_UniWidth), "WIDTH", oscP(o, O_UniRange), "RANGE");
  pair(oscP(o, O_UniWtPos), "WT POS", oscP(o, O_UniSpan), "SPAN");
  pair(oscP(o, O_UniWarp1), "WARP 1", oscP(o, O_UniWarp2), "WARP 2");
  pair(oscP(o, O_UniRotate), "ROTATE", oscP(o, O_UniRandStart), "RND START");
  if (vali(c, oscP(o, O_Type)) == OscGranular) pair(oscP(o, O_GTrig), "GRAINS", -1, nullptr, true);
}

// Every remaining setting of the oscillator type, as a grid of value boxes.
void optionsPanel(Ctx& c, int o, float x, float y, float w, float h) {
  const int type = vali(c, oscP(o, O_Type));
  std::vector<int> ks = {O_PitchTrack, O_PitchMode, O_Ratio, O_HzOffset, O_PhaseMemory, O_Smooth, O_KeyMin, O_KeyMax, O_VelMin, O_VelMax};
  auto front = [&ks](std::initializer_list<int> l) {  // (no vector::insert: GCC 11 misreads it as an overflow)
    std::vector<int> n(l);
    for (const int k : ks) n.push_back(k);
    ks.swap(n);
  };
  if (type == OscSample || type == OscGranular || type == OscSpectral) {
    front({O_Start, O_End, O_Reverse, O_ScanSync, O_ScanBpm, O_ScanKey, O_LoopXfade, O_RelLoop, O_Slice, O_SliceMode});
  }
  if (type == OscGranular) {
    front({O_GDensMode, O_GLenMode, O_GMaxGrains, O_GReverse, O_GWinShape, O_GWinSkew, O_GWinParam, O_GRndWin, O_GRndSkew,
           O_GRndWarp, O_GRndWarp2});
  }
  if (type == OscSpectral) {
    front({O_SLo, O_SHi, O_SPhaseLock, O_STransients});
  }
  if (type == OscMultisample) {
    front({O_MEnvOn, O_MDelay, O_MAttack, O_MHold, O_MDecay, O_MSustain, O_MRelease});
  }
  // scroll when it does not fit
  const float rowH = 19, colW = (w - 12) / 2;
  const int rows = (static_cast<int>(ks.size()) + 1) / 2;
  const float total = rows * rowH;
  static std::map<uint64_t, float> scroll;
  float& sc = scroll[c.t.key() * 4 + static_cast<uint64_t>(o)];
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  if (ImGui::IsMouseHoveringRect(a, b) && ImGui::GetIO().MouseWheel != 0) sc -= ImGui::GetIO().MouseWheel * 24.f;
  sc = std::clamp(sc, 0.f, std::max(0.f, total - h + 6));
  c.dl->PushClipRect(a, b, true);
  for (size_t i = 0; i < ks.size(); ++i) {
    const float bx = x + 4 + (i % 2) * (colW + 4), by = y + 4 + (i / 2) * rowH - sc;
    if (by < y - rowH || by > y + h) continue;
    const int k = oscP(o, ks[i]);
    const std::string nm = paramShortName(k);
    valueBox(c, k, bx, by, colW, 16, nm.c_str());
  }
  c.dl->PopClipRect();
  if (total > h) {  // scroll bar
    const float th = h * h / total, ty = y + (h - th) * (sc / std::max(1.f, total - h + 6));
    c.dl->AddRectFilled(c.p(x + w - 3, ty), c.p(x + w - 1, ty + th), c.P.lineHi, 1);
  }
}

// ------------------------------------------------------------------ oscillator column
void warpBlock(Ctx& c, int o, float x, float y, float w) {
  // two mode fields (name + < var >), WARP 1 / 2 knobs either side
  const float half = w / 2;
  for (int i = 0; i < 2; ++i) {
    const int km = oscP(o, i == 0 ? O_Warp1Mode : O_Warp2Mode), kv = oscP(o, i == 0 ? O_Warp1Var : O_Warp2Var);
    const float bx = x + half * i;
    ImGui::PushID(i + 40);
    const int mode = vali(c, km);
    const auto& names = warpNames();
    const std::string nm = mode <= 0 || mode >= static_cast<int>(names.size()) ? std::string("OFF") : names[static_cast<size_t>(mode)];
    panelRect(c, bx + 0.5f, y, half - 1, 34, c.P.well, 2);
    ImGui::SetCursorScreenPos(c.p(bx, y));
    if (ImGui::InvisibleButton("##wm", c.F.sz(half - 1, 17))) ImGui::OpenPopup("##warp");
    const bool hv = ImGui::IsItemHovered();
    if (hv) c.dl->AddRectFilled(c.p(bx + 0.5f, y), c.p(bx + half - 0.5f, y + 17), c.P.panelHi, c.s(2));
    textMid(c, bx + 2, bx + half - 2, y + 9, nm.c_str(), mode > 0 ? c.P.accent : c.P.textFaint, 8.0f, true);
    if (hv) c.a.setHint(std::string("Warp ") + (i == 0 ? "1" : "2") + ": " + nm + " (click: choose)");
    if (ImGui::BeginPopup("##warp")) {
      warpMenuItems(c, o, i);
      ImGui::EndPopup();
    }
    // < var >
    for (int d = 0; d < 2; ++d) {
      ImGui::PushID(d);
      const float ax = d == 0 ? bx + 2 : bx + half - 15;
      ImGui::SetCursorScreenPos(c.p(ax, y + 18));
      if (ImGui::InvisibleButton("##st", c.F.sz(13, 15))) {
        int nmde = mode;
        for (int q = 0; q < static_cast<int>(names.size()); ++q) {
          nmde = (nmde + (d == 0 ? -1 : 1) + static_cast<int>(names.size())) % static_cast<int>(names.size());
          if (warpAllowed(vali(c, oscP(o, O_Type)), nmde)) break;
        }
        setv(c, km, nmde);
      }
      iconArrow(c.dl, c.p(ax + 6.5f, y + 25.5f), c.s(6), d == 0 ? 0 : 1, ImGui::IsItemHovered() ? c.P.text : c.P.textDim);
      ImGui::PopID();
    }
    if (mode > 0) valueBox(c, kv, bx + 17, y + 18, half - 34, 15, "", nullptr);
    ImGui::PopID();
  }
}

void oscColumn(Ctx& c, int o, const Aux* aux) {
  const float x0 = kColX[1 + o], x1 = kColX[2 + o], w = x1 - x0;
  const int type = vali(c, oscP(o, O_Type));
  ImGui::PushID(o + 300);
  // header: power, name, type, route
  moduleFrame(c, x0, kPageY, w, kColH, kHeadB, kOscName[o], oscP(o, O_Enable));
  {
    const float tx = x0 + 62, tw = w - 62 - 32;
    ImGui::SetCursorScreenPos(c.p(tx, kHeadY));
    if (ImGui::InvisibleButton("##ty", c.F.sz(tw, kHeadHgt))) ImGui::OpenPopup("##otype");
    const bool hv = ImGui::IsItemHovered();
    vline(c, tx, kHeadY + 4, kHeadY + kHeadHgt - 3, c.P.line);
    const ParamDesc* d = pdesc(oscP(o, O_Type));
    std::string tn = d && type >= 0 && type < static_cast<int>(d->choices.size()) ? d->choices[static_cast<size_t>(type)] : "?";
    for (auto& ch : tn) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    textMid(c, tx, tx + tw - 14, kHeadY + kHeadHgt * 0.5f, tn.c_str(), hv ? c.P.text : c.P.textDim, 8.5f, true);
    iconArrow(c.dl, c.p(tx + tw - 9, kHeadY + kHeadHgt * 0.5f), c.s(6), 3, c.P.textDim);
    if (hv) c.a.setHint("Oscillator type: wavetable, multisample, sample, granular or spectral");
    if (ImGui::BeginPopup("##otype")) {
      if (d)
        for (int i = 0; i < static_cast<int>(d->choices.size()); ++i)
          if (ImGui::MenuItem(d->choices[static_cast<size_t>(i)].c_str(), nullptr, i == type)) setv(c, oscP(o, O_Type), i);
      ImGui::EndPopup();
    }
    routeButton(c, o, x1 - 31, kHeadY, 26);
  }
  // display
  displayWell(c, x0, kDispY, w, kDispB - kDispY);
  const OscAux* oa = aux ? &aux->osc[static_cast<size_t>(o)] : nullptr;
  const float liveP = c.hasLive && c.live.voices > 0 && c.live.oscOn[o] ? c.live.oscPos[o] : -1.f;
  // source selector row
  {
    std::string nm;
    const auto& d = c.m.osc[static_cast<size_t>(o)];
    if (type == OscMultisample) nm = d.sfz.empty() ? std::string("\xE2\x80\x94") : refStem(c.m, d.sfz);
    else if (type != OscWavetable && !d.sample.empty()) nm = refStem(c.m, d.sample);
    else if (!d.table.empty()) nm = d.tableName.empty() ? std::string("Custom") : d.tableName;
    else {
      const int t = vali(c, oscP(o, O_Table));
      const auto& n = factoryTableNames();
      nm = t >= 1 && t <= static_cast<int>(n.size()) ? n[static_cast<size_t>(t - 1)] : "\xE2\x80\x94";
    }
    int step = 0;
    dropField(c, "tbl", x0 + 24, kDispY + 3, w - 48, 18, nm.c_str(), [&] { tableMenu(c, o); }, true, &step);
    if (step && type == OscWavetable) {
      const int n = static_cast<int>(factoryTableNames().size());
      const int t = vali(c, oscP(o, O_Table));
      if (!c.m.osc[static_cast<size_t>(o)].table.empty()) {
        c.m.osc[static_cast<size_t>(o)].table.clear();
        commit(c, "Choose table");
      }
      setv(c, oscP(o, O_Table), ((std::max(1, t) - 1 + step + n) % n) + 1);
    }
  }
  // OCT / SEM / FIN / CRS
  {
    const float bw = (w - 10) / 4;
    const int ks[4] = {O_Octave, O_Semi, O_Fine, O_Coarse};
    const char* ls[4] = {"OCT", "SEM", "FIN", "CRS"};
    for (int i = 0; i < 4; ++i) valueBox(c, oscP(o, ks[i]), x0 + 5 + bw * i, kDispY + 25, bw - 3, 17, ls[i]);
  }
  const float gy = kDispY + 46, gh = kDispB - 22 - gy;
  const int panel = c.u.oscPanel[o];
  if (panel == 1) {
    panelRect(c, x0 + 3, gy, w - 6, gh + 20, theme::withAlpha(c.P.panelLo, 0.96f));
    unisonPanel(c, o, x0, gy, w);
  } else if (panel == 2) {
    panelRect(c, x0 + 3, gy, w - 6, gh + 20, theme::withAlpha(c.P.panelLo, 0.96f));
    optionsPanel(c, o, x0, gy, w, gh + 20);
  } else if (type == OscWavetable || (type != OscMultisample && (!oa || !oa->sample))) {
    // wavetable view: ghost frames behind the current cycle
    if (aux) {
      const float pos = liveP >= 0 ? liveP : valf(c, oscP(o, O_WtPos));
      const auto& cy = cycleOf(c, *aux, o, pos);
      drawCycle(c, cy, x0 + 6, gy + 2, w - 12, gh - 4, val(c, oscP(o, O_Enable)) >= 0.5 ? c.P.accent : c.P.textFaint);
      const MipTable* mip = oa && oa->mip ? oa->mip.get() : nullptr;
      if (!mip) {
        const auto& fm = factoryMips();
        mip = fm[static_cast<size_t>(std::clamp(vali(c, oscP(o, O_Table)) - 1, 0, static_cast<int>(fm.size()) - 1))].get();
      }
      // frame counter / position
      char b[32];
      const int frames = mip ? mip->frames : 1;
      std::snprintf(b, sizeof b, "%d / %d", 1 + static_cast<int>(std::lround(pos * (frames - 1))), frames);
      labelBeside(c, x0 + 7, kDispB - 18, 16, b, c.P.textDim, 8);  // J-128: on the PHASE / RAND baseline
    }
    // edit (pencil)
    ImGui::SetCursorScreenPos(c.p(x1 - 22, gy + 2));
    if (ImGui::InvisibleButton("##ed", c.F.sz(16, 16))) c.u.wtEdit = true, c.u.wtOsc = o, c.u.clip = false;
    iconPencil(c.dl, c.p(x1 - 14, gy + 10), c.s(10), ImGui::IsItemHovered() ? c.P.accent : c.P.textFaint);
    if (ImGui::IsItemHovered()) c.a.setHint("Edit the wavetable");
  } else if (type == OscMultisample) {
    // key zones of the loaded map
    const auto& zones = oa ? oa->zones : std::vector<Zone>{};
    if (zones.empty()) {
      textC(c, x0 + w * 0.5f, gy + gh * 0.5f - 6, "Load a multisample (.sfz) from the menu above", c.P.textFaint, 8.5f, true);
    } else {
      for (const auto& z : zones) {
        const float zx0 = x0 + 6 + (w - 12) * z.loKey / 128.f, zx1 = x0 + 6 + (w - 12) * (z.hiKey + 1) / 128.f;
        const float zy0 = gy + 4 + (gh - 8) * (1.f - (z.hiVel + 1) / 128.f), zy1 = gy + 4 + (gh - 8) * (1.f - z.loVel / 128.f);
        c.dl->AddRectFilled(c.p(zx0, zy0), c.p(zx1, zy1), theme::withAlpha(c.P.accent, 0.22f));
        c.dl->AddRect(c.p(zx0, zy0), c.p(zx1, zy1), theme::withAlpha(c.P.accent, 0.7f));
      }
      char b[48];
      std::snprintf(b, sizeof b, "%d zones", static_cast<int>(zones.size()));
      text(c, x0 + 7, gy + 3, b, c.P.textDim, 8, true);
    }
  } else {
    // sample-based: waveform overview with start / end / loop / position markers
    const Sample* smp = oa->sample.get();
    const auto& ov = overview(smp, smp->left.data(), smp->frames(), 256);
    if (type == OscSpectral) {
      // spectrogram-like tint behind the waveform
      if (oa->spec && oa->spec->frames > 0) {
        const auto& sp = *oa->spec;
        const int cols = 96, rows = 24;
        for (int cx = 0; cx < cols; ++cx)
          for (int ry = 0; ry < rows; ++ry) {
            const int f = sp.frames * cx / cols;
            const int bin = static_cast<int>(std::pow(SpecSrc::kBins, static_cast<float>(ry) / rows));
            const float m = sp.mag[static_cast<size_t>(f) * SpecSrc::kBins + static_cast<size_t>(std::clamp(bin, 0, SpecSrc::kBins - 1))];
            if (m < 0.02f) continue;
            const float px = x0 + 6 + (w - 12) * cx / cols, py = gy + gh - 2 - (gh - 4) * (ry + 1) / rows;
            c.dl->AddRectFilled(c.p(px, py), c.p(px + (w - 12) / cols + 0.5f, py + (gh - 4) / rows + 0.5f),
                                theme::withAlpha(c.P.accent, std::min(0.5f, std::sqrt(m) * 0.6f)));
          }
      }
    }
    drawOverview(c, ov, x0 + 6, gy + 2, w - 12, gh - 4, theme::withAlpha(c.P.accent, type == OscSpectral ? 0.55f : 0.9f));
    auto mark = [&](float f, ImU32 col) { vline(c, x0 + 6 + (w - 12) * f, gy + 2, gy + gh - 2, col); };
    mark(valf(c, oscP(o, O_Start)), c.P.text);
    mark(valf(c, oscP(o, O_End)), c.P.text);
    if (vali(c, oscP(o, O_LoopMode)) > 0) {
      const float s0 = valf(c, oscP(o, O_LoopStart)), s1 = valf(c, oscP(o, O_LoopEnd));
      c.dl->AddRectFilled(c.p(x0 + 6 + (w - 12) * s0, gy + 2), c.p(x0 + 6 + (w - 12) * s1, gy + gh - 2), theme::withAlpha(c.P.mod, 0.10f));
      mark(s0, c.P.mod);
      mark(s1, c.P.mod);
    }
    for (double sl : oa->slices) mark(static_cast<float>(sl / std::max<size_t>(1, smp->frames())), c.P.textFaint);
    if (type == OscGranular || type == OscSpectral) mark(liveP >= 0 ? liveP : valf(c, oscP(o, O_Position)), c.P.warn);
    else if (liveP >= 0) mark(liveP, c.P.warn);
  }
  // bottom row of the display
  {
    const float by = kDispB - 20;
    hline(c, x0 + 3, x1 - 3, by - 0.5f, c.P.line);
    if (type == OscWavetable || type == OscMultisample) {
      if (type == OscMultisample) {
        checkBox(c, oscP(o, O_MEnvOn), x0 + 7, by + 5, "ENV", 8);
        valueBox(c, oscP(o, O_MVelTrack), x0 + 50, by + 2, 92, 16, "VEL TRACK");
      } else {
        valueBox(c, oscP(o, O_Phase), x0 + 70, by + 2, 76, 16, "PHASE");
      }
      valueBox(c, oscP(o, O_RandPhase), x0 + 150, by + 2, 80, 16, "RAND");
    } else {
      choiceBox(c, oscP(o, O_LoopMode), x0 + 5, by + 2, 103, 16);
      if (type == OscSample) {
        valueBox(c, oscP(o, O_LoopStart), x0 + 112, by + 2, 58, 16, "LS");
        valueBox(c, oscP(o, O_LoopEnd), x0 + 172, by + 2, 58, 16, "LE");
      } else {
        stepper(c, oscP(o, O_Unison), x0 + 172, by + 2, 58, 16);
        text(c, x0 + 114, by + 5, "UNISON", c.P.textDim, 8, true);
      }
    }
    // panel switch (waveform / options)
    ImGui::SetCursorScreenPos(c.p(x1 - 23, by + 1));
    if (ImGui::InvisibleButton("##opt", c.F.sz(18, 18))) c.u.oscPanel[o] = c.u.oscPanel[o] == 2 ? 0 : 2;
    iconArrow(c.dl, c.p(x1 - 14, by + 10), c.s(7), c.u.oscPanel[o] == 2 ? 2 : 3, ImGui::IsItemHovered() || c.u.oscPanel[o] == 2 ? c.P.accent : c.P.textDim);
    if (ImGui::IsItemHovered()) c.a.setHint("More oscillator settings (pitch mode, key / velocity zone, sample and grain options)");
  }
  deck(c, x0, kDispB, w, kLowerY - kDispB);
  // knob rows
  auto unisonBox = [&](float x, float y) {
    ImGui::SetCursorScreenPos(c.p(x, y));
    if (ImGui::InvisibleButton("##gear", c.F.sz(18, 20))) c.u.oscPanel[o] = c.u.oscPanel[o] == 1 ? 0 : 1;
    const bool on = c.u.oscPanel[o] == 1;
    c.dl->AddRectFilled(c.p(x, y), c.p(x + 18, y + 20), on ? c.P.accentSoft : c.P.well, c.s(2));
    c.dl->AddRect(c.p(x, y), c.p(x + 18, y + 20), c.P.line, c.s(2));
    iconGear(c.dl, c.p(x + 9, y + 10), c.s(6), on || ImGui::IsItemHovered() ? c.P.accent : c.P.textDim);
    if (ImGui::IsItemHovered()) c.a.setHint("Unison settings");
    stepper(c, oscP(o, O_Unison), x + 20, y, 36, 20);
    cellLabel(c, x + 28, kRow1 + kKnobR + 4.5f, "UNISON", 56);
  };
  vline(c, x0 + 214, kDispB + 8, kLowerY - 8, c.P.line);
  if (type == OscGranular) {
    // window shape button, SCAN, DENS, LENGTH | PAN LEVEL ; OFFSET DIR PITCH RAND RAND RAND
    {
      ImGui::SetCursorScreenPos(c.p(x0 + 12, kRow1 - 11));
      if (ImGui::InvisibleButton("##win", c.F.sz(24, 22))) ImGui::OpenPopup("##winmenu");
      const bool hv = ImGui::IsItemHovered();
      c.dl->AddRectFilled(c.p(x0 + 12, kRow1 - 11), c.p(x0 + 36, kRow1 + 11), hv ? c.P.headOn : c.P.head, c.s(3));
      // window glyph
      ImVec2 prev;
      for (int i = 0; i <= 16; ++i) {
        const float t = i / 16.f;
        const ImVec2 q = c.p(x0 + 15 + t * 18, kRow1 + 7 - 13 * std::sin(3.14159f * t) * std::sin(3.14159f * t));
        if (i) c.dl->AddLine(prev, q, c.P.accent, std::max(1.2f, c.s(1.3f)));
        prev = q;
      }
      cellLabel(c, x0 + 24, kRow1 + kKnobR + 4.5f, "WINDOW", 40);
      if (ImGui::BeginPopup("##winmenu")) {
        const ParamDesc* d = pdesc(oscP(o, O_GWinShape));
        if (d)
          for (int i = 0; i < static_cast<int>(d->choices.size()); ++i)
            if (ImGui::MenuItem(d->choices[static_cast<size_t>(i)].c_str(), nullptr, i == vali(c, oscP(o, O_GWinShape))))
              setv(c, oscP(o, O_GWinShape), i);
        ImGui::EndPopup();
      }
    }
    knob(c, oscP(o, O_Position), x0 + 69, kRow1, kKnobR, "SCAN");
    knob(c, oscP(o, O_GDensity), x0 + 112, kRow1, kKnobR, "DENS");
    knob(c, oscP(o, O_GLength), x0 + 156, kRow1, kKnobR, "LENGTH");
    knob(c, oscP(o, O_ScanRate), x0 + 192, kRow1, kKnobR - 2, "RATE", KnobSmall);
    knob(c, oscP(o, O_GRndOffset), x0 + 24, kRow2, kKnobR, "OFFSET");
    knob(c, oscP(o, O_GRndDir), x0 + 69, kRow2, kKnobR, "DIR");
    knob(c, oscP(o, O_GRndPitch), x0 + 112, kRow2, kKnobR, "PITCH");
    knob(c, oscP(o, O_GRndLength), x0 + 156, kRow2, kKnobR, "LEN RND");
    knob(c, oscP(o, O_GRndPan), x0 + 192, kRow2, kKnobR - 2, "PAN RND", KnobSmall);
  } else {
    if (type == OscSpectral) {
      knob(c, oscP(o, O_ScanRate), x0 + 25, kRow1, kKnobR, "SCAN");
      knob(c, oscP(o, O_SCut), x0 + 70, kRow1, kKnobR, "CUT");
      knob(c, oscP(o, O_SMix), x0 + 187, kRow1, kKnobR, "MIX");
      // spectral filter band
      const float fx = x0 + 88, fw = 80, fy = kRow1 - 9;
      wellRect(c, fx, fy, fw, 18, 2);
      const float lo = valf(c, oscP(o, O_SLo)), hi = valf(c, oscP(o, O_SHi)), cut = valf(c, oscP(o, O_SCut));
      const float a0 = std::clamp(lo + cut * 0.5f, 0.f, 1.f), a1 = std::clamp(hi + cut * 0.5f, 0.f, 1.f);
      c.dl->AddRectFilled(c.p(fx + 2 + (fw - 4) * a0, fy + 3), c.p(fx + 2 + (fw - 4) * std::max(a0, a1), fy + 15), theme::withAlpha(c.P.accent, 0.5f));
      cellLabel(c, fx + fw * 0.5f, kRow1 + kKnobR + 4.5f, "FILTER");
    } else {
      const int k0 = type == OscWavetable ? O_WtPos : type == OscMultisample ? O_MTimbre : O_ScanRate;
      knob(c, oscP(o, k0), x0 + 25, kRow1, kKnobR, type == OscWavetable ? "WT POS" : type == OscMultisample ? "TIMBRE" : "SCAN");
      unisonBox(x0 + 52, kRow1 - 10);
      knob(c, oscP(o, O_UniDetune), x0 + 142, kRow1, kKnobR, "DETUNE");
      knob(c, oscP(o, O_UniBlend), x0 + 187, kRow1, kKnobR, "BLEND");
    }
    knob(c, oscP(o, O_Warp1), x0 + 25, kRow2, kKnobR, nullptr, KnobNoLabel);
    knob(c, oscP(o, O_Warp2), x0 + 187, kRow2, kKnobR, nullptr, KnobNoLabel);
    warpBlock(c, o, x0 + 42, kRow2 - 18, 128);
    cellLabel(c, x0 + 25, kRow2 + kKnobR + 4.5f, "1", 20);
    cellLabel(c, x0 + 187, kRow2 + kKnobR + 4.5f, "2", 20);
    caption(c, x0 + 42, x0 + 170, kRow2 + kKnobR + 4.5f, "WARP");
  }
  knob(c, oscP(o, O_Pan), x0 + 242, kRow1, kKnobR, "PAN");
  knob(c, oscP(o, O_Level), x0 + 242, kRow2, kKnobR, "LEVEL");
  dimIfOff(c, x0, x1, oscP(o, O_Enable));
  ImGui::PopID();
}

// ------------------------------------------------------------------ sub
void subColumn(Ctx& c) {
  const float x0 = kColX[0], x1 = kColX[1], w = x1 - x0;
  ImGui::PushID("sub");
  moduleFrame(c, x0, kPageY, w, kColH, kHeadB, "SUB", subP(S_Enable));
  routeButton(c, RtSub, x1 - 25, kHeadY, 20);
  displayWell(c, x0, kDispY, w, kDispB - kDispY);
  valueBox(c, subP(S_Octave), x0 + 5, kDispY + 4, w - 10, 16, "OCT");
  valueBox(c, subP(S_Coarse), x0 + 5, kDispY + 23, w - 10, 16, "CRS");
  // shapes: 2 x 3 icons
  const int cur = vali(c, subP(S_Shape));
  for (int i = 0; i < SubShapeCount; ++i) {
    // J-128: three rows between CRS and the phase box (they ran 7 px into it)
    const float bx = x0 + 6 + (i % 2) * 33.f, by = kDispY + 43 + (i / 2) * 30.f;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(c.p(bx, by));
    if (ImGui::InvisibleButton("##sh", c.F.sz(31, 28))) setv(c, subP(S_Shape), i);
    const bool hv = ImGui::IsItemHovered(), on = i == cur;
    c.dl->AddRectFilled(c.p(bx, by), c.p(bx + 31, by + 28), on ? c.P.accentSoft : (hv ? c.P.panelHi : c.P.panelLo), c.s(2));
    c.dl->AddRect(c.p(bx, by), c.p(bx + 31, by + 28), on ? theme::withAlpha(c.P.accent, 0.6f) : c.P.line, c.s(2));
    ImVec2 prev;
    for (int q = 0; q <= 24; ++q) {
      const float t = q / 24.f;
      float y = 0;
      switch (i) {
        case SubSine: y = std::sin(6.2832f * t); break;
        case SubRoundRect: y = std::tanh(std::sin(6.2832f * t) * 4.f); break;
        case SubTriangle: y = t < 0.25f ? t * 4 : t < 0.75f ? 2 - t * 4 : t * 4 - 4; break;
        case SubSaw: y = 1 - 2 * t; break;
        case SubSquare: y = t < 0.5f ? 0.85f : -0.85f; break;
        default: y = t < 0.25f ? 0.85f : -0.85f; break;
      }
      const ImVec2 p = c.p(bx + 3.5f + t * 24, by + 14 - y * 9);
      if (q) c.dl->AddLine(prev, p, on ? c.P.accent : c.P.textDim, std::max(1.2f, c.s(1.3f)));
      prev = p;
    }
    if (hv) c.a.setHint(std::string("Sub shape: ") + (i == 0 ? "sine" : i == 1 ? "rounded square" : i == 2 ? "triangle" : i == 3 ? "saw" : i == 4 ? "square" : "pulse"));
    ImGui::PopID();
  }
  valueBox(c, subP(S_Phase), x0 + 5, kDispB - 38, w - 10, 16, "\xCF\x86");
  checkBox(c, subP(S_ContPhase), x0 + 6, kDispB - 17, "FREE", 7.5f);
  checkBox(c, subP(S_PitchTrack), x0 + 42, kDispB - 17, "KEY", 7.5f);
  deck(c, x0, kDispB, w, kLowerY - kDispB);
  knob(c, subP(S_Pan), x0 + 37, kRow1, kKnobR, "PAN");
  knob(c, subP(S_Level), x0 + 37, kRow2, kKnobR, "LEVEL");
  dimIfOff(c, x0, x1, subP(S_Enable));
  ImGui::PopID();
}

// ------------------------------------------------------------------ noise
void noiseColumn(Ctx& c, const Aux* aux) {
  const float x0 = kColX[4], x1 = kColX[5], w = x1 - x0;
  ImGui::PushID("noise");
  moduleFrame(c, x0, kPageY, w, kColH, kHeadB, "NOISE", noiseP(N_Enable));
  routeButton(c, RtNoise, x1 - 25, kHeadY, 20);
  displayWell(c, x0, kDispY, w, kDispB - kDispY);
  const int t = vali(c, noiseP(N_Type));
  const auto& names = noiseNames();
  std::string nm = t >= 0 && t < static_cast<int>(names.size()) ? names[static_cast<size_t>(t)] : "?";
  if (t == 0) nm = c.m.noiseFile.empty() ? std::string("User file\xE2\x80\xA6") : refStem(c.m, c.m.noiseFile);
  int step = 0;
  dropField(c, "nt", x0 + 5, kDispY + 3, w - 10, 18, nm.c_str(),
            [&] {
              for (int i = 1; i < static_cast<int>(names.size()); ++i)
                if (ImGui::MenuItem(names[static_cast<size_t>(i)].c_str(), nullptr, i == t)) setv(c, noiseP(N_Type), i);
              ImGui::Separator();
              if (ImGui::MenuItem(i18n::tr("Load audio file...")))
                openFile(c, i18n::tr("Load noise sample"), {".wav", ".flac", ".aif", ".aiff", ".mp3", ".ogg"},
                         [](App& a, const PluginTarget& tg, const std::string& p) {
                           editState(a, tg, "Load noise", [&](PluginState& st, Model& m) {
                             m.noiseFile = p;
                             st.params[static_cast<size_t>(noiseP(N_Type))] = 0;
                           });
                         });
            });
  (void)step;
  // toggles: one-shot, key track
  toggleBtn(c, noiseP(N_OneShot), x0 + 5, kDispY + 25, 45, 15, "1-SHOT", 7.5f);
  toggleBtn(c, noiseP(N_PitchTrack), x0 + 52, kDispY + 25, 45, 15, "KEY", 7.5f);
  // waveform
  const float gy = kDispY + 44, gh = kDispB - 64 - gy;
  const float* data = nullptr;
  size_t n = 0;
  const void* id = nullptr;
  if (t >= 1) {
    const auto& fn = factoryNoises();
    if (t - 1 < static_cast<int>(fn.size())) data = fn[static_cast<size_t>(t - 1)].L.data(), n = fn[static_cast<size_t>(t - 1)].L.size(), id = data;
  } else if (aux && aux->noiseFile) {
    data = aux->noiseFile->left.data(), n = aux->noiseFile->frames(), id = aux->noiseFile.get();
  }
  if (data && n) drawOverview(c, overview(static_cast<const Sample*>(id), data, n, 96), x0 + 6, gy, w - 12, gh, theme::withAlpha(c.P.accent, 0.8f));
  valueBox(c, noiseP(N_Color), x0 + 5, kDispB - 60, w - 10, 16, "COLOR");
  valueBox(c, noiseP(N_Phase), x0 + 5, kDispB - 40, w - 10, 16, "START");
  valueBox(c, noiseP(N_RandPhase), x0 + 5, kDispB - 20, w - 10, 16, "RAND");
  deck(c, x0, kDispB, w, kLowerY - kDispB);
  knob(c, noiseP(N_Fine), x0 + 28, kRow1, kKnobR, "FINE");
  knob(c, noiseP(N_Pan), x0 + 73, kRow1, kKnobR, "PAN");
  knob(c, noiseP(N_Pitch), x0 + 28, kRow2, kKnobR, "PITCH");
  knob(c, noiseP(N_Level), x0 + 73, kRow2, kKnobR, "LEVEL");
  dimIfOff(c, x0, x1, noiseP(N_Enable));
  ImGui::PopID();
}

// ------------------------------------------------------------------ filter
// Source routed into filter f (balance not fully on the other filter)?
bool feeds(const Ctx& c, int r, int f) {
  if (vali(c, routeP(r, R_Dest)) != RouteFilter) return false;
  const double b = val(c, routeP(r, R_Balance));
  return f == 0 ? b < 0.99 : b > -0.99;
}
void toggleFeed(Ctx& c, int r, int f) {
  const bool me = feeds(c, r, f), other = feeds(c, r, 1 - f);
  if (me && other) setv(c, routeP(r, R_Balance), f == 0 ? 1.0 : -1.0);  // keep the other only
  else if (me) setv(c, routeP(r, R_Dest), RouteMain);
  else if (other) setv(c, routeP(r, R_Balance), 0.0);  // both
  else setv(c, routeP(r, R_Dest), RouteFilter), setv(c, routeP(r, R_Balance), f == 0 ? -1.0 : 1.0);
}

void filterColumn(Ctx& c) {
  const float x0 = kColX[5], x1 = kColX[6], w = x1 - x0;
  ImGui::PushID("filter");
  int& tab = c.u.filterTab;
  frameBox(c, x0, kPageY, w, kColH);
  // two tabs with power + route each, inside the frame's header strip
  for (int f = 0; f < kFilters; ++f) {
    const float hx0 = x0 + kGutter + 1, hw = w - 2 * kGutter - 2;
    const float tx = hx0 + f * hw / 2, tw = hw / 2;
    ImGui::PushID(f);
    const bool on = tab == f;
    c.dl->AddRectFilled(c.p(tx, kHeadY + 1), c.p(tx + tw, kHeadY + kHeadHgt), on ? c.P.headOn : c.P.head, c.s(2.0f),
                        f == 0 ? ImDrawFlags_RoundCornersTopLeft : ImDrawFlags_RoundCornersTopRight);
    if (f == 1) vline(c, tx, kHeadY + 1, kHeadY + kHeadHgt, c.P.frame);
    if (on) c.dl->AddRectFilled(c.p(tx + 3, kHeadY + 1), c.p(tx + tw - 3, kHeadY + 2.6f), c.P.accent, c.s(1));
    led(c, filtP(f, F_Enable), tx + 6, kHeadY + 5.5f, 9);
    ImGui::SetCursorScreenPos(c.p(tx + 18, kHeadY));
    if (ImGui::InvisibleButton("##tab", c.F.sz(tw - 42, kHeadHgt))) tab = f;
    text(c, tx + 20, kHeadY + 5.0f, f == 0 ? "FILTER 1" : "FILTER 2", val(c, filtP(f, F_Enable)) >= 0.5 ? c.P.text : c.P.textFaint, 9, true);
    routeButton(c, f == 0 ? RtF1 : RtF2, tx + tw - 24, kHeadY, 21);
    ImGui::PopID();
  }
  hline(c, x0 + kGutter + 1, x1 - kGutter - 1, kHeadY + kHeadHgt - 0.5f, c.P.frame);
  const int f = tab;
  ImGui::PushID(f + 10);
  // type selector
  displayWell(c, x0, kDispY, w, kDispB - kDispY);
  {
    const int ty = vali(c, filtP(f, F_Type));
    const auto& names = filterNames();
    const std::string nm = ty >= 0 && ty < static_cast<int>(names.size()) ? names[static_cast<size_t>(ty)] : "?";
    int step = 0;
    dropField(c, "ft", x0 + 5, kDispY + 3, w - 10, 18, nm.c_str(), [&] { filterMenuItems(c, filtP(f, F_Type)); }, true, &step);
    if (step) setv(c, filtP(f, F_Type), std::clamp(ty + step, 0, static_cast<int>(names.size()) - 1));
  }
  valueBox(c, filtP(f, F_X), x1 - 94, kDispY + 25, 43, 14, "X");
  valueBox(c, filtP(f, F_Y), x1 - 49, kDispY + 25, 44, 14, "Y");
  // response graph
  {
    const float gx = x0 + 5, gy = kDispY + 42, gw = w - 10, gh = kDispB - 24 - gy;
    constexpr int N = 96;
    float hz[N], mag[N];
    for (int i = 0; i < N; ++i) hz[i] = 20.f * std::pow(1000.f, static_cast<float>(i) / (N - 1));
    double cut = cutoffHz(val(c, filtP(f, F_Freq)));
    if (c.hasLive && c.live.voices > 0 && c.live.filterCut[f] > 0) cut = c.live.filterCut[f];
    Filter::responseCurve(vali(c, filtP(f, F_Type)), 48000, cut, val(c, filtP(f, F_Res)), val(c, filtP(f, F_Var)), hz, mag, N);
    std::vector<ImVec2> pts(N);
    for (int i = 0; i < N; ++i) {
      const float db = mag[i] > 1e-6f ? 20.f * std::log10(mag[i]) : -60.f;
      const float yn = std::clamp((db + 36.f) / 60.f, 0.f, 1.f);
      pts[static_cast<size_t>(i)] = c.p(gx + gw * i / (N - 1), gy + gh - yn * gh);
    }
    const ImVec2 a = c.p(gx, gy), b = c.p(gx + gw, gy + gh);
    c.dl->PushClipRect(a, b, true);
    skin::curveArea(c.dl, pts.data(), N, b.y, val(c, filtP(f, F_Enable)) >= 0.5 ? c.P.accent : c.P.textFaint, std::max(1.6f, c.s(1.7f)), 0.18f);
    c.dl->PopClipRect();
    // drag in the graph: x = cutoff, y = resonance
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##fg", ImVec2(b.x - a.x, b.y - a.y));
    if (ImGui::IsItemActive() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      const double fx = std::clamp((m.x - a.x) / (b.x - a.x), 0.f, 1.f);
      const double hzv = 20.0 * std::pow(1000.0, fx);
      setv(c, filtP(f, F_Freq), std::clamp(cutoffNorm(hzv), 0.0, 1.0), true);
      setv(c, filtP(f, F_Res), std::clamp(1.0 - (m.y - a.y) / (b.y - a.y), 0.0, 1.0), true);
    }
    if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
    if (ImGui::IsItemHovered()) c.a.setHint("Drag: cutoff (left / right) and resonance (up / down)");
  }
  // S A B C N sources + key track
  {
    const float by = kDispB - 20;
    hline(c, x0 + 3, x1 - 3, by - 0.5f, c.P.line);
    const char* lab[5] = {"S", "A", "B", "C", "N"};
    const int rs[5] = {RtSub, RtA, RtB, RtC, RtNoise};
    const float bw = (w - 10 - 32) / 5;
    for (int i = 0; i < 5; ++i) {
      ImGui::PushID(i + 60);
      const bool on = feeds(c, rs[i], f);
      if (button(c, "src", x0 + 5 + bw * i, by + 2, bw - 2, 16, lab[i], on, 8.5f)) toggleFeed(c, rs[i], f);
      if (ImGui::IsItemHovered())
        c.a.setHint(std::string(lab[i] == std::string("S") ? "Sub" : lab[i] == std::string("N") ? "Noise" : std::string("Osc ") + lab[i]) +
                    (on ? " feeds this filter (click: remove)" : " does not feed this filter (click: add)"));
      ImGui::PopID();
    }
    toggleBtn(c, filtP(f, F_KeyTrack), x1 - 34, by + 2, 29, 16, "KEY", 7.5f);
  }
  deck(c, x0, kDispB, w, kLowerY - kDispB);
  // knobs
  const char* varLabel = filterVarLabel(vali(c, filtP(f, F_Type)));
  knob(c, filtP(f, F_Freq), x0 + 32, kRow1, kKnobR, "CUTOFF");
  knob(c, filtP(f, F_Res), x0 + 80, kRow1, kKnobR, "RES");
  knob(c, filtP(f, F_Stereo), x0 + 145, kRow1, kKnobR, "PAN");
  knob(c, filtP(f, F_Drive), x0 + 32, kRow2, kKnobR, "DRIVE");
  knob(c, filtP(f, F_Var), x0 + 80, kRow2, kKnobR, varLabel && *varLabel ? varLabel : "VAR");
  knob(c, filtP(f, F_Wet), x0 + 145, kRow2, kKnobR, "MIX");
  vline(c, x0 + 115, kDispB + 8, kLowerY - 8, c.P.line);
  vfader(c, filtP(f, F_Level), x0 + 172, kDispB + 9, 18, 90);
  cellLabel(c, x0 + 181, kRow2 + kKnobR + 4.5f, "LEVEL", 34);
  dimIfOff(c, x0, x1, filtP(f, F_Enable));
  ImGui::PopID();
  ImGui::PopID();
}

}  // namespace

std::string fs_stem(const std::string& p) {
  size_t a = p.find_last_of("/\\");
  std::string n = a == std::string::npos ? p : p.substr(a + 1);
  const size_t d = n.find_last_of('.');
  return d == std::string::npos || d == 0 ? n : n.substr(0, d);
}

std::string refStem(const Model& m, const std::string& ref) {
  if (const Media* e = findMedia(m, ref)) return fs_stem(e->name.empty() ? ref : e->name);
  return fs_stem(ref);
}

void oscPage(Ctx& c) {
  const auto aux = auxOf(c);
  subColumn(c);
  for (int o = 0; o < kOscs; ++o) oscColumn(c, o, aux.get());
  noiseColumn(c, aux.get());
  filterColumn(c);
}

}  // namespace gl::packs::plasma::ui
