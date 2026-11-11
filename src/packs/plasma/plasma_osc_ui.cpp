// Gridline — "plasma" pack: the source and filter views of the stage: Layer
// 1-3 (engine, table or sample display with the table map, position, morph,
// engine card, pitch, stack, out), Root and Texture, and Filters (both, with
// their response graphs and inputs), plus the rail's mini displays.
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include "../../app/App.h"
#include "../../core/I18n.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {

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
// warped = false: the same cycle with both morphs off (the "before" picture).
const std::vector<float>& cycleOf(const Ctx& c, const Aux& aux, int o, float pos, bool warped = true, float a1 = -1, float a2 = -1) {
  static std::map<uint64_t, CycleCache> cache;
  CycleCache& e = cache[c.t.key() * 8 + static_cast<uint64_t>(o) * 2 + (warped ? 0 : 1)];
  uint64_t h = hashRange(c.st.params, oscP(o, 0), oscP(o, O_Count), 0xcbf29ce484222325ull);
  h = (h ^ static_cast<uint64_t>(std::lround(pos * 4096))) * 0x100000001B3ull;
  h = (h ^ reinterpret_cast<uintptr_t>(&aux)) * 0x100000001B3ull;
  h = (h ^ static_cast<uint64_t>(val(c, G_S1Mode) >= 0.5)) * 0x100000001B3ull;
  h = (h ^ static_cast<uint64_t>(std::lround((a1 + 2) * 1024) * 8191 + std::lround((a2 + 2) * 1024))) * 0x100000001B3ull;
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
  if (!warped) s.w1 = s.w2 = 0;
  s.a1 = a1 >= 0 ? a1 : valf(c, oscP(o, O_Warp1));
  s.a2 = a2 >= 0 ? a2 : valf(c, oscP(o, O_Warp2));
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
    c.dl->AddLine(a, ImVec2(b.x, std::max(b.y, a.y + 1)), col, std::max(1.0f, c.s(w / n) * 0.6f));
  }
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
}  // namespace

const TableCatalog& tableCatalogUi(const Ctx& c, bool rescan) {
  static TableCatalog cat;
  static bool built = false;
  if (!built || rescan) {
    cat = buildTableCatalog({userTableDir(c.a.configDir())}, libraryTableDirs(defaultSerumRoots()));
    built = true;
  }
  return cat;
}

void chooseTableEntry(Ctx& c, int o, const TableEntry& e) {
  auto& d = c.m.osc[static_cast<size_t>(o)];
  if (e.factory > 0) {
    d.table.clear(), d.tableName.clear(), d.frames.clear();
  } else {
    d.table = e.ref;
    d.tableName = e.name;
    d.frames.clear();
  }
  // the table, its slot and the wavetable type together: one undo step
  commitParams(c, "Choose table", {{oscP(o, O_Table), e.factory > 0 ? e.factory : 0}, {oscP(o, O_Type), OscWavetable}});
}

