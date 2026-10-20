// Gridline — "modulation" pack: Patch Host editor (graph view, node list,
// inner plugin parameters, macro mappings).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include "../../core/I18n.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "modulation_skin.h"
#include "modulation_widgets.h"
#include "patchv2.h"

namespace gl::packs::modulation {

using theme::pal;

namespace {

std::string nodeName(const PatchNode& n) {
  const PluginInfo* p = findPlugin(n.plugin.type);
  return p ? p->name : n.plugin.type;
}
bool isGen(const PatchNode& n) {
  const PluginInfo* p = findPlugin(n.plugin.type);
  return p && p->kind == PluginKind::Generator;
}

void commit(EditTarget& e, const PatchGraph& g, const char* label, uint64_t key = 0) {
  if (!e.st) return;
  PluginState st = *e.st;
  st.data = serializePatchGraph(g);
  e.set(st, label, key);
}

// Diagram: instruments | serial stages (parallel effects stacked) | output.
void diagram(EditTarget& e, const PatchGraph& g, int& sel, bool fxHost, float W) {
  const skin::Skin& K = sk::patcher();
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float H = 150;
  ImGui::InvisibleButton("##diagram", ImVec2(W, H));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::graphWell(dl, p0, ImVec2(p0.x + W, p0.y + H), K);
  for (float gx = p0.x + 20; gx < p0.x + W; gx += 20) dl->AddLine(ImVec2(gx, p0.y + 1), ImVec2(gx, p0.y + H - 1), theme::withAlpha(K.graphGrid, 0.5f));
  for (float gy = p0.y + 20; gy < p0.y + H; gy += 20) dl->AddLine(ImVec2(p0.x + 1, gy), ImVec2(p0.x + W - 1, gy), theme::withAlpha(K.graphGrid, 0.5f));
  // columns: input/instruments, then each serial stage, then out
  std::vector<std::vector<int>> cols;
  std::vector<int> gens;
  for (size_t i = 0; i < g.nodes.size(); ++i)
    if (isGen(g.nodes[i])) gens.push_back(static_cast<int>(i));
  cols.push_back(gens);
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    if (isGen(g.nodes[i])) continue;
    if (g.nodes[i].parallel && cols.size() > 1)
      cols.back().push_back(static_cast<int>(i));
    else
      cols.push_back({static_cast<int>(i)});
  }
  int ncol = static_cast<int>(cols.size()) + 1;  // + output
  float cw = (W - 20) / static_cast<float>(ncol);
  const float bw = std::min(120.0f, cw - 22), bh = 26;
  auto boxPos = [&](int c, int r, int count) {
    float x = p0.x + 10 + c * cw + (cw - bw) * 0.5f;
    float total = count * bh + (count - 1) * 8;
    float y = p0.y + (H - total) * 0.5f + r * (bh + 8);
    return ImVec2(x, y);
  };
  ImGuiIO& io = ImGui::GetIO();
  // connections
  for (int c = 0; c + 1 < ncol; ++c) {
    int na = std::max<int>(1, static_cast<int>(c < static_cast<int>(cols.size()) ? cols[static_cast<size_t>(c)].size() : 1));
    int nb = c + 1 < static_cast<int>(cols.size()) ? std::max<int>(1, static_cast<int>(cols[static_cast<size_t>(c + 1)].size())) : 1;
    for (int ra = 0; ra < na; ++ra)
      for (int rb = 0; rb < nb; ++rb) {
        ImVec2 a = boxPos(c, ra, na), b = boxPos(c + 1, rb, nb);
        dl->AddBezierCubic(ImVec2(a.x + bw, a.y + bh * 0.5f), ImVec2(a.x + bw + 20, a.y + bh * 0.5f), ImVec2(b.x - 20, b.y + bh * 0.5f),
                           ImVec2(b.x, b.y + bh * 0.5f), K.accent2C(), 1.8f);
      }
  }
  auto box = [&](ImVec2 at, const std::string& text, bool selected, bool dim) {
    dl->AddRectFilled(ImVec2(at.x + 2, at.y + 3), ImVec2(at.x + bw + 2, at.y + bh + 3), theme::withAlpha(IM_COL32_BLACK, 0.3f), 4);
    dl->AddRectFilled(at, ImVec2(at.x + bw, at.y + bh), selected ? skin::lighten(K.wellC(), 0.12f) : K.wellC(), 4);
    dl->AddRectFilled(at, ImVec2(at.x + bw, at.y + 4), selected ? K.select : skin::lighten(K.wellC(), 0.2f), 4, ImDrawFlags_RoundCornersTop);
    dl->AddRect(at, ImVec2(at.x + bw, at.y + bh), selected ? K.select : skin::darken(K.panel, 0.4f), 4, 0, selected ? 1.5f : 1.0f);
    ImGui::PushFont(nullptr, 12.0f);
    std::string t = text;
    while (t.size() > 3 && ImGui::CalcTextSize(t.c_str()).x > bw - 10) t = t.substr(0, t.size() - 4) + "...";
    ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    dl->AddText(ImVec2(at.x + (bw - ts.x) * 0.5f, at.y + 2 + (bh - ts.y) * 0.5f), dim ? K.textDim : K.text, t.c_str());
    ImGui::PopFont();
    return io.MousePos.x >= at.x && io.MousePos.x < at.x + bw && io.MousePos.y >= at.y && io.MousePos.y < at.y + bh;
  };
  bool clicked = ImGui::IsItemClicked();
  for (int c = 0; c < static_cast<int>(cols.size()); ++c) {
    const auto& col = cols[static_cast<size_t>(c)];
    if (col.empty()) {
      box(boxPos(c, 0, 1), fxHost ? "Input" : "Notes in", false, true);
      continue;
    }
    for (int r = 0; r < static_cast<int>(col.size()); ++r) {
      int idx = col[static_cast<size_t>(r)];
      const PatchNode& nd = g.nodes[static_cast<size_t>(idx)];
      if (box(boxPos(c, r, static_cast<int>(col.size())), nodeName(nd), idx == sel, nd.bypass) && clicked) sel = idx;
    }
  }
  box(boxPos(ncol - 1, 0, 1), "Output", false, true);
  if (ImGui::IsItemHovered()) e.a->setHint("Click a module to edit it. Effects run left to right; stacked effects run in parallel.");
}

// Inner parameters of one hosted plugin as free knobs; `changed` stores the
// edited state (with a merge key per parameter).
void stateKnobs(EditTarget& e, PluginState& st, float W, const std::function<void(uint64_t)>& changed);

void nodeParams(EditTarget& e, PatchGraph& g, int sel, float W) {
  stateKnobs(e, g.nodes[static_cast<size_t>(sel)].plugin, W, [&](uint64_t k) {
    commit(e, g, "edit module parameter", e.key ^ 0x50484E50ull ^ (static_cast<uint64_t>(sel) << 12) ^ k);
  });
}

void stateKnobs(EditTarget& e, PluginState& st, float W, const std::function<void(uint64_t)>& changed) {
  struct NodeRef {
    PluginState& plugin;
  } nd{st};
  const PluginInfo* info = findPlugin(nd.plugin.type);
  if (!info) return;
  float used = 0;
  const float cell = 66;
  bool released = false;
  for (size_t k = 0; k < info->params.size() && k < nd.plugin.params.size(); ++k) {
    const ParamDesc& d = info->params[k];
    if (used > 0 && used + cell > W - 20) used = 0;
    else if (used > 0) ImGui::SameLine(0, 0);
    ImGui::PushID(static_cast<int>(k));
    ImGui::BeginGroup();
    float x = ImGui::GetCursorPosX();
    double v = nd.plugin.params[k];
    ImGui::SetCursorPosX(x + (cell - 28) * 0.5f);
    bool rel = false;
    if (ui::Knob("k", &v, d, 14, nullptr, d.min < 0 && d.max > 0, &rel)) {
      nd.plugin.params[k] = static_cast<float>(d.clamp(v));
      changed(static_cast<uint64_t>(k));
    }
    released = released || rel;
    {  // draw the module knob as a skin wheel over the generic knob
      ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
      ImVec2 c((r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f);
      float rr = std::min(r1.x - r0.x, r1.y - r0.y) * 0.5f;
      const skin::Skin& K = sk::patcher();
      ImDrawList* kdl = ImGui::GetWindowDrawList();
      kdl->AddCircleFilled(c, rr + 1.5f, K.wellC(), 32);
      skin::drawWheel(kdl, c, rr, d.toNormalized(nd.plugin.params[k]), d.min < 0 && d.max > 0, true, ImGui::IsItemHovered() || ImGui::IsItemActive(), K,
                      ImGui::IsItemActive() ? K.fillPressed : K.fill);
    }
    std::string val = d.format(nd.plugin.params[k]);
    ImGui::PushFont(e.a->fontMono, 10.0f);
    ImVec2 ts = ImGui::CalcTextSize(val.c_str());
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(pal().text), "%s", val.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, 11.0f);
    std::string nm = d.name;
    while (nm.size() > 3 && ImGui::CalcTextSize(nm.c_str()).x > cell - 4) nm = nm.substr(0, nm.size() - 1);
    ts = ImGui::CalcTextSize(nm.c_str());
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(pal().textDim), "%s", nm.c_str());
    ImGui::PopFont();
    ImGui::SetCursorPosX(x);
    ImGui::Dummy(ImVec2(cell, 0));
    ImGui::EndGroup();
    if (ImGui::IsItemHovered()) e.a->setHint(d.name + ": " + d.format(nd.plugin.params[k]));
    ImGui::PopID();
    used += cell;
  }
  if (released) e.a->doc.endGesture();
}