namespace {

void tableMenu(Ctx& c, int o) {
  const int type = vali(c, oscP(o, O_Type));
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
  // every table, by group with its size, and a search over all of them
  {
    const TableCatalog& cat = tableCatalogUi(c);
    const std::string& curRef = c.m.osc[static_cast<size_t>(o)].table;
    auto isCur = [&](const TableEntry& e) { return e.factory > 0 ? (curRef.empty() && cur == e.factory) : curRef == e.ref; };
    static char find[64] = "";
    if (ImGui::IsWindowAppearing()) find[0] = 0;
    ImGui::SetNextItemWidth(c.s(220));
    ImGui::InputTextWithHint("##tfind", i18n::tr("Search every table"), find, sizeof find);
    char total[64];
    std::snprintf(total, sizeof total, i18n::tr("%d tables"), static_cast<int>(cat.entries.size()));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", total);
    if (find[0]) {
      std::string q = find;
      for (char& ch : q) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      int shown = 0;
      for (const auto& e : cat.entries) {
        std::string n = e.name + " " + e.group;
        for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (n.find(q) == std::string::npos) continue;
        if (++shown > 300) break;
        ImGui::PushID(&e);
        if (ImGui::MenuItem(e.name.c_str(), e.group.c_str(), isCur(e))) chooseTableEntry(c, o, e);
        ImGui::PopID();
      }
      if (!shown) ImGui::TextDisabled("%s", i18n::tr("No table has that in its name"));
    } else {
      size_t at = 0;
      for (const auto& g : cat.groups) {
        char lb[160];
        std::snprintf(lb, sizeof lb, "%s (%d)", i18n::tr(g.name.c_str()), g.count);
        if (ImGui::BeginMenu(lb)) {
          for (size_t i = at; i < at + static_cast<size_t>(g.count) && i < cat.entries.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(cat.entries[i].name.c_str(), nullptr, isCur(cat.entries[i]))) chooseTableEntry(c, o, cat.entries[i]);
            ImGui::PopID();
          }
          ImGui::EndMenu();
        }
        at += static_cast<size_t>(g.count);
      }
    }
    ImGui::Separator();
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
  if (ImGui::MenuItem(i18n::tr("Read the table folders again"))) tableCatalogUi(c, true);
  if (ImGui::IsItemHovered()) c.a.setHint("Lists the tables again: your table folder and the installed wavetable library");
  if (ImGui::MenuItem(i18n::tr("Edit the table..."))) c.u.wtOsc = o, openView(c.u, VTable);
}

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


// ================================================================== shared pieces
void routeFields(Ctx& c, int r, float x, float y, float w, float rowH) {
  ImGui::PushID(r + 700);
  segments(c, routeP(r, R_Dest), x, y, w, 20, {"Filters", "Out", "Direct", "Off"});
  const bool toFilters = vali(c, routeP(r, R_Dest)) == RouteFilter;
  bar(c, routeP(r, R_Balance), x, y + rowH, w, 20, "Filter 1 / Filter 2", BarCentre | (toFilters ? 0 : BarDim));
  const float hw = (w - 6) * 0.5f;
  bar(c, routeP(r, R_Bus1), x, y + rowH * 2, hw, 20, "Send A");
  bar(c, routeP(r, R_Bus2), x + hw + 6, y + rowH * 2, hw, 20, "Send B");
  chip(c, tailP(T_RawA + r), x, y + rowH * 3, w, 20, "Leaves Envelope 1 out");
  ImGui::PopID();
}

float routeCompact(Ctx& c, int r, float x, float y, float w) {
  ImGui::PushID(r + 720);
  segments(c, routeP(r, R_Dest), x, y, w, 26, {"Filters", "Out", "Direct", "Off"});
  if (ImGui::IsItemHovered()) c.a.setHint("Where it goes: the filters, out through the effects, direct (past them), or nowhere");
  const bool toFilters = vali(c, routeP(r, R_Dest)) == RouteFilter;
  const float bw = w - 150;
  bar(c, routeP(r, R_Balance), x, y + 34, bw, 26, "Filter 1 / 2", BarCentre | (toFilters ? 0 : BarDim));
  moreLink(c, "sends", x + bw + 10, y + 34, w - bw - 10, 26, "Sends...", {routeP(r, R_Bus1), routeP(r, R_Bus2), tailP(T_RawA + r)}, {},
           "Send A, Send B, and leaving Envelope 1 out");
  ImGui::PopID();
  return y + 60;
}

namespace {
// The rail and the layer view: the sample of a layer (or null).
const OscAux* oscAux(const Aux* aux, int o) { return aux ? &aux->osc[static_cast<size_t>(o)] : nullptr; }
bool showsTable(const Ctx& c, const OscAux* oa, int o) {
  const int type = vali(c, oscP(o, O_Type));
  return type == OscWavetable || (type != OscMultisample && (!oa || !oa->sample));
}
const MipTable* mipOf(const Ctx& c, const OscAux* oa, int o) {
  if (oa && oa->mip) return oa->mip.get();
  const auto& fm = factoryMips();
  return fm[static_cast<size_t>(std::clamp(vali(c, oscP(o, O_Table)) - 1, 0, static_cast<int>(fm.size()) - 1))].get();
}
float livePos(const Ctx& c, int o) { return c.hasLive && c.live.voices > 0 && c.live.oscOn[o] ? c.live.oscPos[o] : -1.f; }
// A morph's amount as it plays: the setting plus what its links add now (the lead note), -1 when
// nothing plays.
float liveMorph(const Ctx& c, int o, int i) {
  if (!(c.hasLive && c.live.voices > 0 && c.live.oscOn[o])) return -1.f;
  const int k = oscP(o, i == 0 ? O_Warp1 : O_Warp2);
  float a = valf(c, k);
  for (int s = 0; s < kModSlots; ++s)
    if (vali(c, modP(s, M_Dest)) == k + 1) a += c.live.mod[s];
  return std::clamp(a, 0.f, 1.f);
}

// The table in depth: every frame a line, stacked back and up in perspective and fading with
// depth; the set position lit; the playing frame (position plus its links, live) filled and
// glowing with a trail of where it just was; a depth rail at the right with the set position (a
// diamond) and the playing one (a dot). fill: unused (kept for callers).
void tableDepth(Ctx& c, int o, const MipTable* mip, float x, float y, float w, float h, bool on, bool fill) {
  (void)fill;
  if (!mip || mip->frames < 1) return;
  const int lv = 4, len = mip->len[static_cast<size_t>(lv)], frames = mip->frames;
  const ImU32 col = on ? (gKnobCol ? gKnobCol : partCol(c, o)) : c.P.textFaint;
  struct Pr { float x0, w, y0, a; };
  auto proj = [&](float d) { return Pr{x + w * 0.06f + d * w * 0.27f, w * 0.64f * (1.f - 0.16f * d), y + h * 0.74f - d * h * 0.5f, h * 0.17f * (1.f - 0.22f * d)}; };
  auto line = [&](float fpos, int np, std::vector<ImVec2>& pts) {
    const Pr p = proj(fpos);
    const int f = std::clamp(static_cast<int>(std::lround(fpos * (frames - 1))), 0, frames - 1);
    const float* data = mip->frame(lv, f);
    pts.resize(static_cast<size_t>(np));
    for (int j = 0; j < np; ++j) {
      const float t = j / static_cast<float>(np - 1);
      pts[static_cast<size_t>(j)] = c.p(p.x0 + p.w * t, p.y0 - std::clamp(data[std::min(len - 1, static_cast<int>(t * len))], -1.f, 1.f) * p.a);
    }
  };
  const ImU32 grid2 = theme::withAlpha(c.P.text, c.P.dark ? 0.08f : 0.11f);
  {  // the floor rails
    const Pr f0 = proj(0), f1 = proj(1);
    c.dl->AddLine(c.p(f0.x0, f0.y0), c.p(f1.x0, f1.y0), grid2, 1.f);
    c.dl->AddLine(c.p(f0.x0 + f0.w, f0.y0), c.p(f1.x0 + f1.w, f1.y0), grid2, 1.f);
  }
  const float set = valf(c, oscP(o, O_WtPos));
  const int F = std::clamp(frames, 2, 30);
  std::vector<ImVec2> pts;
  for (int i = F - 1; i >= 0; --i) {  // back to front
    const float d = i / static_cast<float>(F - 1);
    line(d, 72, pts);
    const bool near = std::fabs(d - set) < 0.5f / (F - 1);
    const float al = near ? 0.95f : (0.07f + 0.22f * (1.f - d)) * (c.P.dark ? 1.f : 1.4f);
    c.dl->AddPolyline(pts.data(), 72, theme::withAlpha(col, al), 0, near ? c.s(1.4f) : 1.f);
  }
  const float lp = livePos(c, o), play = lp >= 0 ? lp : set;
  static std::map<uint64_t, std::vector<float>> trails;
  auto& tr = trails[c.t.key() * 4 + static_cast<uint64_t>(o)];
  if (lp >= 0) {
    if (tr.empty() || std::fabs(tr.back() - lp) > 1e-4f) tr.push_back(lp);
    if (tr.size() > 10) tr.erase(tr.begin());
  } else {
    tr.clear();
  }
  for (size_t i = 0; i + 1 < tr.size(); ++i) {
    line(tr[i], 72, pts);
    const float k = static_cast<float>(tr.size() - 1 - i);
    c.dl->AddPolyline(pts.data(), 72, theme::withAlpha(col, 0.5f * std::pow(1.f - k / (tr.size() + 1.f), 1.6f)), 0, c.s(1.2f));
  }
  {  // the playing frame
    const Pr pp = proj(play);
    line(play, 120, pts);
    fillUnder(c, pts, c.p(0, pp.y0).y, col, 0.32f, 0.02f, c.p(0, pp.y0 - pp.a).y);
    glowLine(c, pts, col, M(2.2f), 1.2f);
    c.dl->AddPolyline(pts.data(), 120, IM_COL32(255, 248, 236, c.P.dark ? 150 : 200), 0, std::max(0.8f, c.s(0.7f)));
  }
  {  // the depth rail: set position (diamond) and the playing one (dot)
    const Pr r0 = proj(0), r1 = proj(1);
    const float rx0 = r0.x0 + r0.w + w * 0.035f, rx1 = r1.x0 + r1.w + w * 0.035f, ry0 = r0.y0 + r0.a * 0.9f, ry1 = r1.y0 + r1.a * 0.9f;
    auto at = [&](float t) { return c.p(rx0 + (rx1 - rx0) * t, ry0 + (ry1 - ry0) * t); };
    c.dl->AddLine(at(0), at(1), grid2, c.s(M(2)));
    c.dl->AddLine(at(set), at(play), theme::withAlpha(col, 0.45f), c.s(M(2)));
    const ImVec2 m = at(set);
    const float e = c.s(M(4));
    c.dl->AddQuadFilled(ImVec2(m.x, m.y - e), ImVec2(m.x + e, m.y), ImVec2(m.x, m.y + e), ImVec2(m.x - e, m.y), theme::withAlpha(col, 0.9f));
    glowDot(c, at(play), M(3.6f), col);
  }
}

// A layer's sample with its region, loop and slice marks.
void sampleDisplay(Ctx& c, int o, const OscAux* oa, float x, float y, float w, float h, bool marks) {
  const int type = vali(c, oscP(o, O_Type));
  if (type == OscMultisample) {
    const auto& zones = oa ? oa->zones : std::vector<Zone>{};
    if (zones.empty()) {
      if (marks) textC(c, x + w * 0.5f, y + h * 0.5f - 6, "Load a multisample (.sfz) with the field above", c.P.textDim, 10.0f, false);
      return;
    }
    for (const auto& z : zones) {
      const float zx0 = x + 4 + (w - 8) * z.loKey / 128.f, zx1 = x + 4 + (w - 8) * (z.hiKey + 1) / 128.f;
      const float zy0 = y + 4 + (h - 8) * (1.f - (z.hiVel + 1) / 128.f), zy1 = y + 4 + (h - 8) * (1.f - z.loVel / 128.f);
      c.dl->AddRectFilled(c.p(zx0, zy0), c.p(zx1, zy1), theme::withAlpha(c.P.knob, 0.18f));
      c.dl->AddRect(c.p(zx0, zy0), c.p(zx1, zy1), theme::withAlpha(c.P.knob, 0.6f));
    }
    if (marks) {
      char b[48];
      std::snprintf(b, sizeof b, "%d zones", static_cast<int>(zones.size()));
      text(c, x + 8, y + 6, b, c.P.textDim, 10.0f, false);
    }
    return;
  }
  if (!oa || !oa->sample) return;
  const Sample* smp = oa->sample.get();
  if (type == OscSpectral && oa->spec && oa->spec->frames > 0 && marks) {
    const auto& sp = *oa->spec;
    const int cols = 120, rows = 30;
    for (int cx = 0; cx < cols; ++cx)
      for (int ry = 0; ry < rows; ++ry) {
        const int f = sp.frames * cx / cols;
        const int bin = static_cast<int>(std::pow(SpecSrc::kBins, static_cast<float>(ry) / rows));
        const float m = sp.mag[static_cast<size_t>(f) * SpecSrc::kBins + static_cast<size_t>(std::clamp(bin, 0, SpecSrc::kBins - 1))];
        if (m < 0.02f) continue;
        const float px = x + 4 + (w - 8) * cx / cols, py = y + h - 2 - (h - 4) * (ry + 1) / rows;
        c.dl->AddRectFilled(c.p(px, py), c.p(px + (w - 8) / cols + 0.5f, py + (h - 4) / rows + 0.5f), theme::withAlpha(c.P.hot, std::min(0.45f, std::sqrt(m) * 0.5f)));
      }
  }
  drawOverview(c, overview(smp, smp->left.data(), smp->frames(), marks ? 320 : 64), x + 4, y + 2, w - 8, h - 4, theme::withAlpha(c.P.knob, 0.85f));
  if (!marks) return;
  auto mark = [&](float f, ImU32 col) { vline(c, x + 4 + (w - 8) * f, y + 2, y + h - 2, col); };
  mark(valf(c, oscP(o, O_Start)), c.P.text);
  mark(valf(c, oscP(o, O_End)), c.P.text);
  if (vali(c, oscP(o, O_LoopMode)) > 0) {
    const float s0 = valf(c, oscP(o, O_LoopStart)), s1 = valf(c, oscP(o, O_LoopEnd));
    c.dl->AddRectFilled(c.p(x + 4 + (w - 8) * s0, y + 2), c.p(x + 4 + (w - 8) * s1, y + h - 2), theme::withAlpha(c.P.accent, 0.12f));
    mark(s0, c.P.accent);
    mark(s1, c.P.accent);
  }
  for (double sl : oa->slices) mark(static_cast<float>(sl / std::max<size_t>(1, smp->frames())), c.P.textFaint);
  const float lp = livePos(c, o);
  if (type == OscGranular || type == OscSpectral) mark(lp >= 0 ? lp : valf(c, oscP(o, O_Position)), c.P.hot);
  else if (lp >= 0) mark(lp, c.P.hot);
}

// The table map: one thin row per frame (tinted by its level), the position ringed.
void tableMap(Ctx& c, int o, const MipTable* mip, float x, float y, float w, float h) {
  display(c, x, y, w, h, 1, 4);
  if (!mip || mip->frames < 1) return;
  static std::map<const MipTable*, std::vector<float>> rmsCache;
  auto& rms = rmsCache[mip];
  if (rms.empty()) {
    const int lv = 4;
    const int len = mip->len[static_cast<size_t>(lv)];
    float top = 1e-6f;
    rms.resize(static_cast<size_t>(mip->frames));
    for (int f = 0; f < mip->frames; ++f) {
      const float* d = mip->frame(lv, f);
      double acc = 0;
      for (int i = 0; i < len; ++i) acc += static_cast<double>(d[i]) * d[i];
      rms[static_cast<size_t>(f)] = static_cast<float>(std::sqrt(acc / std::max(1, len)));
      top = std::max(top, rms[static_cast<size_t>(f)]);
    }
    for (float& r : rms) r /= top;
  }
  const int frames = mip->frames;
  const int rows = std::min(frames, static_cast<int>(h / 2));
  const float rh = (h - 4) / rows;
  for (int r = 0; r < rows; ++r) {
    const int f = frames * r / rows;
    const float lv = rms[static_cast<size_t>(f)];
    const float ry = y + 2 + r * rh;
    c.dl->AddRectFilled(c.p(x + 3, ry), c.p(x + 3 + (w - 6) * (0.15f + 0.85f * lv), ry + std::max(1.0f, rh - 0.6f)),
                        theme::withAlpha(c.P.knob, 0.12f + 0.45f * lv));
  }
  const float lp = livePos(c, o);
  const float pos = valf(c, oscP(o, O_WtPos));
  const float py = y + 2 + (h - 4) * pos;
  c.dl->AddRect(c.p(x + 1.5f, py - 3), c.p(x + w - 1.5f, py + 3), c.P.hot, c.s(2), 0, std::max(1.2f, c.s(1.5f)));
  if (lp >= 0) hline(c, x + 2, x + w - 2, y + 2 + (h - 4) * lp, c.P.hot);
  // a click or a drag on the map sets the position
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##map", c.F.sz(w, h));
  if (ImGui::IsItemActive()) {
    const float my = (ImGui::GetIO().MousePos.y - c.p(0, y + 2).y) / c.s(h - 4);
    setv(c, oscP(o, O_WtPos), std::clamp(static_cast<double>(my), 0.0, 1.0), true);
  }
  if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
  if (ImGui::IsItemHovered()) {
    char b[96];
    std::snprintf(b, sizeof b, "Table map: %d frames, frame %d (click or drag: position)", frames, 1 + static_cast<int>(std::lround(pos * (frames - 1))));
    c.a.setHint(b);
  }
}

// The stack fan: each voice's pitch offset (across the top, height = its level) and its place
// in the stereo field (the dots on the L-R line below). The dial under the pointer lights its row.
void stackFan(Ctx& c, int o, float x, float y, float w, float h) {
  display(c, x, y, w, h, 8, 0);
  const int n = std::max(1, vali(c, oscP(o, O_Unison)));
  const float det = std::sqrt(std::clamp(valf(c, oscP(o, O_UniDetune)), 0.f, 1.f));
  const float blend = valf(c, oscP(o, O_UniBlend)), width = valf(c, oscP(o, O_UniWidth)), tilt = valf(c, oscP(o, O_UniSpan));
  const bool hp = c.u.hoverK == oscP(o, O_UniDetune) || c.u.hoverK == oscP(o, O_UniBlend) || c.u.hoverK == oscP(o, O_Unison);
  const bool hs = c.u.hoverK == oscP(o, O_UniWidth);
  const float cx = x + w * 0.5f, ph = h * 0.62f, sy = y + ph + (h - ph) * 0.5f;
  hline(c, x + 8, x + w - 8, sy, theme::withAlpha(hs ? c.P.hot : c.P.line, hs ? 0.7f : 1.f));
  textR(c, x + w - 3, sy - 6, "R", c.P.textFaint, 9.5f, false);
  text(c, x + 2, sy - 6, "L", c.P.textFaint, 9.5f, false);
  vline(c, cx, y + 3, y + ph, theme::withAlpha(c.P.line, 0.8f));
  for (int i = 0; i < n; ++i) {
    const float t = n == 1 ? 0.f : -1.f + 2.f * i / (n - 1);
    const float off = t * (1.f + tilt * t * 0.5f) * det * (w * 0.42f);
    const bool outer = n > 2 && (i == 0 || i == n - 1 || std::fabs(t) > 0.34f);
    const float lv = n == 1 ? 1.f : (outer ? 0.25f + 0.75f * blend : 1.f - 0.4f * blend);
    const float vx = cx + off;
    c.dl->AddLine(c.p(vx, y + ph), c.p(vx, y + ph - (ph - 6) * lv), hp ? c.P.hot : (outer ? c.P.knob : c.P.text), std::max(1.4f, c.s(2.0f)));
    const float px = cx + t * width * (w * 0.5f - 14);  // stereo place: spread by the width
    c.dl->AddCircleFilled(c.p(px, sy), c.s(2.6f), hs ? c.P.hot : (outer ? c.P.knob : c.P.text), 10);
  }
}
}  // namespace

void cycleMini(Ctx& c, int o, float x, float y, float w, float h) {
  const auto aux = auxOf(c);
  const OscAux* oa = oscAux(aux.get(), o);
  if (!showsTable(c, oa, o)) {
    sampleDisplay(c, o, oa, x, y, w, h, false);
    return;
  }
  if (!aux) return;
  const float lp = livePos(c, o);
  const auto& cy = cycleOf(c, *aux, o, lp >= 0 ? lp : valf(c, oscP(o, O_WtPos)));
  const int n = 48;
  ImVec2 pts[n + 1];
  for (int i = 0; i <= n; ++i) {
    const size_t k = std::min(cy.size() - 1, cy.size() * static_cast<size_t>(i) / n);
    pts[i] = c.p(x + 3 + (w - 6) * i / n, y + h * 0.5f - std::clamp(cy[k], -1.f, 1.f) * h * 0.4f);
  }
  c.dl->AddPolyline(pts, n + 1, val(c, oscP(o, O_Enable)) >= 0.5 ? c.P.knob : c.P.textFaint, 0, std::max(1.2f, c.s(1.4f)));
}

void filterCurve(Ctx& c, int f, float x, float y, float w, float h, bool interactive) {
  constexpr int N = 120;
  float hz[N], mag[N];
  for (int i = 0; i < N; ++i) hz[i] = 20.f * std::pow(1000.f, static_cast<float>(i) / (N - 1));
  double cut = cutoffHz(val(c, filtP(f, F_Freq)));
  if (c.hasLive && c.live.voices > 0 && c.live.filterCut[f] > 0) cut = c.live.filterCut[f];
  Filter::responseCurve(vali(c, filtP(f, F_Type)), 48000, cut, val(c, filtP(f, F_Res)), val(c, filtP(f, F_Var)), hz, mag, N);
  std::vector<ImVec2> pts(N);
  for (int i = 0; i < N; ++i) {
    const float db = mag[i] > 1e-6f ? 20.f * std::log10(mag[i]) : -60.f;
    const float yn = std::clamp((db + 36.f) / 60.f, 0.f, 1.f);
    pts[static_cast<size_t>(i)] = c.p(x + w * i / (N - 1), y + h - 2 - yn * (h - 4));
  }
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  const bool on = val(c, filtP(f, F_Enable)) >= 0.5;
  c.dl->PushClipRect(a, b, true);
  c.dl->AddPolyline(pts.data(), N, on ? c.P.knob : c.P.textFaint, 0, std::max(1.4f, c.s(interactive ? 2.0f : 1.4f)));
  c.dl->PopClipRect();
  if (!interactive) return;
  // the handle: x = cutoff, y = resonance (Shift: morph X / Y)
  const float hx = x + w * static_cast<float>(std::clamp(std::log(cutoffHz(val(c, filtP(f, F_Freq))) / 20.0) / std::log(1000.0), 0.0, 1.0));
  const float hy = y + h - valf(c, filtP(f, F_Res)) * h;
  ImGui::SetCursorScreenPos(a);
  ImGui::InvisibleButton("##fg", ImVec2(b.x - a.x, b.y - a.y));
  const bool act = ImGui::IsItemActive(), hv = ImGui::IsItemHovered();
  if (act && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const double fx = std::clamp((m.x - a.x) / (b.x - a.x), 0.f, 1.f), fy = std::clamp(1.0 - (m.y - a.y) / (b.y - a.y), 0.0, 1.0);
    if (ImGui::GetIO().KeyShift) {
      setv(c, filtP(f, F_X), fx, true);
      setv(c, filtP(f, F_Y), fy, true);
    } else {
      setv(c, filtP(f, F_Freq), std::clamp(cutoffNorm(20.0 * std::pow(1000.0, fx)), 0.0, 1.0), true);
      setv(c, filtP(f, F_Res), fy, true);
    }
  }
  if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
  {  // the cutoff as a line with its value at the top, the resonance as the handle's height
    const bool hk = c.u.hoverK == filtP(f, F_Freq) || act, hr = c.u.hoverK == filtP(f, F_Res);
    vline(c, hx, y + 18, y + h, theme::withAlpha(hk ? c.P.hot : c.P.knob, hk ? 0.8f : 0.35f));
    if (hr) hline(c, x, x + w, hy, theme::withAlpha(c.P.hot, 0.6f));
    const std::string cs = paramText(filtP(f, F_Freq), val(c, filtP(f, F_Freq)), &c.st.params);
    MonoText mono;
    const float tw = textW(c, cs.c_str(), 10.5f, false);
    const float lx = std::clamp(hx - tw * 0.5f, x + 4, x + w - tw - 4);
    text(c, lx, y + 4, cs.c_str(), hk ? c.P.hot : c.P.textDim, 10.5f, false);
  }
  c.dl->AddCircleFilled(c.p(hx, hy), c.s(act ? 6.f : 5.f), act || hv ? c.P.hot : theme::withAlpha(c.P.hot, 0.85f), 16);
  c.dl->AddCircle(c.p(hx, hy), c.s(act ? 6.f : 5.f), c.P.frame, 16, std::max(1.0f, c.s(1.0f)));
  if (hv) c.a.setHint("Drag the point: cutoff (left / right) and resonance (up / down) \xC2\xB7 Shift+drag: morph X / Y");
}

// ================================================================== Layer 1-3
void layerView(Ctx& c) {
  // Grid: left column x 12..612 (source, the display, position, Morph and the engine's card),
  // right column x 624..988 (Out, Pitch, Stack); 12 apart everywhere.
  const int o = std::clamp(c.u.layer, 0, kOscs - 1);
  ImGui::PushID(o + 300);
  char title[32];
  std::snprintf(title, sizeof title, "Layer %d", o + 1);
  const float hx = stageHead(c, title);
  segments(c, oscP(o, O_Type), hx, kStageHeadY + 3, 500, 24, {"Wavetable", "Multisample", "Sample", "Grains", "Spectral"});
  if (button(c, "zones", 888, kStageHeadY + 3, 100, 24, "Key zones", false, 11.0f)) c.u.view = VZones;
  if (ImGui::IsItemHovered()) c.a.setHint("Key zones: the key and velocity range of this layer");
  const auto aux = auxOf(c);
  const OscAux* oa = oscAux(aux.get(), o);
  const int type = vali(c, oscP(o, O_Type));
  const bool table = showsTable(c, oa, o);
  const float L = 12, LW = 600, R = 624, RW = 364;
  // the source: table / sample / multisample field, Edit, Load
  {
    std::string nm;
    const auto& d = c.m.osc[static_cast<size_t>(o)];
    if (type == OscMultisample) nm = d.sfz.empty() ? std::string("No multisample") : refStem(c.m, d.sfz);
    else if (type != OscWavetable && !d.sample.empty()) nm = refStem(c.m, d.sample);
    else if (!d.table.empty()) nm = d.tableName.empty() ? std::string("Custom table") : d.tableName;
    else {
      const int t = vali(c, oscP(o, O_Table));
      const auto& n = factoryTableNames();
      nm = t >= 1 && t <= static_cast<int>(n.size()) ? n[static_cast<size_t>(t - 1)] : "No table";
    }
    reach(oscP(o, O_Table));
    int step = 0;
    dropField(c, "tbl", L, kStageY + 10, table ? LW - 168 : LW, 28, nm.c_str(), [&] { tableMenu(c, o); }, true, &step);
    if (step && type == OscWavetable) {
      const int n = static_cast<int>(factoryTableNames().size());
      const int t = vali(c, oscP(o, O_Table));
      c.m.osc[static_cast<size_t>(o)].table.clear();
      commitParams(c, "Choose table", {{oscP(o, O_Table), ((std::max(1, t) - 1 + step + n) % n) + 1}});
    }
    if (table) {
      if (button(c, "edit", L + LW - 160, kStageY + 10, 76, 28, "Edit", false, 12.0f)) c.u.wtOsc = o, openView(c.u, VTable);
      if (ImGui::IsItemHovered()) c.a.setHint("Table editor: draw, compute and arrange the frames of this table");
      if (button(c, "load", L + LW - 78, kStageY + 10, 78, 28, "Load...", false, 12.0f))
        openFile(c, i18n::tr("Load wavetable"), {".wav"}, [o](App& a, const PluginTarget& t, const std::string& p) {
          editState(a, t, "Load wavetable", [&](PluginState& st, Model& m) {
            auto& dd = m.osc[static_cast<size_t>(o)];
            dd.table = "file:" + p;
            dd.tableName = fs_stem(p);
            dd.frames.clear();
            st.params[static_cast<size_t>(oscP(o, O_Table))] = 0;
          });
        });
      if (ImGui::IsItemHovered()) c.a.setHint("Load a wavetable (.wav)");
    }
  }
  // the display (and the table map beside a table): the view's focus
  const float dy = kStageY + 48, dh = 230;
  if (table) {
    const float mw = 96, ww = LW - mw - 8;
    display(c, L, dy, ww, dh, 8, 4);
    if (aux) {
      const float lp = livePos(c, o);
      const float at = lp >= 0 ? lp : valf(c, oscP(o, O_WtPos));
      const bool morphed = (vali(c, oscP(o, O_Warp1Mode)) > 0 && valf(c, oscP(o, O_Warp1)) > 0.001f) ||
                           (vali(c, oscP(o, O_Warp2Mode)) > 0 && valf(c, oscP(o, O_Warp2)) > 0.001f);
      if (morphed) {  // before the morphs, faint behind the result
        drawCycle(c, cycleOf(c, *aux, o, at, false), L + 10, dy + 8, ww - 20, dh - 16, theme::withAlpha(c.P.textDim, 0.4f));
        text(c, L + ww - 150, dy + 8, "faint: before the morphs", c.P.textFaint, 10.0f, false);
      }
      const auto& cy = cycleOf(c, *aux, o, at);
      drawCycle(c, cy, L + 10, dy + 8, ww - 20, dh - 16, val(c, oscP(o, O_Enable)) >= 0.5 ? c.P.knob : c.P.textFaint);
    }
    const MipTable* mip = mipOf(c, oa, o);
    tableMap(c, o, mip, L + ww + 8, dy, mw, dh);
    if (mip && mip->frames > 1) {  // which frame plays, in words, at the display's corner
      const float lp = livePos(c, o), pos = lp >= 0 ? lp : valf(c, oscP(o, O_WtPos));
      char b[48];
      std::snprintf(b, sizeof b, "Frame %d of %d", 1 + static_cast<int>(std::lround(pos * (mip->frames - 1))), mip->frames);
      MonoText mono;
      text(c, L + 10, dy + 8, b, c.u.hoverK == oscP(o, O_WtPos) ? c.P.hot : c.P.textDim, 10.5f, false);
    }
  } else {
    display(c, L, dy, LW, dh, 8, 4);
    sampleDisplay(c, o, oa, L, dy, LW, dh, true);
  }
  // position (or the engine's main sweep)
  const float py = dy + dh + 8;
  if (type == OscGranular || type == OscSpectral) bar(c, oscP(o, O_Position), L, py, LW, 28, "Position");
  else if (type == OscMultisample) bar(c, oscP(o, O_MTimbre), L, py, LW, 28, "Timbre", BarCentre);
  else if (type == OscSample) bar(c, oscP(o, O_ScanRate), L, py, LW, 28, "Speed", BarCentre);
  else bar(c, oscP(o, O_WtPos), L, py, LW, 28, "Position");
  // cards: Morph, and the engine's own card
  const float cy0 = py + 40, ch = kStageB - 12 - cy0, cw = (LW - 12) * 0.5f;
  {
    const float y = card(c, L - 3, cy0, cw + 6, ch, "Morph");
    for (int i = 0; i < 2; ++i) {
      ImGui::PushID(i + 40);
      const float sy = y + 2 + i * 104.f;  // two equal slots: the morph, its amount, then its two spreads
      const int km = oscP(o, i == 0 ? O_Warp1Mode : O_Warp2Mode), ka = oscP(o, i == 0 ? O_Warp1 : O_Warp2),
                kv = oscP(o, i == 0 ? O_Warp1Var : O_Warp2Var), ks = oscP(o, i == 0 ? O_UniWarp1 : O_UniWarp2);
      reach(km);
      const int mode = vali(c, km);
      const std::string nm = morphDisplayName(mode);
      int step = 0;
      if (i == 1) hline(c, L + 10, L + cw - 10, sy - 8, theme::withAlpha(c.P.line, 0.6f));
      dropField(c, "mp", L + 10, sy, cw - 20, 28, nm.c_str(), [&] { morphPicker(c, o, i); }, true, &step);
      if (step) {  // step in Gridline's order, over the modes this engine allows
        std::vector<int> order;
        order.push_back(0);
        for (const auto& g : morphMenuGroups())
          for (int m : g.items)
            if (warpAllowed(type, m)) order.push_back(m);
        int at = 0;
        for (size_t q = 0; q < order.size(); ++q)
          if (order[q] == mode) at = static_cast<int>(q);
        setv(c, km, order[static_cast<size_t>((at + step + static_cast<int>(order.size())) % static_cast<int>(order.size()))]);
      }
      if (ImGui::IsItemHovered()) c.a.setHint(std::string("Morph ") + (i == 0 ? "1" : "2") + ": " + nm + " (click: choose, arrows: step)");
      const int dim = mode > 0 ? 0 : BarDim;
      bar(c, ka, L + 10, sy + 34, cw - 20, 28, "Amount", dim);
      const float hw = (cw - 28) * 0.5f;
      bar(c, kv, L + 10, sy + 68, hw, 26, "Variation", dim);
      bar(c, ks, L + 18 + hw, sy + 68, hw, 26, "Stack spread", BarCentre | dim);
      ImGui::PopID();
    }
  }
  {
    const float x = L + cw + 12 - 3, w = cw + 6;
    std::vector<FieldItem> items;
    const char* title2 = "Start and smoothing";
    std::vector<const char*> tabsN;
    int& tab = c.u.layerCard;
    const std::vector<FieldItem> region = {
        {oscP(o, O_Start), "Start"},           {oscP(o, O_End), "End"},           {oscP(o, O_Reverse), "Reverse"},
        {oscP(o, O_Slice), "Slice"},           {oscP(o, O_SliceMode), "Slice plays"}, {oscP(o, O_ScanRate), "Speed"},
        {oscP(o, O_ScanSync), "Tempo sync"},   {oscP(o, O_ScanBpm), "Division"},  {oscP(o, O_ScanKey), "Follows keys"}};
    const std::vector<FieldItem> loop = {{oscP(o, O_LoopMode), "Loop"},       {oscP(o, O_LoopStart), "Loop start"},
                                         {oscP(o, O_LoopEnd), "Loop end"},    {oscP(o, O_LoopXfade), "Crossfade"},
                                         {oscP(o, O_RelLoop), "Follows start"}};
    if (type == OscWavetable) {
      items = {{oscP(o, O_Phase), "Start phase"}, {oscP(o, O_RandPhase), "Phase scatter"}, {oscP(o, O_PhaseMemory), "Phase memory"},
               {oscP(o, O_Smooth), "Frame glide"}};
    } else if (type == OscMultisample) {
      title2 = "Multisample";
      items = {{oscP(o, O_MVelTrack), "Velocity"}, {oscP(o, O_RandPhase), "Scatter"}, {oscP(o, O_MEnvOn), "Own envelope"},
               {oscP(o, O_MDelay), "Wait"},       {oscP(o, O_MAttack), "Attack"},    {oscP(o, O_MHold), "Hold"},
               {oscP(o, O_MDecay), "Decay"},      {oscP(o, O_MSustain), "Sustain"},  {oscP(o, O_MRelease), "Release"}};
    } else if (type == OscSample) {
      tabsN = {"Region", "Loop"};
      tab = std::clamp(tab, 0, 1);
      items = tab == 0 ? region : loop;
    } else if (type == OscGranular) {
      tabsN = {"Grains", "Scatter", "Region", "Loop"};
      tab = std::clamp(tab, 0, 3);
      if (tab == 0)
        items = {{oscP(o, O_GDensity), "Density"},   {oscP(o, O_GDensMode), "Density by"}, {oscP(o, O_GLength), "Length"},
                 {oscP(o, O_GLenMode), "Length by"}, {oscP(o, O_GMaxGrains), "Most grains"}, {oscP(o, O_GReverse), "Reverse"},
                 {oscP(o, O_GWinShape), "Window"},   {oscP(o, O_GWinSkew), "Tilt"},       {oscP(o, O_GWinParam), "Edge"},
                 {oscP(o, O_GTrig), "Stack start"}};
      else if (tab == 1)
        items = {{oscP(o, O_GRndOffset), "Start"},  {oscP(o, O_GRndDir), "Direction"}, {oscP(o, O_GRndPitch), "Pitch"},
                 {oscP(o, O_GRndLength), "Length"}, {oscP(o, O_GRndPan), "Pan"},       {oscP(o, O_GRndGain), "Level"},
                 {oscP(o, O_GRndWin), "Window"},    {oscP(o, O_GRndSkew), "Tilt"},     {oscP(o, O_GRndWarp), "Morph 1"},
                 {oscP(o, O_GRndWarp2), "Morph 2"}};
      else items = tab == 2 ? region : loop;
    } else {  // spectral
      tabsN = {"Bands", "Region", "Loop"};
      tab = std::clamp(tab, 0, 2);
      if (tab == 0)
        items = {{oscP(o, O_SCut), "Shift"},  {oscP(o, O_SMix), "Mix"},          {oscP(o, O_SLo), "Low"},
                 {oscP(o, O_SHi), "High"},    {oscP(o, O_SPhaseLock), "Phase lock"}, {oscP(o, O_STransients), "Transients"}};
      else items = tab == 1 ? region : loop;
    }
    float y = card(c, x, cy0, w, ch, tabsN.empty() ? title2 : "");
    if (!tabsN.empty()) {
      const float tw = (w - 24) / static_cast<float>(tabsN.size());
      for (size_t i = 0; i < tabsN.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 90);
        if (button(c, "ct", x + 12 + tw * i, cy0 + 8, tw - 4, 24, tabsN[i], tab == static_cast<int>(i), 11.0f)) tab = static_cast<int>(i);
        ImGui::PopID();
      }
      y += 10;
    }
    // a short list is spaced to fill the card, level with the Morph rows beside it
    const float rowH = items.size() <= 5 ? std::min(46.f, (cy0 + ch - 14 - y) / static_cast<float>(items.size())) : 31.f;
    fieldGrid(c, x + 12, y, w - 24, items.size() <= 5 ? 1 : 2, rowH, items, items.size() <= 5 ? 28.f : 26.f);
  }
  // right: Out, Pitch, Stack (each card's secondary settings behind its More... button)
  {
    const float y0 = kStageY + 10, h = 200;
    const float y = card(c, R - 3, y0, RW + 6, h, "Out", oscP(o, O_Enable));
    knob(c, oscP(o, O_Level), R + 60, y + 36, 26, "Level");
    knob(c, oscP(o, O_Pan), R + 160, y + 36, 26, "Pan");
    routeCompact(c, o, R + 10, y + 92, RW - 20);
  }
  {
    const float y0 = kStageY + 222, h = 112;
    const float y = card(c, R - 3, y0, RW + 6, h, "Pitch");
    if (!cardMore(c, "pitchmore", R - 3, y0, RW + 6, h,
                  {{oscP(o, O_Coarse), "Offset"}, {oscP(o, O_HzOffset), "Shift in Hz"}, {oscP(o, O_Ratio), "Ratio"}, {oscP(o, O_PitchMode), "Pitch steps"}},
                  "More pitch settings: offset, shift in Hz, ratio, pitch steps")) {
      fieldGrid(c, R + 10, y + 2, RW - 20, 3, 34, {{oscP(o, O_Octave), "Octave"}, {oscP(o, O_Semi), "Semitone"}, {oscP(o, O_Fine), "Fine"}}, 26);
      chip(c, oscP(o, O_PitchTrack), R + 10, y + 38, 150, 26, "Follows keys");
    }
  }
  {
    const float y0 = kStageY + 346, h = kStageB - 12 - y0;
    const float y = card(c, R - 3, y0, RW + 6, h, "Stack");
    if (cardMore(c, "stackmore", R - 3, y0, RW + 6, h,
                 {{oscP(o, O_UniSpan), "Tilt"}, {oscP(o, O_UniRange), "Range"}, {oscP(o, O_UniRotate), "Order turn"},
                  {oscP(o, O_UniRandStart), "Start scatter"}, {oscP(o, O_UniWtPos), "Position spread"}, {tailP(T_UniMix), "Voice pan"}},
                 "More stack settings: tilt, range, order, start scatter, position spread, voice pan")) {
      ImGui::PopID();
      return;
    }
    text(c, R + 12, y + 8, "Voices", c.P.label, 11.5f, false);
    stepper(c, oscP(o, O_Unison), R + 70, y + 2, 64, 26);
    stackFan(c, o, R + 146, y + 2, RW - 156, 52);
    knob(c, oscP(o, O_UniDetune), R + 62, y + 88, 24, "Detune");
    knob(c, oscP(o, O_UniBlend), R + 182, y + 88, 24, "Outer level");
    knob(c, oscP(o, O_UniWidth), R + 302, y + 88, 24, "Width", KnobBipolar);
    fieldGrid(c, R + 10, y + 140, RW - 20, 2, 34, {{oscP(o, O_DetuneMode), "Spread"}, {oscP(o, O_UniStack), "Order"}}, 26);
  }
  ImGui::PopID();
}

// ================================================================== Root and Texture
void rootTextureView(Ctx& c) {
  // two equal cards (x 12..494, 506..988): the picture first, main dials, then fields and routing
  stageHead(c, "Root and Texture");
  ImGui::PushID("roottex");
  const float W = 482, T = kStageY + 10, H = kStageB - 12 - T;
  {  // Root
    const float x = 12;
    const float y = card(c, x - 3, T, W + 6, H, "Root", subP(S_Enable));
    const bool turned = cardMore(c, "rootmore", x - 3, T, W + 6, H - 120, {{subP(S_Phase), "Start phase"}, {subP(S_ContPhase), "Free running"}},
                                 "More Root settings: start phase, free running");
    if (!turned) {
    const int cur = vali(c, subP(S_Shape));
    reach(subP(S_Shape));
    static const char* const names[SubShapeCount] = {"Sine", "Round", "Triangle", "Saw", "Square", "Pulse"};
    const float tw = (W - 20 - 5 * 8) / 6;
    for (int i = 0; i < SubShapeCount; ++i) {
      const float bx = x + 10 + i * (tw + 8), by = y + 4;
      ImGui::PushID(i);
      ImGui::SetCursorScreenPos(c.p(bx, by));
      if (ImGui::InvisibleButton("##sh", c.F.sz(tw, 172))) setv(c, subP(S_Shape), i);
      const bool hv = ImGui::IsItemHovered(), on = i == cur;
      c.dl->AddRectFilled(c.p(bx, by), c.p(bx + tw, by + 150), on ? c.P.headOn : (hv ? c.P.panelHi : c.P.well), c.s(5));
      c.dl->AddRect(c.p(bx, by), c.p(bx + tw, by + 150), on ? c.P.accent : c.P.line, c.s(5), 0, std::max(1.f, c.s(on ? 1.6f : 1.f)));
      ImVec2 prev;
      for (int q = 0; q <= 32; ++q) {
        const float t = q / 32.f;
        float yv = 0;
        switch (i) {
          case SubSine: yv = std::sin(6.2832f * t); break;
          case SubRoundRect: yv = std::tanh(std::sin(6.2832f * t) * 4.f); break;
          case SubTriangle: yv = t < 0.25f ? t * 4 : t < 0.75f ? 2 - t * 4 : t * 4 - 4; break;
          case SubSaw: yv = 1 - 2 * t; break;
          case SubSquare: yv = t < 0.5f ? 0.85f : -0.85f; break;
          default: yv = t < 0.25f ? 0.85f : -0.85f; break;
        }
        const ImVec2 p = c.p(bx + 8 + t * (tw - 16), by + 75 - yv * 52);
        if (q) c.dl->AddLine(prev, p, on ? c.P.hot : c.P.knob, std::max(1.4f, c.s(1.8f)));
        prev = p;
      }
      textC(c, bx + tw * 0.5f, by + 156, names[i], on ? c.P.text : c.P.label, 11.5f, false);
      ImGui::PopID();
    }
    knob(c, subP(S_Level), x + 70, y + 262, 30, "Level");
    knob(c, subP(S_Pan), x + 170, y + 262, 30, "Pan");
    fieldGrid(c, x + 230, y + 220, W - 240, 1, 34, {{subP(S_Octave), "Octave"}, {subP(S_Coarse), "Offset st"}}, 26);
    chip(c, subP(S_PitchTrack), x + 230, y + 288, W - 240, 26, "Follows keys");
    }
    hline(c, x + 10, x + W - 10, T + H - 110, c.P.line);
    text(c, x + 10, T + H - 100, "Goes to", c.P.label, 11.5f, true);
    routeCompact(c, RtSub, x + 10, T + H - 76, W - 20);
  }
  {  // Texture
    const float x = 506;
    const float y = card(c, x - 3, T, W + 6, H, "Texture", noiseP(N_Enable));
    const bool turned = cardMore(c, "texmore", x - 3, T, W + 6, H - 120,
                                 {{noiseP(N_Phase), "Start"}, {noiseP(N_RandPhase), "Start scatter"}, {tailP(T_NKeyMin), "Lowest key"},
                                  {tailP(T_NKeyMax), "Highest key"}, {tailP(T_NKeyFold), "Fold keys in"}, {tailP(T_NoiseLaw), "Pitch law"}},
                                 "More Texture settings: start, start scatter, key range, fold keys in, pitch law");
    if (!turned) {
    const int t = vali(c, noiseP(N_Type));
    reach(noiseP(N_Type));
    const auto& names = noiseNames();
    std::string nm = t >= 0 && t < static_cast<int>(names.size()) ? names[static_cast<size_t>(t)] : "?";
    if (t == 0) nm = c.m.noiseFile.empty() ? std::string("Your file...") : refStem(c.m, c.m.noiseFile);
    int step = 0;
    dropField(c, "nt", x + 10, y + 2, W - 20, 28, nm.c_str(),
              [&] {
                for (int i = 1; i < static_cast<int>(names.size()); ++i)
                  if (ImGui::MenuItem(names[static_cast<size_t>(i)].c_str(), nullptr, i == t)) setv(c, noiseP(N_Type), i);
                ImGui::Separator();
                if (ImGui::MenuItem(i18n::tr("Load audio file...")))
                  openFile(c, i18n::tr("Load texture sample"), {".wav", ".flac", ".aif", ".aiff", ".mp3", ".ogg"},
                           [](App& a, const PluginTarget& tg, const std::string& p) {
                             editState(a, tg, "Load texture", [&](PluginState& st, Model& m) {
                               m.noiseFile = p;
                               st.params[static_cast<size_t>(noiseP(N_Type))] = 0;
                             });
                           });
              },
              true, &step);
    if (step) setv(c, noiseP(N_Type), std::clamp(t + step, 1, static_cast<int>(names.size()) - 1));
    const float dy = y + 38;
    display(c, x + 10, dy, W - 20, 150, 8, 2);
    {
      const auto aux = auxOf(c);
      const float* data = nullptr;
      size_t n = 0;
      const void* id = nullptr;
      if (t >= 1) {
        const auto& fn = factoryNoises();
        if (t - 1 < static_cast<int>(fn.size())) data = fn[static_cast<size_t>(t - 1)].L.data(), n = fn[static_cast<size_t>(t - 1)].L.size(), id = data;
      } else if (aux && aux->noiseFile) {
        data = aux->noiseFile->left.data(), n = aux->noiseFile->frames(), id = aux->noiseFile.get();
      }
      if (data && n) drawOverview(c, overview(static_cast<const Sample*>(id), data, n, 160), x + 14, dy + 2, W - 28, 146, theme::withAlpha(c.P.knob, 0.8f));
    }
    const float dx = (W - 20) / 4;
    knob(c, noiseP(N_Level), x + 10 + dx * 0.5f, y + 246, 26, "Level");
    knob(c, noiseP(N_Pan), x + 10 + dx * 1.5f, y + 246, 26, "Pan");
    knob(c, tailP(T_NoiseTone), x + 10 + dx * 2.5f, y + 246, 26, "Tone");
    knob(c, noiseP(N_Color), x + 10 + dx * 3.5f, y + 246, 26, "Colour");
    fieldGrid(c, x + 10, y + 296, W - 20, 2, 34,
              {{noiseP(N_Pitch), "Pitch"}, {noiseP(N_Fine), "Cents"}, {noiseP(N_OneShot), "Play once"}, {noiseP(N_PitchTrack), "Follows keys"}}, 26);
    }
    hline(c, x + 10, x + W - 10, T + H - 110, c.P.line);
    text(c, x + 10, T + H - 100, "Goes to", c.P.label, 11.5f, true);
    routeCompact(c, RtNoise, x + 10, T + H - 76, W - 20);
  }
  ImGui::PopID();
}