bool patchHostBody(EditTarget& e);

}  // namespace

bool drawPatchHostEditor(EditTarget& e) {
  // Reference patch host: slate canvas under a darker top bar, amber selection.
  skin::Backdrop bd(sk::patcher(), e.width, e.key ^ 0x50415443ull, 4);
  bool r = patchHostBody(e);
  return r;
}

namespace {


// ------------------------------------------------------------ v2 graphs (J-41)
// FL patches as wired: the plugins at their FL positions with their audio,
// note and controller links, the selected plugin's parameters, and the
// patch's exposed parameters (host Param slots).
bool patchV2Body(EditTarget& e) {
  const auto& P = pal();
  const skin::Skin& K = sk::patcher();
  PatchV2 g;
  if (!parsePatchV2(e.st->data, g)) return false;
  const PatchV2Topo t = patchV2Topology(g);
  const float W = e.width;
  static std::map<uint64_t, int> selMap;
  int& sel = selMap[e.key];
  if (sel >= static_cast<int>(g.nodes.size())) sel = -1;
  // FL editor positions (1600 records: node id, x, y).
  std::map<uint64_t, ImVec2> pos;
  for (const auto& it : g.items) {
    if (it.empty() || it[0] == '#') continue;
    const std::vector<uint8_t> b = patchUnhex(it);
    for (const auto& c : patchChunks(b.data(), b.size())) {
      if (c.id != 0) continue;
      for (const auto& k : patchChunks(c.data.data(), c.data.size()))
        if (k.id == 1600 && k.data.size() >= 16) {
          uint64_t id = 0;
          std::memcpy(&id, k.data.data(), 8);
          float x = 0, y = 0;
          std::memcpy(&x, k.data.data() + 8, 4);
          std::memcpy(&y, k.data.data() + 12, 4);
          pos[id] = ImVec2(x, y);
        }
    }
  }
  const size_t N = g.nodes.size();
  std::vector<ImVec2> at(N);
  float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
  for (size_t i = 0; i < N; ++i) {
    auto it = pos.find(g.nodes[i].id);
    at[i] = it != pos.end() ? it->second : ImVec2(static_cast<float>(i % 6) * 150.0f, static_cast<float>(i / 6) * 60.0f);
    mnx = std::min(mnx, at[i].x), mny = std::min(mny, at[i].y), mxx = std::max(mxx, at[i].x), mxy = std::max(mxy, at[i].y);
  }
  pui::beginSection("PATCH", W);
  const float CW = W - 20, CH = 300, bw = 112, bh = 22, padX = 70;
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##canvas", ImVec2(CW, CH));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::graphWell(dl, p0, ImVec2(p0.x + CW, p0.y + CH), K);
  const float sx = mxx > mnx ? (CW - 2 * padX - bw) / (mxx - mnx) : 1.0f, sy = mxy > mny ? (CH - 20 - bh) / (mxy - mny) : 1.0f;
  const float sc = std::min(1.0f, std::min(sx, sy));
  auto box = [&](size_t i) { return ImVec2(p0.x + padX + (at[i].x - mnx) * sc, p0.y + 10 + (at[i].y - mny) * sc); };
  const ImVec2 hostIn(p0.x + 8, p0.y + CH * 0.5f), hostOut(p0.x + CW - 8, p0.y + CH * 0.5f);
  auto wire = [&](ImVec2 a, ImVec2 b, ImU32 col, float th) {
    dl->AddBezierCubic(a, ImVec2(a.x + 30, a.y), ImVec2(b.x - 30, b.y), b, col, th);
  };
  const ImU32 audioCol = K.accent2C(), ctlCol = theme::withAlpha(theme::litRole(theme::role::kGraph), 0.55f),
              noteCol = theme::withAlpha(K.fill, 0.7f);
  for (const auto& l : t.audio) {
    const ImVec2 a = l.src < 0 ? hostIn : ImVec2(box(static_cast<size_t>(l.src)).x + bw, box(static_cast<size_t>(l.src)).y + bh * 0.5f);
    const ImVec2 b = l.dst < 0 ? hostOut : ImVec2(box(static_cast<size_t>(l.dst)).x, box(static_cast<size_t>(l.dst)).y + bh * 0.5f);
    wire(a, b, audioCol, 1.8f);
  }
  for (const auto& c : t.controls) {
    if (c.src < 0 || c.dst < 0) continue;
    const ImVec2 a(box(static_cast<size_t>(c.src)).x + bw, box(static_cast<size_t>(c.src)).y + bh - 4);
    const ImVec2 b(box(static_cast<size_t>(c.dst)).x, box(static_cast<size_t>(c.dst)).y + bh - 4);
    wire(a, b, ctlCol, 1.0f);
  }
  for (size_t i = 0; i < N; ++i)
    if (t.notes[i]) wire(ImVec2(hostIn.x, hostIn.y - 20), ImVec2(box(i).x, box(i).y + 4), noteCol, 1.2f);
  dl->AddCircleFilled(hostIn, 5, audioCol, 12);
  dl->AddCircleFilled(hostOut, 5, audioCol, 12);
  ImGuiIO& io = ImGui::GetIO();
  int hover = -1;
  for (size_t i = 0; i < N; ++i) {
    const ImVec2 b0 = box(i), b1(b0.x + bw, b0.y + bh);
    const bool hov = io.MousePos.x >= b0.x && io.MousePos.x < b1.x && io.MousePos.y >= b0.y && io.MousePos.y < b1.y;
    if (hov) hover = static_cast<int>(i);
    const bool known = findPlugin(g.nodes[i].plugin.type) != nullptr;
    dl->AddRectFilled(ImVec2(b0.x + 2, b0.y + 3), ImVec2(b1.x + 2, b1.y + 3), theme::withAlpha(IM_COL32_BLACK, 0.3f), 4);
    dl->AddRectFilled(b0, b1, static_cast<int>(i) == sel ? skin::lighten(K.wellC(), 0.15f) : K.wellC(), 4);
    dl->AddRect(b0, b1, static_cast<int>(i) == sel ? K.fill : (hov ? K.text : theme::withAlpha(K.textDim, 0.5f)), 4, 0, 1.2f);
    std::string nm = g.nodes[i].display.empty() ? g.nodes[i].flName : g.nodes[i].display;
    ImGui::PushFont(e.a->fontMono, 10.0f);
    while (nm.size() > 3 && ImGui::CalcTextSize(nm.c_str()).x > bw - 8) nm.pop_back();
    dl->AddText(ImVec2(b0.x + 4, b0.y + 5), known ? K.text : P.textFaint, nm.c_str());
    ImGui::PopFont();
  }
  if (ImGui::IsItemClicked() && hover >= 0) sel = hover;
  if (ImGui::IsItemHovered()) {
    if (hover >= 0) {
      const auto& n = g.nodes[static_cast<size_t>(hover)];
      const PluginInfo* info = findPlugin(n.plugin.type);
      e.a->setHint((n.display.empty() ? n.flName : n.display) + " — " + (info ? info->name : std::string("not available in Gridline")) + "   (click to edit)");
    } else {
      e.a->setHint("Orange: audio, green: controller links, light: notes. Click a plugin to edit it.");
    }
  }
  ImGui::PushFont(e.a->fontMono, 10.5f);
  ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("%zu plugins, %zu audio / %zu controller links, %zu parameters%s"), N, t.audio.size(), t.controls.size(),
                     g.slots.size(), t.unsupported.empty() ? "" : " (some FL routing not run)");
  ImGui::PopFont();
  pui::endSection();
  if (sel >= 0 && static_cast<size_t>(sel) < N) {
    PatchV2Node& nd = g.nodes[static_cast<size_t>(sel)];
    const PluginInfo* info = findPlugin(nd.plugin.type);
    std::string title = "PLUGIN: " + (nd.display.empty() ? nd.flName : nd.display) + (info ? "  (" + info->name + ")" : "");
    pui::beginSection(title.c_str(), W);
    if (info) {
      const int s2 = sel;
      stateKnobs(e, nd.plugin, W, [&](uint64_t k) {
        PluginState st = *e.st;
        st.data = serializePatchV2(g);
        e.set(st, "edit patch plugin", e.key ^ 0x50483256ull ^ (static_cast<uint64_t>(s2) << 12) ^ k);
      });
    } else {
      ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("%s has no Gridline equivalent; it is kept for export."), nd.flName.c_str());
    }
    pui::endSection();
  }
  if (!g.slots.empty()) {
    pui::beginSection("PATCH PARAMETERS", W);
    float used = 0;
    for (size_t k = 0; k < t.slots.size() && k < static_cast<size_t>(kPatchMaps); ++k) {
      const auto& sl = t.slots[k];
      std::string label = "Param " + std::to_string(k + 1);
      if (sl.node >= 0 && static_cast<size_t>(sl.node) < N) {
        const auto& n = g.nodes[static_cast<size_t>(sl.node)];
        const PatchV2Map* m = n.map(sl.fl);
        const PluginInfo* info = findPlugin(n.plugin.type);
        if (m && info && m->gl >= 0 && static_cast<size_t>(m->gl) < info->params.size()) label = info->params[static_cast<size_t>(m->gl)].name;
      }
      if (used > 0 && used + 70 > W - 20) used = 0;
      else if (used > 0) ImGui::SameLine(0, 0);
      ImGui::PushID(static_cast<int>(k) + 9000);
      w::knob(e, PhMap0 + static_cast<int>(k), label.c_str(), 70);
      ImGui::PopID();
      used += 70;
    }
    pui::endSection();
  }
  return true;
}