// ================================================================== Filters
void filtersView(Ctx& c) {
  // two equal cards (x 12..494, 506..988): type, the response (drag it), six dials, fields,
  // inputs and routing
  const float hx = stageHead(c, "Filters");
  (void)hx;
  field(c, tailP(T_KeyCentre), 536, kStageHeadY + 3, 220, 24, "Key follow centre");
  field(c, tailP(T_DriveMode), 766, kStageHeadY + 3, 222, 24, "Drive");
  const float W = 482, T = kStageY + 10, H = kStageB - 12 - T;
  for (int f = 0; f < kFilters; ++f) {
    ImGui::PushID(f + 10);
    const float x = f == 0 ? 12 : 506;
    float y = card(c, x - 3, T, W + 6, H, f == 0 ? "Filter 1" : "Filter 2", filtP(f, F_Enable));
    {
      const int t = vali(c, filtP(f, F_Type));
      reach(filtP(f, F_Type));
      int step = 0;
      const std::string nm = filterDisplayName(t);
      dropField(c, "ft", x + 110, T + 5, W - 120, 26, nm.c_str(), [&] { filterPicker(c, filtP(f, F_Type)); }, true, &step);
      if (ImGui::IsItemHovered()) c.a.setHint("Filter type: " + nm + " (click: choose, arrows: step)");
      if (step) {
        std::vector<int> order;
        for (const auto& g : filterMenuGroups())
          for (int m : g.items) order.push_back(m);
        int at = 0;
        for (size_t q = 0; q < order.size(); ++q)
          if (order[q] == t) at = static_cast<int>(q);
        setv(c, filtP(f, F_Type), order[static_cast<size_t>((at + step + static_cast<int>(order.size())) % static_cast<int>(order.size()))]);
      }
    }
    display(c, x + 10, y + 6, W - 20, 176, 10, 4);
    filterCurve(c, f, x + 10, y + 6, W - 20, 176, true);
    y += 196;
    static const char* const vl[][2] = {{"BODY", "Body"},   {"GAIN", "Gain"},   {"MORPH", "Blend"},     {"CUT", "Tone cut"}, {"SPREAD", "Spread"},
                                        {"SMOOTH", "Smooth"}, {"SIZE", "Size"}, {"TONE", "Tone"},       {"BASS", "Bass"},    {"VOWEL", "Vowel"},
                                        {"WIDTH", "Width"}, {"LOW/HIGH", "Low / high"}, {"SHAPE", "Shape"}, {"SLOPE", "Slope"}};
    const char* raw = filterVarLabel(vali(c, filtP(f, F_Type)));
    const char* charName = "Character";
    for (const auto& p : vl)
      if (raw && std::strcmp(raw, p[0]) == 0) charName = p[1];
    const float dx = (W - 20) / 6;
    const int ks[6] = {F_Freq, F_Res, F_Drive, F_Var, F_Stereo, F_Wet};
    const char* ls[6] = {"Cutoff", "Resonance", "Drive", charName, "Stereo", "Wet"};
    for (int i = 0; i < 6; ++i) knob(c, filtP(f, ks[i]), x + 10 + dx * (i + 0.5f), y + 30, i < 2 ? 26 : 22, ls[i], i == 4 ? KnobBipolar : 0);
    y += 82;
    y = fieldGrid(c, x + 10, y, W - 20, 3, 34,
                  {{filtP(f, F_X), "Morph X"}, {filtP(f, F_Y), "Morph Y"}, {filtP(f, F_Level), "Level"},
                   {filtP(f, F_KeyTrack), "Follows keys"}, {tailP(f == 0 ? T_Pad1 : T_Pad2), "Input pad"}}, 26);
    hline(c, x + 10, x + W - 10, y + 2, c.P.line);
    text(c, x + 10, y + 12, "Inputs", c.P.label, 11.5f, true);
    {
      const char* lab[5] = {"Layer 1", "Layer 2", "Layer 3", "Root", "Texture"};
      const int rs[5] = {RtA, RtB, RtC, RtSub, RtNoise};
      const float bw = (W - 20 - 64) / 5;
      for (int i = 0; i < 5; ++i) {
        ImGui::PushID(i + 60);
        const bool on = feeds(c, rs[i], f);
        if (button(c, "src", x + 74 + bw * i, y + 8, bw - 4, 26, lab[i], on, 11.0f)) toggleFeed(c, rs[i], f);
        if (ImGui::IsItemHovered()) c.a.setHint(std::string(lab[i]) + (on ? " feeds this filter (click: take it out)" : " does not feed this filter (click: add it)"));
        ImGui::PopID();
      }
    }
    y += 42;
    text(c, x + 10, y + 6, "Goes to", c.P.label, 11.5f, true);
    routeCompact(c, f == 0 ? RtF1 : RtF2, x + 10, y + 28, W - 20);
    if (val(c, filtP(f, F_Enable)) < 0.5)
      c.dl->AddRectFilled(c.p(x, T + 36), c.p(x + W, kStageB - 16), theme::withAlpha(c.P.frame, 0.35f), c.s(5));
    ImGui::PopID();
  }
}


// ================================================================== Panels: 1 Sources, 2 Tone
namespace {
float rootWave(int shape, float t) {
  switch (shape) {
    case SubSine: return std::sin(6.2832f * t);
    case SubRoundRect: return std::tanh(std::sin(6.2832f * t) * 4.f);
    case SubTriangle: return t < 0.25f ? t * 4 : t < 0.75f ? 2 - t * 4 : t * 4 - 4;
    case SubSaw: return 1 - 2 * t;
    case SubSquare: return t < 0.5f ? 0.85f : -0.85f;
    default: return t < 0.25f ? 0.85f : -0.85f;
  }
}
void waveLine(Ctx& c, int shape, float x, float y, float w, float h, ImU32 col, float th) {
  ImVec2 prev;
  for (int q = 0; q <= 40; ++q) {
    const float t = q / 40.f;
    const ImVec2 p = c.p(x + t * w, y + h * 0.5f - rootWave(shape, t) * h * 0.4f);
    if (q) c.dl->AddLine(prev, p, col, std::max(1.2f, c.s(th)));
    prev = p;
  }
}
void textureMini(Ctx& c, float x, float y, float w, float h) {
  const float col = valf(c, noiseP(N_Color));
  for (int i = 0; i < 20; ++i) {
    const float t = (i + 0.5f) / 20.f;
    const float hgt = (0.25f + 0.6f * std::fabs(std::sin(i * 12.9898f + col * 3.f))) * h * 0.42f;
    c.dl->AddLine(c.p(x + t * w, y + h * 0.5f - hgt), c.p(x + t * w, y + h * 0.5f + hgt), theme::withAlpha(c.P.knob, 0.75f), std::max(1.0f, c.s(1.0f)));
  }
}
// The five sources as chips: on / off light, name and a live picture; a click shows that source.
void sourceChips(Ctx& c, float x, float y, float w) {
  const float gap = 6, cw = (w - gap * 4) / 5, ch = 52;
  static const char* const names[5] = {"Layer 1", "Layer 2", "Layer 3", "Root", "Texture"};
  const int en[5] = {oscP(0, O_Enable), oscP(1, O_Enable), oscP(2, O_Enable), subP(S_Enable), noiseP(N_Enable)};
  for (int i = 0; i < 5; ++i) {
    const float bx = x + i * (cw + gap);
    ImGui::PushID(i + 5100);
    const bool on = val(c, en[i]) >= 0.5, sel = c.u.src == i;
    reach(en[i]);
    ImGui::SetCursorScreenPos(c.p(bx, y));
    if (ImGui::InvisibleButton("##on", c.F.sz(20, 22))) setv(c, en[i], on ? 0 : 1);
    if (ImGui::IsItemHovered()) c.a.setHint(std::string(names[i]) + (on ? ": on (click: off)" : ": off (click: on)"));
    ImGui::SetCursorScreenPos(c.p(bx, y));
    const bool click = ImGui::InvisibleButton("##chip", c.F.sz(cw, ch));
    const bool hv = ImGui::IsItemHovered();
    if (click) {
      c.u.src = i;
      if (i < 3) c.u.layer = i, c.u.view = VLayer;
      else c.u.view = VRootTex;
    }
    if (hv) c.a.setHint(std::string(names[i]) + ": click to show it");
    const ImVec2 a = c.p(bx, y), b = c.p(bx + cw, y + ch);
    c.dl->AddRectFilled(a, b, sel ? c.P.headOn : (hv ? c.P.panelHi : c.P.field), c.s(5));
    c.dl->AddRect(a, b, sel ? c.P.accent : c.P.line, c.s(5), 0, std::max(1.f, c.s(sel ? 1.6f : 1.f)));
    const ImVec2 dc = c.p(bx + 10, y + 11);
    if (on) c.dl->AddCircleFilled(dc, c.s(3.5f), c.P.good, 12);
    else c.dl->AddCircle(dc, c.s(3.5f), c.P.boxEdge, 12, std::max(1.f, c.s(1.1f)));
    text(c, bx + 18, y + 4, names[i], on ? c.P.text : c.P.textDim, 11.0f, true);
    const float px = bx + 5, py = y + 24, pw = cw - 10, ph = ch - 29;
    c.dl->AddRectFilled(c.p(px, py), c.p(px + pw, py + ph), c.P.well, c.s(3));
    c.dl->PushClipRect(c.p(px, py), c.p(px + pw, py + ph), true);
    if (i < 3) cycleMini(c, i, px, py, pw, ph);
    else if (i == 3) waveLine(c, vali(c, subP(S_Shape)), px + 3, py, pw - 6, ph, on ? c.P.knob : c.P.textFaint, 1.4f);
    else textureMini(c, px + 2, py, pw - 4, ph);
    c.dl->PopClipRect();
    if (!on) c.dl->AddRectFilled(c.p(px, py), c.p(px + pw, py + ph), theme::withAlpha(c.P.frame, 0.45f), c.s(3));
    ImGui::PopID();
  }
}
// The output of source route r: level and pan dials, where it goes; the sends in the details.
float outputSection(Ctx& c, int kLevel, int kPan, int r, float x, float y, float w) {
  const bool det = section(c, "out", x, y, w, "Output", "", true);
  knob(c, kLevel, x + 30, y + 22, 20, "Level", KnobValue);
  knob(c, kPan, x + 96, y + 22, 20, "Pan", KnobValue | KnobBipolar);
  text(c, x + 140, y, "Goes to", c.P.label, 11.0f, true);
  routeCompact(c, r, x + 140, y + 18, w - 140);
  y += 78;
  if (det)
    y = fieldGrid(c, x, y, w, 2, 32, {{routeP(r, R_Bus1), "Send A"}, {routeP(r, R_Bus2), "Send B"}, {tailP(T_RawA + r), "Skip Envelope 1"}}, 26);
  return y;
}
std::vector<FieldItem> engineItems(int o, int type, const char*& title) {
  const std::vector<FieldItem> region = {
      {oscP(o, O_Start), "Start"},         {oscP(o, O_End), "End"},           {oscP(o, O_Reverse), "Reverse"},
      {oscP(o, O_Slice), "Slice"},         {oscP(o, O_SliceMode), "Slice plays"}, {oscP(o, O_ScanRate), "Speed"},
      {oscP(o, O_ScanSync), "Tempo sync"}, {oscP(o, O_ScanBpm), "Division"},  {oscP(o, O_ScanKey), "Follows keys"}};
  const std::vector<FieldItem> loop = {{oscP(o, O_LoopMode), "Loop"},    {oscP(o, O_LoopStart), "Loop start"}, {oscP(o, O_LoopEnd), "Loop end"},
                                       {oscP(o, O_LoopXfade), "Crossfade"}, {oscP(o, O_RelLoop), "Follows start"}};
  std::vector<FieldItem> v;
  auto add = [&](const std::vector<FieldItem>& a) { v.insert(v.end(), a.begin(), a.end()); };
  if (type == OscWavetable) {
    title = "Start and smoothing";
    v = {{oscP(o, O_Phase), "Start phase"}, {oscP(o, O_RandPhase), "Phase scatter"}, {oscP(o, O_PhaseMemory), "Phase memory"},
         {oscP(o, O_Smooth), "Frame glide"}};
  } else if (type == OscMultisample) {
    title = "Multisample";
    v = {{oscP(o, O_MVelTrack), "Velocity"}, {oscP(o, O_RandPhase), "Scatter"}, {oscP(o, O_MEnvOn), "Own envelope"},
         {oscP(o, O_MDelay), "Wait"},       {oscP(o, O_MAttack), "Attack"},    {oscP(o, O_MHold), "Hold"},
         {oscP(o, O_MDecay), "Decay"},      {oscP(o, O_MSustain), "Sustain"},  {oscP(o, O_MRelease), "Release"}};
  } else if (type == OscSample) {
    title = "Sample region and loop";
    add(region), add(loop);
  } else if (type == OscGranular) {
    title = "Grains";
    v = {{oscP(o, O_GDensity), "Density"},   {oscP(o, O_GDensMode), "Density by"}, {oscP(o, O_GLength), "Length"},
         {oscP(o, O_GLenMode), "Length by"}, {oscP(o, O_GMaxGrains), "Most grains"}, {oscP(o, O_GReverse), "Reverse"},
         {oscP(o, O_GWinShape), "Window"},   {oscP(o, O_GWinSkew), "Tilt"},       {oscP(o, O_GWinParam), "Edge"},
         {oscP(o, O_GTrig), "Stack start"},  {oscP(o, O_GRndOffset), "Scatter start"}, {oscP(o, O_GRndDir), "Scatter direction"},
         {oscP(o, O_GRndPitch), "Scatter pitch"}, {oscP(o, O_GRndLength), "Scatter length"}, {oscP(o, O_GRndPan), "Scatter pan"},
         {oscP(o, O_GRndGain), "Scatter level"}, {oscP(o, O_GRndWin), "Scatter window"}, {oscP(o, O_GRndSkew), "Scatter tilt"},
         {oscP(o, O_GRndWarp), "Scatter morph 1"}, {oscP(o, O_GRndWarp2), "Scatter morph 2"}};
    add(region), add(loop);
  } else {
    title = "Spectral bands";
    v = {{oscP(o, O_SCut), "Shift"},  {oscP(o, O_SMix), "Mix"},          {oscP(o, O_SLo), "Low"},
         {oscP(o, O_SHi), "High"},    {oscP(o, O_SPhaseLock), "Phase lock"}, {oscP(o, O_STransients), "Transients"}};
    add(region), add(loop);
  }
  return v;
}
}  // namespace