bool patchHostBody(EditTarget& e) {
  const auto& P = pal();
  if (!e.st) return false;
  if (isPatchV2(e.st->data)) return patchV2Body(e);  // J-41: FL patches
  bool fxHost = e.st->type == "gl.patcherfx";
  float W = e.width;
  PatchGraph g = patchGraphOf(*e.st);
  static std::map<uint64_t, int> selMap;
  int& sel = selMap[e.key];
  if (sel >= static_cast<int>(g.nodes.size())) sel = static_cast<int>(g.nodes.size()) - 1;

  pui::beginSection("PATCH", W);
  diagram(e, g, sel, fxHost, W - 20);
  // add modules
  ImGui::SetNextItemWidth(180);
  if (g.nodes.size() < static_cast<size_t>(kPatchNodes) && ImGui::BeginCombo("##addfx", "Add effect...")) {
    for (const auto& p : pluginCatalog())
      if (p.kind == PluginKind::Effect && p.type != "gl.patcherfx" && ImGui::Selectable(p.name.c_str())) {
        PatchNode n;
        n.plugin = defaultPluginState(p.type);
        g.nodes.push_back(n);
        sel = static_cast<int>(g.nodes.size()) - 1;
        commit(e, g, "add module");
      }
    ImGui::EndCombo();
  }
  if (!fxHost) {
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(180);
    if (g.nodes.size() < static_cast<size_t>(kPatchNodes) && ImGui::BeginCombo("##addgen", "Add instrument layer...")) {
      for (const auto& p : pluginCatalog())
        if (p.kind == PluginKind::Generator && p.type != "gl.patcher" && p.outputs.empty() && ImGui::Selectable(p.name.c_str())) {
          PatchNode n;
          n.plugin = defaultPluginState(p.type);
          // instruments go before the effects
          size_t at = 0;
          while (at < g.nodes.size() && isGen(g.nodes[at])) ++at;
          g.nodes.insert(g.nodes.begin() + static_cast<long>(at), n);
          for (auto& m : g.macros)
            for (auto& t : m)
              if (t.node >= static_cast<int>(at)) ++t.node;
          sel = static_cast<int>(at);
          commit(e, g, "add instrument layer");
        }
      ImGui::EndCombo();
    }
  }
  ImGui::SameLine(0, 10);
  ImGui::PushFont(e.a->fontMono, 10.5f);
  ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("%zu / %d modules"), g.nodes.size(), kPatchNodes);
  ImGui::PopFont();
  pui::endSection();

  if (sel >= 0 && sel < static_cast<int>(g.nodes.size())) {
    PatchNode& nd = g.nodes[static_cast<size_t>(sel)];
    std::string title = "MODULE: " + nodeName(nd);
    pui::beginSection(title.c_str(), W);
    if (!isGen(nd)) {
      bool par = nd.parallel;
      if (ui::TextButton(par ? "Parallel" : "Serial", par, 80)) {
        nd.parallel = !par;
        commit(e, g, "change module routing");
      }
      ui::Hint("Serial: runs after the previous effect. Parallel: runs beside it on the same input.");
      ImGui::SameLine(0, 6);
      float mix = nd.mix * 100;
      ImGui::SetNextItemWidth(140);
      if (ImGui::SliderFloat("##mix", &mix, 0, 100, "Mix %.0f%%")) {
        nd.mix = mix / 100;
        commit(e, g, "change module mix", e.key ^ 0x4D495800ull ^ static_cast<uint64_t>(sel));
      }
      if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
      ImGui::SameLine(0, 6);
    }
    if (ui::TextButton(nd.bypass ? "Bypassed" : "Bypass", nd.bypass, 80)) {
      nd.bypass = !nd.bypass;
      commit(e, g, "bypass module");
    }
    ImGui::SameLine(0, 6);
    if (ui::IconButton("left", ui::Icon::Left, false, 24, "Move earlier") && sel > 0 && isGen(g.nodes[static_cast<size_t>(sel - 1)]) == isGen(nd)) {
      std::swap(g.nodes[static_cast<size_t>(sel)], g.nodes[static_cast<size_t>(sel - 1)]);
      for (auto& m : g.macros)
        for (auto& t : m) t.node = t.node == sel ? sel - 1 : (t.node == sel - 1 ? sel : t.node);
      --sel;
      commit(e, g, "move module");
    } else {
      ImGui::SameLine(0, 2);
      if (ui::IconButton("right", ui::Icon::Right, false, 24, "Move later") && sel + 1 < static_cast<int>(g.nodes.size()) &&
          isGen(g.nodes[static_cast<size_t>(sel + 1)]) == isGen(nd)) {
        std::swap(g.nodes[static_cast<size_t>(sel)], g.nodes[static_cast<size_t>(sel + 1)]);
        for (auto& m : g.macros)
          for (auto& t : m) t.node = t.node == sel ? sel + 1 : (t.node == sel + 1 ? sel : t.node);
        ++sel;
        commit(e, g, "move module");
      } else {
        ImGui::SameLine(0, 2);
        if (ui::IconButton("rm", ui::Icon::Trash, false, 24, "Remove module")) {
          int gone = sel;
          g.nodes.erase(g.nodes.begin() + gone);
          for (auto& m : g.macros) {
            m.erase(std::remove_if(m.begin(), m.end(), [&](const PatchMacroTarget& t) { return t.node == gone; }), m.end());
            for (auto& t : m)
              if (t.node > gone) --t.node;
          }
          sel = std::min(gone, static_cast<int>(g.nodes.size()) - 1);
          commit(e, g, "remove module");
          pui::endSection();
          return true;
        }
      }
    }
    nodeParams(e, g, sel, W);
    pui::endSection();
  }

  // Macros
  pui::beginSection("MACROS", W);
  const float cell = (W - 20) / 8.0f;
  for (int m = 0; m < kMacros; ++m) {
    ImGui::PushID(m);
    if (m) ImGui::SameLine(0, 0);
    ImGui::BeginGroup();
    float x = ImGui::GetCursorPosX();
    {
      ImVec2 c = ImGui::GetCursorScreenPos();
      ImVec2 at(ImGui::GetWindowPos().x + x - ImGui::GetScrollX() + cell * 0.5f - 20, c.y);
      sk::wheel(e, m, at, 40, sk::patcher());
      ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + 44));
    }
    std::string name = g.macroNames[m].empty() ? "Macro " + std::to_string(m + 1) : g.macroNames[m];
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s", name.c_str());
    ImGui::SetCursorPosX(x + 4);
    ImGui::SetNextItemWidth(cell - 8);
    if (ImGui::InputText("##name", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
      g.macroNames[m] = buf;
      commit(e, g, "rename macro");
    }
    ImGui::SetCursorPosX(x + 4);
    if (ui::TextButton("+ map", false, cell - 8)) ImGui::OpenPopup("##map");
    if (ImGui::BeginPopup("##map")) {
      for (size_t n = 0; n < g.nodes.size(); ++n) {
        const PluginInfo* info = findPlugin(g.nodes[n].plugin.type);
        if (!info) continue;
        ImGui::PushID(static_cast<int>(n));
        if (ImGui::BeginMenu((std::to_string(n + 1) + "  " + info->name).c_str())) {
          for (size_t k = 0; k < info->params.size(); ++k)
            if (ImGui::MenuItem(info->params[k].name.c_str())) {
              g.macros[m].push_back({static_cast<int>(n), static_cast<int>(k), 0.0f, 1.0f});
              commit(e, g, "map macro");
            }
          ImGui::EndMenu();
        }
        ImGui::PopID();
      }
      ImGui::EndPopup();
    }
    ImGui::SetCursorPosX(x);
    ImGui::Dummy(ImVec2(cell, 0));
    ImGui::EndGroup();
    ImGui::PopID();
  }
  // mapping list
  bool any = false;
  for (int m = 0; m < kMacros; ++m)
    for (size_t k = 0; k < g.macros[m].size(); ++k) {
      PatchMacroTarget& t = g.macros[m][k];
      const PluginInfo* info = static_cast<size_t>(t.node) < g.nodes.size() ? findPlugin(g.nodes[static_cast<size_t>(t.node)].plugin.type) : nullptr;
      if (!info || static_cast<size_t>(t.param) >= info->params.size()) continue;
      any = true;
      ImGui::PushID(m * 100 + static_cast<int>(k));
      std::string name = g.macroNames[m].empty() ? "Macro " + std::to_string(m + 1) : g.macroNames[m];
      ImGui::PushFont(e.a->fontMono, 11.0f);
      ImGui::Text("%-10s -> %s / %s", name.c_str(), info->name.c_str(), info->params[static_cast<size_t>(t.param)].name.c_str());
      ImGui::PopFont();
      ImGui::SameLine(W - 300);
      float lo = t.min * 100, hi = t.max * 100;
      ImGui::SetNextItemWidth(220);
      if (ImGui::DragFloatRange2("##range", &lo, &hi, 0.5f, 0, 100, "from %.0f%%", "to %.0f%%")) {
        t.min = lo / 100;
        t.max = hi / 100;
        commit(e, g, "change macro range", e.key ^ 0x4D41434Full ^ static_cast<uint64_t>(m * 100 + static_cast<int>(k)));
      }
      if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
      ImGui::SameLine(0, 4);
      if (ui::IconButton("x", ui::Icon::Close, false, 20, "Remove mapping")) {
        g.macros[m].erase(g.macros[m].begin() + static_cast<long>(k));
        commit(e, g, "unmap macro");
        ImGui::PopID();
        pui::endSection();
        return true;
      }
      ImGui::PopID();
    }
  if (!any) ImGui::TextDisabled("%s", i18n::tr("Map a macro to module parameters with \"+ map\"."));
  if (fxHost) {
    w::knob(e, PhMix, "Mix");
    ImGui::SameLine(0, 0);
  }
  w::knob(e, PhGain, "Output");
  pui::endSection();
  return true;
}

}  // namespace

}  // namespace gl::packs::modulation