float layerColumn(Ctx& c, int o, float x, float y, float w) {
  ImGui::PushID(o + 300);
  const auto aux = auxOf(c);
  const OscAux* oa = oscAux(aux.get(), o);
  const int type = vali(c, oscP(o, O_Type));
  const bool table = showsTable(c, oa, o);
  const bool detail = showDetails(c);
  if (detail) {
    segments(c, oscP(o, O_Type), x, y, w, 26, {"Wavetable", "Multisample", "Sample", "Grains", "Spectral"});
    y += 34;
  }
  {  // what it plays: table / sample / multisample, Edit, Load
    std::string nm;
    const auto& d = c.m.osc[static_cast<size_t>(o)];
    if (type == OscMultisample) nm = d.sfz.empty() ? std::string("No multisample") : refStem(c.m, d.sfz);
    else if (type != OscWavetable && !d.sample.empty()) nm = refStem(c.m, d.sample);
    else if (!d.table.empty()) nm = d.tableName.empty() ? std::string("Custom table") : d.tableName;
    else {
      const int t = vali(c, oscP(o, O_Table));
      const auto& n = factoryTableNames();
      nm = t >= 1 && t <= static_cast<int>(n.size()) ? n[static_cast<size_t>(t - 1)] : "No table";
    }
    reach(oscP(o, O_Table));
    int step = 0;
    const bool slim = w < 260;  // a narrow block: smaller Edit and Load buttons, more room for the name
    const float ew = slim ? 40 : 56, lw = slim ? 46 : 62;
    dropField(c, "tbl", x, y, table ? w - ew - lw - 10 : w, 28, nm.c_str(), [&] { tableMenu(c, o); }, true, &step);
    if (step && type == OscWavetable) {
      const int n = static_cast<int>(factoryTableNames().size());
      const int t = vali(c, oscP(o, O_Table));
      c.m.osc[static_cast<size_t>(o)].table.clear();
      commitParams(c, "Choose table", {{oscP(o, O_Table), ((std::max(1, t) - 1 + step + n) % n) + 1}});
    }
    if (table) {
      if (button(c, "edit", x + w - lw - 4 - ew, y, ew, 28, "Edit", false, 11.5f)) c.u.wtOsc = o, openView(c.u, VTable);
      if (ImGui::IsItemHovered()) c.a.setHint("Table editor: draw, compute and arrange the frames of this table");
      if (button(c, "load", x + w - lw, y, lw, 28, slim ? "Load" : "Load...", false, 11.5f))
        openFile(c, i18n::tr("Load wavetable"), {".wav"}, [o](App& a, const PluginTarget& t, const std::string& p) {
          editState(a, t, "Load wavetable", [&](PluginState& st, Model& m) {
            auto& dd = m.osc[static_cast<size_t>(o)];
            dd.table = "file:" + p;
            dd.tableName = fs_stem(p);
            dd.frames.clear();
            st.params[static_cast<size_t>(oscP(o, O_Table))] = 0;
          });
        });
      if (ImGui::IsItemHovered()) c.a.setHint("Load a wavetable (.wav)");
    }
    y += 36;
  }
  {  // the picture, edited in place: drag across the wave to move through the table, the frames in
     // depth (3D) or the playing wave with the wave before the morphs faint behind it (2D), the
     // morphs and their amounts on it, the table map beside it
    const float dh = detail ? 112 : fitPic(c, 104);
    if (table) {
      const float mw = 44, ww = w - mw - 6;
      display(c, x, y, ww, dh, 8, 4);
      const MipTable* mip = mipOf(c, oa, o);
      const bool on = val(c, oscP(o, O_Enable)) >= 0.5;
      bool& deep = c.u.wt3d[o];
      const float lp = livePos(c, o), at = lp >= 0 ? lp : valf(c, oscP(o, O_WtPos));
      // the 2D / 3D switch first (it takes the pointer before the scrub area under it)
      if (mip && mip->frames > 1) {
        if (button(c, "deep", x + ww - 40, y + 4, 36, 18, deep ? "3D" : "2D", deep, 9.5f)) deep = !deep;
        if (ImGui::IsItemHovered()) c.a.setHint(deep ? "Frames in depth (click: the playing wave)" : "The playing wave (click: the frames in depth)");
      }
      ImGui::SetCursorScreenPos(c.p(x, y));
      ImGui::InvisibleButton("##scrub", c.F.sz(ww, dh));
      markPicture();
      if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        const float mx = (ImGui::GetIO().MousePos.x - c.p(x + 8, 0).x) / c.s(ww - 16);
        setv(c, oscP(o, O_WtPos), std::clamp(static_cast<double>(mx), 0.0, 1.0), true);
      }
      if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        c.a.setHint("Drag across the wave to move through the table (the map at the right shows where)");
      }
      reach(oscP(o, O_WtPos));
      if (aux && deep && mip && mip->frames > 1) {
        tableDepth(c, o, mip, x + 8, y + 16, ww - 16, dh - 22, on, true);
      } else if (aux) {
        // the playing wave, morphing live as its links move the morphs; the wave before the
        // morphs faint behind it
        const float a1 = liveMorph(c, o, 0), a2 = liveMorph(c, o, 1);
        const bool morphed = (vali(c, oscP(o, O_Warp1Mode)) > 0 && std::max(a1, valf(c, oscP(o, O_Warp1))) > 0.001f) ||
                             (vali(c, oscP(o, O_Warp2Mode)) > 0 && std::max(a2, valf(c, oscP(o, O_Warp2))) > 0.001f);
        if (morphed) drawCycle(c, cycleOf(c, *aux, o, at, false), x + 8, y + 18, ww - 16, dh - 40, theme::withAlpha(c.P.textDim, 0.4f));
        drawCycle(c, cycleOf(c, *aux, o, at, true, a1, a2), x + 8, y + 18, ww - 16, dh - 40, on ? c.P.knob : c.P.textFaint);
      }
      tableMap(c, o, mip, x + ww + 6, y, mw, dh);
      if (mip && mip->frames > 1) {
        char b[48];
        std::snprintf(b, sizeof b, "Frame %d of %d", 1 + static_cast<int>(std::lround(at * (mip->frames - 1))), mip->frames);
        MonoText mono;
        text(c, x + 8, y + 5, b, c.u.hoverK == oscP(o, O_WtPos) ? c.P.hot : c.P.textDim, 10.5f, false);
      }
      // the morphs on the wave: name and amount, live
      float px = x + 6;
      for (int i = 0; i < 2; ++i) {
        const int mode = vali(c, oscP(o, i == 0 ? O_Warp1Mode : O_Warp2Mode));
        if (mode <= 0) continue;
        char b[64];
        const int pc = static_cast<int>(std::lround(valf(c, oscP(o, i == 0 ? O_Warp1 : O_Warp2)) * 100));
        std::snprintf(b, sizeof b, "%s %d%%", morphDisplayName(mode).c_str(), pc);
        float tw = textW(c, b, 9.5f, false);
        if (px + tw + 12 > x + ww - 6) {
          std::snprintf(b, sizeof b, "Morph %d %d%%", i + 1, pc);
          tw = textW(c, b, 9.5f, false);
        }
        if (px + tw + 12 > x + ww - 6) break;
        c.dl->AddRectFilled(c.p(px, y + dh - 19), c.p(px + tw + 10, y + dh - 5), theme::withAlpha(c.P.panel, 0.85f), c.s(7));
        text(c, px + 5, y + dh - 18, b, c.P.accentText, 9.5f, false);
        px += tw + 16;
      }
    } else {
      display(c, x, y, w, dh, 8, 4);
      sampleDisplay(c, o, oa, x, y, w, dh, true);
    }
    y += dh + 8;
  }
  if (!detail) {  // Simple: the main dials (every other control with More, or in Full)
    const int kPos = type == OscGranular || type == OscSpectral ? oscP(o, O_Position) : type == OscMultisample ? oscP(o, O_MTimbre)
                     : type == OscSample                        ? oscP(o, O_ScanRate) : oscP(o, O_WtPos);
    const char* posName = type == OscMultisample ? "Timbre" : type == OscSample ? "Speed" : "Position";
    const int ks[6] = {kPos, oscP(o, O_Warp1), oscP(o, O_Octave), oscP(o, O_Unison), oscP(o, O_UniDetune), oscP(o, O_Level)};
    const char* ls[6] = {posName, "Morph", "Octave", "Voices", "Detune", "Level"};
    const float dx = w / 6;
    y = dialRow(y);
    for (int i = 0; i < 6; ++i) knob(c, ks[i], x + dx * (i + 0.5f), y + 20, 16, ls[i], KnobValue | (i == 2 ? KnobBipolar : 0));
    y += kDialRowH;
    ImGui::PopID();
    return y;
  }
  if (type == OscGranular || type == OscSpectral) bar(c, oscP(o, O_Position), x, y, w, 26, "Position");
  else if (type == OscMultisample) bar(c, oscP(o, O_MTimbre), x, y, w, 26, "Timbre", BarCentre);
  else if (type == OscSample) bar(c, oscP(o, O_ScanRate), x, y, w, 26, "Speed", BarCentre);
  else bar(c, oscP(o, O_WtPos), x, y, w, 26, "Position");
  y += 36;
  // Shape: the two morphs (each a choice and an amount) and, for a wavetable, how each note starts
  // and how frames glide. One section folded behind More in Simple, so the column fits.
  {
    const int m1 = vali(c, oscP(o, O_Warp1Mode)), m2 = vali(c, oscP(o, O_Warp2Mode));
    const char* engTitle = "";
    const auto items = engineItems(o, type, engTitle);
    const bool wt = type == OscWavetable;
    std::string sum = (m1 || m2) ? "Morph: " + morphDisplayName(m1 ? m1 : m2) : std::string("No morph");
    const bool det = section(c, "shape", x, y, w, "Shape", sum, true);
    if (det) {
      text(c, x, y, "Morph", c.P.label, 11.0f, false);
      y += 18;
      for (int i = 0; i < 2; ++i) {
        ImGui::PushID(i + 40);
        const int km = oscP(o, i == 0 ? O_Warp1Mode : O_Warp2Mode), ka = oscP(o, i == 0 ? O_Warp1 : O_Warp2);
        reach(km);
        const int mode = vali(c, km);
        const std::string nm = morphDisplayName(mode);
        int step = 0;
        const float pw = w * 0.52f;
        dropField(c, "mp", x, y, pw, 26, nm.c_str(), [&] { morphPicker(c, o, i); }, true, &step);
        if (step) {
          std::vector<int> order;
          order.push_back(0);
          for (const auto& g : morphMenuGroups())
            for (int m : g.items)
              if (warpAllowed(type, m)) order.push_back(m);
          int at = 0;
          for (size_t q = 0; q < order.size(); ++q)
            if (order[q] == mode) at = static_cast<int>(q);
          setv(c, km, order[static_cast<size_t>((at + step + static_cast<int>(order.size())) % static_cast<int>(order.size()))]);
        }
        if (ImGui::IsItemHovered()) c.a.setHint(std::string("Morph ") + (i == 0 ? "1" : "2") + ": " + nm + " (click: choose, arrows: step)");
        bar(c, ka, x + pw + 6, y, w - pw - 6, 26, "Amount", mode > 0 ? 0 : BarDim);
        y += 32;
        ImGui::PopID();
      }
      y = fieldGrid(c, x, y, w, 2, 32,
                    {{oscP(o, O_Warp1Var), "Variation 1"}, {oscP(o, O_UniWarp1), "Stack spread 1"},
                     {oscP(o, O_Warp2Var), "Variation 2"}, {oscP(o, O_UniWarp2), "Stack spread 2"}}, 26);
      if (wt) {
        text(c, x, y + 2, engTitle, c.P.label, 11.0f, false);
        y += 20;
        y = fieldGrid(c, x, y, w, 2, 32, {items[0], items[1]}, 26);
        y = fieldGrid(c, x, y, w, 1, 32, {items[2], items[3]}, 26);
      }
    }
    sectionEnd(c, x, y, w);
    if (!wt) {  // the engine's own settings (sample region, grains, bands): their own section
      char es[24];
      std::snprintf(es, sizeof es, "%d settings", static_cast<int>(items.size()));
      if (section(c, "eng", x, y, w, engTitle, es, true)) y = fieldGrid(c, x, y, w, 2, 32, items, 26);
      sectionEnd(c, x, y, w);
    }
  }
  // Pitch
  {
    const bool det = section(c, "pitch", x, y, w, "Pitch", "", true);
    y = fieldGrid(c, x, y, w, 2, 32, {{oscP(o, O_Octave), "Octave"}, {oscP(o, O_Semi), "Semitone"}, {oscP(o, O_Fine), "Fine"}}, 26);
    chip(c, oscP(o, O_PitchTrack), x + (w + 8) * 0.5f, y - 32, (w - 8) * 0.5f, 26, "Follows keys");
    if (det)
      y = fieldGrid(c, x, y, w, 2, 32,
                    {{oscP(o, O_Coarse), "Offset"}, {oscP(o, O_HzOffset), "Shift in Hz"}, {oscP(o, O_Ratio), "Ratio"}, {oscP(o, O_PitchMode), "Pitch steps"}},
                    26);
    sectionEnd(c, x, y, w);
  }
  // Unison: the voices in pitch and stereo, how many, how far apart
  {
    char sum[24];
    std::snprintf(sum, sizeof sum, "%d voice%s", std::max(1, vali(c, oscP(o, O_Unison))), vali(c, oscP(o, O_Unison)) > 1 ? "s" : "");
    const bool det = section(c, "uni", x, y, w, "Unison", sum, true);
    const float fw = std::min(150.f, w * 0.45f);
    stackFan(c, o, x, y, fw, 80);
    const float rx = x + fw + 8, rw = w - fw - 8;
    text(c, rx, y + 5, "Voices", c.P.label, 11.0f, false);
    stepper(c, oscP(o, O_Unison), rx + rw - 64, y, 64, 24);
    bar(c, oscP(o, O_UniDetune), rx, y + 28, rw, 24, "Detune");
    bar(c, oscP(o, O_UniWidth), rx, y + 56, rw, 24, "Width", BarCentre);
    y += 88;
    if (det)
      y = fieldGrid(c, x, y, w, 2, 32,
                    {{oscP(o, O_UniBlend), "Outer level"}, {oscP(o, O_DetuneMode), "Spread"}, {oscP(o, O_UniStack), "Order"},
                     {oscP(o, O_UniSpan), "Tilt"}, {oscP(o, O_UniRange), "Range"}, {oscP(o, O_UniRotate), "Order turn"},
                     {oscP(o, O_UniRandStart), "Start scatter"}, {oscP(o, O_UniWtPos), "Position spread"}, {tailP(T_UniMix), "Voice pan"}},
                    26);
    sectionEnd(c, x, y, w);
  }
  y = outputSection(c, oscP(o, O_Level), oscP(o, O_Pan), o, x, y, w);
  ImGui::PopID();
  return y;
}
float rootColumn(Ctx& c, float x, float y, float w) {
  ImGui::PushID("rootcol");
  if (!showDetails(c)) {  // Simple: the six shapes in a row, then the main dials
    const int cur = vali(c, subP(S_Shape));
    reach(subP(S_Shape));
    static const char* const names[SubShapeCount] = {"Sine", "Round", "Triangle", "Saw", "Square", "Pulse"};
    const float gap = 4, tw = (w - gap * 5) / 6, th = 36;
    for (int i = 0; i < SubShapeCount; ++i) {
      const float bx = x + i * (tw + gap);
      ImGui::PushID(i);
      ImGui::SetCursorScreenPos(c.p(bx, y));
      if (ImGui::InvisibleButton("##sh", c.F.sz(tw, th))) setv(c, subP(S_Shape), i);
      const bool hv = ImGui::IsItemHovered(), on = i == cur;
      if (hv) c.a.setHint(std::string("Root shape: ") + names[i]);
      c.dl->AddRectFilled(c.p(bx, y), c.p(bx + tw, y + th), on ? c.P.headOn : (hv ? c.P.panelHi : c.P.well), c.s(4));
      c.dl->AddRect(c.p(bx, y), c.p(bx + tw, y + th), on ? c.P.accent : c.P.line, c.s(4), 0, std::max(1.f, c.s(on ? 1.6f : 1.f)));
      waveLine(c, i, bx + 5, y + 6, tw - 10, th - 12, on ? c.P.hot : c.P.knob, 1.4f);
      ImGui::PopID();
    }
    y += th + 8;
    text(c, x, y, names[std::clamp(cur, 0, SubShapeCount - 1)], c.P.label, 11.0f, true);
    y += 20;
    const float dx = w / 4;
    y = dialRow(y);
    knob(c, subP(S_Octave), x + dx * 0.5f, y + 20, 16, "Octave", KnobValue | KnobBipolar);
    knob(c, subP(S_Coarse), x + dx * 1.5f, y + 20, 16, "Offset", KnobValue | KnobBipolar);
    knob(c, subP(S_Level), x + dx * 2.5f, y + 20, 16, "Level", KnobValue);
    knob(c, subP(S_Pan), x + dx * 3.5f, y + 20, 16, "Pan", KnobValue | KnobBipolar);
    y += kDialRowH;
    ImGui::PopID();
    return y;
  }
  section(c, "shape", x, y, w, "Shape", "", false);
  {
    const int cur = vali(c, subP(S_Shape));
    reach(subP(S_Shape));
    static const char* const names[SubShapeCount] = {"Sine", "Round", "Triangle", "Saw", "Square", "Pulse"};
    const float gap = 6, tw = (w - gap * 2) / 3, th = 58;
    for (int i = 0; i < SubShapeCount; ++i) {
      const float bx = x + (i % 3) * (tw + gap), by = y + (i / 3) * (th + gap);
      ImGui::PushID(i);
      ImGui::SetCursorScreenPos(c.p(bx, by));
      if (ImGui::InvisibleButton("##sh", c.F.sz(tw, th))) setv(c, subP(S_Shape), i);
      const bool hv = ImGui::IsItemHovered(), on = i == cur;
      if (hv) c.a.setHint(std::string("Root shape: ") + names[i]);
      c.dl->AddRectFilled(c.p(bx, by), c.p(bx + tw, by + th), on ? c.P.headOn : (hv ? c.P.panelHi : c.P.well), c.s(5));
      c.dl->AddRect(c.p(bx, by), c.p(bx + tw, by + th), on ? c.P.accent : c.P.line, c.s(5), 0, std::max(1.f, c.s(on ? 1.6f : 1.f)));
      waveLine(c, i, bx + 10, by + 4, tw - 20, th - 22, on ? c.P.hot : c.P.knob, 1.6f);
      textC(c, bx + tw * 0.5f, by + th - 17, names[i], on ? c.P.text : c.P.label, 11.0f, false);
      ImGui::PopID();
    }
    y += 2 * th + gap;
  }
  sectionEnd(c, x, y, w);
  {
    const bool det = section(c, "rp", x, y, w, "Pitch", "", true);
    y = fieldGrid(c, x, y, w, 2, 32, {{subP(S_Octave), "Octave"}, {subP(S_Coarse), "Offset"}}, 26);
    chip(c, subP(S_PitchTrack), x, y, 150, 26, "Follows keys");
    y += 32;
    if (det) y = fieldGrid(c, x, y, w, 2, 32, {{subP(S_Phase), "Start phase"}, {subP(S_ContPhase), "Free running"}}, 26);
    sectionEnd(c, x, y, w);
  }
  y = outputSection(c, subP(S_Level), subP(S_Pan), RtSub, x, y, w);
  ImGui::PopID();
  return y;
}
float textureColumn(Ctx& c, float x, float y, float w) {
  ImGui::PushID("texcol");
  const bool det = true;
  const int t = vali(c, noiseP(N_Type));
  reach(noiseP(N_Type));
  const auto& names = noiseNames();
  std::string nm = t >= 0 && t < static_cast<int>(names.size()) ? names[static_cast<size_t>(t)] : "?";
  if (t == 0) nm = c.m.noiseFile.empty() ? std::string("Your file...") : refStem(c.m, c.m.noiseFile);
  int step = 0;
  dropField(c, "nt", x, y, w, 28, nm.c_str(),
            [&] {
              for (int i = 1; i < static_cast<int>(names.size()); ++i)
                if (ImGui::MenuItem(names[static_cast<size_t>(i)].c_str(), nullptr, i == t)) setv(c, noiseP(N_Type), i);
              ImGui::Separator();
              if (ImGui::MenuItem(i18n::tr("Load audio file...")))
                openFile(c, i18n::tr("Load texture sample"), {".wav", ".flac", ".aif", ".aiff", ".mp3", ".ogg"},
                         [](App& a, const PluginTarget& tg, const std::string& p) {
                           editState(a, tg, "Load texture", [&](PluginState& st, Model& m) {
                             m.noiseFile = p;
                             st.params[static_cast<size_t>(noiseP(N_Type))] = 0;
                           });
                         });
            },
            true, &step);
  if (step) setv(c, noiseP(N_Type), std::clamp(t + step, 1, static_cast<int>(names.size()) - 1));
  y += 36;
  const bool simple = !showDetails(c);
  {
    const float dh = simple ? fitPic(c, 64) : 110;
    display(c, x, y, w, dh, 8, 2);
    const auto aux = auxOf(c);
    const float* data = nullptr;
    size_t n = 0;
    const void* id = nullptr;
    if (t >= 1) {
      const auto& fn = factoryNoises();
      if (t - 1 < static_cast<int>(fn.size())) data = fn[static_cast<size_t>(t - 1)].L.data(), n = fn[static_cast<size_t>(t - 1)].L.size(), id = data;
    } else if (aux && aux->noiseFile) {
      data = aux->noiseFile->left.data(), n = aux->noiseFile->frames(), id = aux->noiseFile.get();
    }
    if (data && n) drawOverview(c, overview(static_cast<const Sample*>(id), data, n, 160), x + 4, y + 2, w - 8, dh - 4, theme::withAlpha(c.P.knob, 0.8f));
    y += dh + 8;
  }
  if (simple) {
    const float dx = w / 4;
    y = dialRow(y);
    knob(c, tailP(T_NoiseTone), x + dx * 0.5f, y + 20, 16, "Tone", KnobValue);
    knob(c, noiseP(N_Color), x + dx * 1.5f, y + 20, 16, "Colour", KnobValue);
    knob(c, noiseP(N_Level), x + dx * 2.5f, y + 20, 16, "Level", KnobValue);
    knob(c, noiseP(N_Pan), x + dx * 3.5f, y + 20, 16, "Pan", KnobValue | KnobBipolar);
    y += kDialRowH;
    ImGui::PopID();
    return y;
  }
  const float dx = w / 2;
  knob(c, tailP(T_NoiseTone), x + dx * 0.5f, y + 24, 22, "Tone", KnobValue);
  knob(c, noiseP(N_Color), x + dx * 1.5f, y + 24, 22, "Colour", KnobValue);
  y += 84;
  y = fieldGrid(c, x, y, w, 2, 32,
                {{noiseP(N_Pitch), "Pitch"}, {noiseP(N_Fine), "Cents"}, {noiseP(N_OneShot), "Play once"}, {noiseP(N_PitchTrack), "Follows keys"}}, 26);
  if (det)
    y = fieldGrid(c, x, y, w, 2, 32,
                  {{noiseP(N_Phase), "Start"}, {noiseP(N_RandPhase), "Start scatter"}, {tailP(T_NKeyMin), "Lowest key"},
                   {tailP(T_NKeyMax), "Highest key"}, {tailP(T_NKeyFold), "Fold keys in"}, {tailP(T_NoiseLaw), "Pitch law"}},
                  26);
  sectionEnd(c, x, y, w);
  y = outputSection(c, noiseP(N_Level), noiseP(N_Pan), RtNoise, x, y, w);
  ImGui::PopID();
  return y;
}

float sourcesColumn(Ctx& c, float x, float y, float w) {
  sourceChips(c, x, y, w);
  y += 60;
  const int s = std::clamp(c.u.src, 0, 4);
  if (s < 3) return layerColumn(c, s, x, y, w);
  return s == 3 ? rootColumn(c, x, y, w) : textureColumn(c, x, y, w);
}

float filterColumn(Ctx& c, int f, float x, float y, float w) {
  ImGui::PushID(f + 10);
  const bool det = showDetails(c);
  {
    const int t = vali(c, filtP(f, F_Type));
    reach(filtP(f, F_Type));
    int step = 0;
    const std::string nm = filterDisplayName(t);
    dropField(c, "ft", x, y, w, 26, nm.c_str(), [&] { filterPicker(c, filtP(f, F_Type)); }, true, &step);
    if (ImGui::IsItemHovered()) c.a.setHint("Filter type: " + nm + " (click: choose, arrows: step)");
    if (step) {
      std::vector<int> order;
      for (const auto& g : filterMenuGroups())
        for (int m : g.items) order.push_back(m);
      int at = 0;
      for (size_t q = 0; q < order.size(); ++q)
        if (order[q] == t) at = static_cast<int>(q);
      setv(c, filtP(f, F_Type), order[static_cast<size_t>((at + step + static_cast<int>(order.size())) % static_cast<int>(order.size()))]);
    }
    y += 34;
  }
  const float dh = det ? 110 : fitPic(c, 72);
  display(c, x, y, w, dh, 8, 3);
  filterCurve(c, f, x, y, w, dh, true);
  markPicture();
  y += dh + 8;
  const float dx = w / 3;
  y = dialRow(y);
  knob(c, filtP(f, F_Freq), x + dx * 0.5f, y + 20, 16, "Cutoff", KnobValue);
  knob(c, filtP(f, F_Res), x + dx * 1.5f, y + 20, 16, "Resonance", KnobValue);
  knob(c, filtP(f, F_Drive), x + dx * 2.5f, y + 20, 16, "Drive", KnobValue);
  y += kDialRowH;  // the value under each dial, with room at small sizes (the text keeps a minimum size)
  if (det) {
    y = fieldGrid(c, x, y, w, 1, 32,
                  {{filtP(f, F_Var), "Character"}, {filtP(f, F_Stereo), "Stereo"}, {filtP(f, F_Wet), "Wet"}, {filtP(f, F_Level), "Level"},
                   {filtP(f, F_X), "Morph X"}, {filtP(f, F_Y), "Morph Y"}, {filtP(f, F_KeyTrack), "Follows keys"},
                   {tailP(f == 0 ? T_Pad1 : T_Pad2), "Input pad"}},
                  26);
    text(c, x, y + 4, "Inputs", c.P.label, 11.0f, true);
    y += 24;
    const char* lab[5] = {"Layer 1", "Layer 2", "Layer 3", "Root", "Texture"};
    const int rs[5] = {RtA, RtB, RtC, RtSub, RtNoise};
    const float bw = (w - 8) / 3;
    for (int i = 0; i < 5; ++i) {
      ImGui::PushID(i + 60);
      const bool fed = feeds(c, rs[i], f);
      if (button(c, "src", x + (bw + 4) * (i % 3), y + (i / 3) * 30, bw, 26, lab[i], fed, 11.0f)) toggleFeed(c, rs[i], f);
      if (ImGui::IsItemHovered()) c.a.setHint(std::string(lab[i]) + (fed ? " feeds this filter (click: take it out)" : " does not feed this filter (click: add it)"));
      ImGui::PopID();
    }
    y += 62;
    text(c, x, y + 4, "Goes to", c.P.label, 11.0f, true);
    y += 24;
    routeCompact(c, f == 0 ? RtF1 : RtF2, x, y, w);
    y += 64;
    text(c, x, y + 4, "Both filters", c.P.label, 11.0f, true);
    y += 24;
    y = fieldGrid(c, x, y, w, 1, 32, {{tailP(T_KeyCentre), "Key follow centre"}, {tailP(T_DriveMode), "Drive"}}, 26);
  }
  ImGui::PopID();
  return y;
}
float toneColumn(Ctx& c, float x, float y, float w) {
  for (int f = 0; f < kFilters; ++f) {
    text(c, x, y + 4, f == 0 ? "Filter 1" : "Filter 2", c.P.text, 12.0f, true);
    y = filterColumn(c, f, x, y + 26, w) + 12;
  }
  return y;
}

void cycleMiniPublic(Ctx& c, int o, float x, float y, float w, float h) { cycleMini(c, o, x, y, w, h); }
void rootWavePublic(Ctx& c, int shape, float x, float y, float w, float h, ImU32 col) { waveLine(c, shape, x, y, w, h, col, 1.4f); }
void textureMiniPublic(Ctx& c, float x, float y, float w, float h) { textureMini(c, x, y, w, h); }
void filterCurvePublic(Ctx& c, int f, float x, float y, float w, float h) { filterCurve(c, f, x, y, w, h, false); }
void tableMenuPublic(Ctx& c, int o) { tableMenu(c, o); }
float rootWaveAt(int shape, float t) { return rootWave(shape, t); }
std::string layerSourceName(const Ctx& c, int o) {
  const int type = vali(c, oscP(o, O_Type));
  const auto& d = c.m.osc[static_cast<size_t>(o)];
  if (type == OscMultisample) return d.sfz.empty() ? std::string("No multisample") : refStem(c.m, d.sfz);
  if (type != OscWavetable && !d.sample.empty()) return refStem(c.m, d.sample);
  if (!d.table.empty()) return d.tableName.empty() ? std::string("Custom table") : d.tableName;
  const int t = vali(c, oscP(o, O_Table));
  const auto& n = factoryTableNames();
  return t >= 1 && t <= static_cast<int>(n.size()) ? n[static_cast<size_t>(t - 1)] : "No table";
}
bool layerHasTable(const Ctx& c, int o) {
  const auto aux = auxOf(c);
  const OscAux* oa = oscAux(aux.get(), o);
  const MipTable* mip = showsTable(c, oa, o) ? mipOf(c, oa, o) : nullptr;
  return mip && mip->frames > 1;
}
// The layer's picture: the table in depth (3D) or the playing wave (2D), morphing live; drag
// across it to move through the table; the frame at the top left, the position's first link at
// the top right; a sample or multisample layer shows its sample.
void tableDepthAt(Ctx& c, int o, float x, float y, float w, float h) {
  const auto aux = auxOf(c);
  const OscAux* oa = oscAux(aux.get(), o);
  const MipTable* mip = mipOf(c, oa, o);
  if (mip) tableDepth(c, o, mip, x, y, w, h, val(c, oscP(o, O_Enable)) >= 0.5, false);
}

void layerPicture(Ctx& c, int o, float x, float y, float w, float h) {
  const auto aux = auxOf(c);
  const OscAux* oa = oscAux(aux.get(), o);
  const bool table = showsTable(c, oa, o);
  wellLook(c, x, y, w, h);
  const ImU32 col = val(c, oscP(o, O_Enable)) >= 0.5 ? partCol(c, o) : c.P.textFaint;
  c.dl->PushClipRect(c.p(x + 1, y + 1), c.p(x + w - 1, y + h - 1), true);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##scrub", c.F.sz(w, h));
  markPicture();
  const bool hov = ImGui::IsItemHovered();
  if (!table) {
    sampleDisplay(c, o, oa, x, y, w, h, true);
    c.dl->PopClipRect();
    return;
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
    const float mx = (ImGui::GetIO().MousePos.x - c.p(x + w * 0.05f, 0).x) / c.s(w * 0.9f);
    setv(c, oscP(o, O_WtPos), std::clamp(static_cast<double>(mx), 0.0, 1.0), true);
  }
  if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW), c.a.setHint("Drag across the wave to move through the table \xC2\xB7 double-click: the table editor");
  reach(oscP(o, O_WtPos));
  const MipTable* mip = mipOf(c, oa, o);
  const bool on = val(c, oscP(o, O_Enable)) >= 0.5;
  const float lp = livePos(c, o), set = valf(c, oscP(o, O_WtPos)), at = lp >= 0 ? lp : set;
  if (c.u.wt3d[o] && mip && mip->frames > 1) {
    tableDepth(c, o, mip, x, y, w, h, on, false);
  } else if (aux) {
    gridLook(c, x, y, w, h, 8, 4, true);
    const float pad = w * 0.05f, y0 = y + h * 0.5f, amp = h * 0.36f;
    auto toPts = [&](const std::vector<float>& cy, int n) {
      std::vector<ImVec2> v(static_cast<size_t>(n + 1));
      for (int i = 0; i <= n; ++i) {
        const size_t k = std::min(cy.size() - 1, cy.size() * static_cast<size_t>(i) / static_cast<size_t>(n));
        v[static_cast<size_t>(i)] = c.p(x + pad + (w - pad * 2) * i / n, y0 - std::clamp(cy[k], -1.f, 1.f) * amp);
      }
      return v;
    };
    {  // the set position, before its links: dashed
      const auto g = toPts(cycleOf(c, *aux, o, set, false), 96);
      for (size_t i = 0; i + 1 < g.size(); i += 2) c.dl->AddLine(g[i], g[i + 1], theme::withAlpha(col, 0.35f), c.s(1.1f));
    }
    const auto pts = toPts(cycleOf(c, *aux, o, at, true, liveMorph(c, o, 0), liveMorph(c, o, 1)), 160);
    fillUnder(c, pts, c.p(0, y0).y, col, 0.30f, 0.04f, c.p(0, y0 - amp).y);
    glowLine(c, pts, col, M(2.2f));
    // the frame rail along the bottom
    const float ry = y + h - M(9), rx0 = x + pad, rx1 = x + w - pad;
    c.dl->AddRectFilled(c.p(rx0, ry - M(1)), c.p(rx1, ry + M(1)), theme::withAlpha(c.P.text, c.P.dark ? 0.08f : 0.11f));
    c.dl->AddRectFilled(c.p(rx0, ry - M(1)), c.p(rx0 + (rx1 - rx0) * at, ry + M(1)), theme::withAlpha(col, 0.35f));
    glowDot(c, c.p(rx0 + (rx1 - rx0) * at, ry), M(3.2f), col);
  }
  if (mip && mip->frames > 1) {
    char b[48];
    std::snprintf(b, sizeof b, "FRAME %d / %d", 1 + static_cast<int>(std::lround(at * (mip->frames - 1))), mip->frames);
    MonoText mono;
    text(c, x + M(8), y + M(6), b, c.u.hoverK == oscP(o, O_WtPos) ? c.P.hot : c.P.textFaint, T(10), false);
  }
  for (int s = 0; s < kModSlots; ++s)  // the position's first link, at the top right
    if (vali(c, modP(s, M_Dest)) == oscP(o, O_WtPos) + 1 && vali(c, modP(s, M_Source)) > SrcNone) {
      const int src = vali(c, modP(s, M_Source));
      modChip(c, x + w - M(8), y + M(5), dockName(src).c_str(), c.P.fam[familyOf(std::max(src, 0))]);
      break;
    }
  if (hov && !ImGui::IsItemActive()) {  // the double-click hint
    const char* hs = "Double-click for more";
    const float tw = textW(c, hs, T(11), false), pw = tw + M(10 + 10 + 17), ph = M(24);
    const float px = x + (w - pw) * 0.5f, py = y + h - M(10) - ph;
    if (pw < w - M(8)) {
      c.dl->AddRectFilled(c.p(px, py), c.p(px + pw, py + ph), theme::withAlpha(barBg(c), 0.9f), c.s(M(12)));
      c.dl->AddRect(c.p(px, py), c.p(px + pw, py + ph), c.P.lineHi, c.s(M(12)));
      c.dl->AddRect(c.p(px + M(10), py + M(5)), c.p(px + M(18.5f), py + M(17.5f)), c.P.text, c.s(M(4.25f)), 0, 1.f);
      c.dl->AddLine(c.p(px + M(14.25f), py + M(8)), c.p(px + M(14.25f), py + M(11)), c.P.accent, c.s(M(1.6f)));
      text(c, px + M(10 + 8.5f + 7), py + ph * 0.5f - T(11) * 0.68f, hs, c.P.text, T(11), false);
    }
  }
  c.dl->PopClipRect();
}

void sourceMini(Ctx& c, int i, float x, float y, float w, float h) {
  if (i < 3) cycleMini(c, i, x, y, w, h);
  else if (i == 3) waveLine(c, vali(c, subP(S_Shape)), x + 3, y, w - 6, h, val(c, subP(S_Enable)) >= 0.5 ? c.P.knob : c.P.textFaint, 1.4f);
  else textureMini(c, x + 2, y, w - 4, h);
}
bool routeFeeds(const Ctx& c, int r, int f) { return feeds(c, r, f); }
void routeToggleFeed(Ctx& c, int r, int f) { toggleFeed(c, r, f); }

}  // namespace gl::packs::plasma::ui
