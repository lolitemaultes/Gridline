// Gridline — "samplers" pack: editors.
//   Multisampler:    toolbar · zone list | key×velocity zone map + keyboard ·
//                    tabs (sample with loop markers, zone, program, modulation)
//   Scratch Sampler: sample + region · move curve editor + mute lane +
//                    vertical waveform · move keyboard · playback/shape panels
#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include "../../core/I18n.h"
#include "../../app/App.h"
#include "../../app/InstrumentSkin.h"
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../engine/Sample.h"
#include "../PacksUi.h"
#include "samplers.h"
#include "../../core/Platform.h"

namespace gl::packs::samplers {

using theme::pal;
namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ helpers
float val(const PluginState& s, int k, float d = 0) {
  audit::stateRead(k);  // J-92 binding audit
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : d;
}

std::string noteName(int n) {
  static const char* nm[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  n = std::clamp(n, 0, 127);
  return std::string(nm[n % 12]) + std::to_string(n / 12 - 1);
}
bool isBlack(int n) {
  int pc = ((n % 12) + 12) % 12;
  return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

ParamDesc pd(const char* n, const char* u, double mn, double mx, double d, bool log = false) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, log ? ParamScale::Log : ParamScale::Linear, {}};
}

uint64_t mergeKey(const PluginTarget& t, int field) { return (t.key() * 1315423911ull) ^ (0xA11CE000ull + static_cast<uint64_t>(field)); }

// Knob over a free value with the value and label underneath (like pui::knobCell).
bool valueKnob(App& a, const char* id, double& v, const ParamDesc& d, const char* label, float cell = 60, float radius = 14) {
  ImGui::PushID(id);
  ImGui::BeginGroup();
  float x = ImGui::GetCursorPosX();
  ImGui::SetCursorPosX(x + (cell - radius * 2) * 0.5f);
  bool changed = ui::Knob("k", &v, d, radius, nullptr, d.min < 0 && d.max > 0);
  std::string txt = d.format(v);
  ImGui::PushFont(a.fontMono, 10.5f);
  ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
  ImGui::SetCursorPosX(x + (cell - ts.x) * 0.5f);
  ImGui::TextColored(theme::vec(pal().text), "%s", txt.c_str());
  ImGui::PopFont();
  ImGui::PushFont(nullptr, 12.0f);
  ts = ImGui::CalcTextSize(label);
  ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(pal().textDim), "%s", label);
  ImGui::PopFont();
  ImGui::SetCursorPosX(x);
  ImGui::Dummy(ImVec2(cell, 0));
  ImGui::EndGroup();
  ImGui::PopID();
  return changed;
}

// Segmented buttons for a free choice; returns the new index or -1.
int segmented(const char* id, const std::vector<std::string>& items, int cur, float width) {
  ImGui::PushID(id);
  int out = -1;
  float w = width > 0 ? (width - (static_cast<float>(items.size()) - 1) * 2.0f) / static_cast<float>(items.size()) : 0;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) ImGui::SameLine(0, 2);
    ImGui::PushID(static_cast<int>(i));
    if (ui::TextButton(items[i].c_str(), static_cast<int>(i) == cur, w)) out = static_cast<int>(i);
    ImGui::PopID();
  }
  ImGui::PopID();
  return out;
}

void smallCaps(App& a, const char* s) {
  ImGui::PushFont(a.fontMono, 10.5f);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", s);
  ImGui::PopFont();
}

// Min/max waveform of frames [f0, f1) into a rect (horizontal).
void drawWave(ImDrawList* dl, const Sample* smp, int64_t f0, int64_t f1, ImVec2 p, ImVec2 sz, ImU32 col) {
  if (!smp || smp->frames() == 0 || f1 <= f0) return;
  float mid = p.y + sz.y * 0.5f;
  int w = std::max(1, static_cast<int>(sz.x));
  const int64_t n = static_cast<int64_t>(smp->frames());
  for (int x = 0; x < w; ++x) {
    int64_t a0 = f0 + (f1 - f0) * x / w, a1 = std::max(a0 + 1, f0 + (f1 - f0) * (x + 1) / w);
    float lo = 0, hi = 0;
    int64_t step = std::max<int64_t>(1, (a1 - a0) / 48);
    for (int64_t i = std::max<int64_t>(0, a0); i < a1 && i < n; i += step) {
      float v = (smp->left[static_cast<size_t>(i)] + smp->right[static_cast<size_t>(i)]) * 0.5f;
      lo = std::min(lo, v), hi = std::max(hi, v);
    }
    dl->AddLine(ImVec2(p.x + static_cast<float>(x), mid - hi * sz.y * 0.46f), ImVec2(p.x + static_cast<float>(x), mid - lo * sz.y * 0.46f + 1), col);
  }
}

void openDialog(App& a, const std::string& title, std::vector<std::string> exts, std::function<void(const std::string&)> ok) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = title;
  const char* h = gl::platform::homeEnv();
  a.fileDialog.dir = h ? h : ".";
  a.fileDialog.exts = std::move(exts);
  a.fileDialog.onOk = std::move(ok);
}
const std::vector<std::string> kAudioExts = {".wav", ".wave", ".flac", ".mp3"};

void preview(App& a, const PluginTarget& t, int key, float vel, bool on) {
  if (!t.isChannel()) return;
  if (on) a.engine.noteOn(t.owner, key, vel);
  else a.engine.noteOff(t.owner, key);
}

// =========================================================================
// Multisampler
// =========================================================================
struct MsEd {
  int sel = 0, tab = 0;
  float viewLo = 0, viewKeys = 128;
  int drag = 0, dragZone = -1, heldKey = -1, marker = -1;
  Zone orig;
  ImVec2 start;
  bool autoMapOnAdd = true;
};
std::map<uint64_t, MsEd> gMs;

ZoneMap currentMap(const PluginState& st) { return parseZoneMap(st.data, static_cast<int>(val(st, MsFactorySet) + 0.5f)); }

void commitMap(App& a, const PluginTarget& t, const ZoneMap& m, const char* label, uint64_t merge = 0) {
  const PluginState* cur = t.state(a.doc.song());
  if (!cur) return;
  PluginState ns = *cur;
  ns.data = serializeZoneMap(m);
  if (ns.data != cur->data) setPluginState(a, t, ns, label, merge);
}

// Adds files (dropped or chosen), maps them, selects the first new zone.
void addFiles(App& a, const PluginTarget& t, const std::vector<std::string>& files, bool replace, bool automap, int atKey = -1) {
  const PluginState* cur = t.state(a.doc.song());
  if (!cur || files.empty()) return;
  ZoneMap m = replace ? ZoneMap{} : currentMap(*cur);
  if (replace) m.zones.clear();
  m.sf2Path.clear();
  size_t first = m.zones.size();
  bool anyNamed = false;
  for (const auto& f : files) {
    if (!resolveSample(f)) {
      a.notify("Cannot decode " + f, true);
      continue;
    }
    Zone z = zoneForFile(f);
    if (noteFromName(f) >= 0) anyNamed = true;
    if (atKey >= 0) {
      z.lo = z.hi = atKey;
      if (noteFromName(f) < 0) z.root = atKey;
    }
    m.zones.push_back(z);
  }
  if (m.zones.size() == first) return;
  if (automap && atKey < 0) {
    if (anyNamed) {
      autoMap(m.zones);
    } else if (replace) {
      mapAsDrumKit(m.zones, 36);
    } else if (m.zones.size() - first == 1 && first == 0) {
      m.zones[0].lo = 0, m.zones[0].hi = 127;
    }
  }
  gMs[t.key()].sel = static_cast<int>(first);
  commitMap(a, t, m, files.size() > 1 ? "add samples" : "add sample");
}

void zoneMenu(App& a, const PluginTarget& t, ZoneMap& m, MsEd& ed, int i) {
  if (i < 0 || i >= static_cast<int>(m.zones.size())) return;
  if (ImGui::MenuItem(i18n::tr("Duplicate zone"))) {
    Zone z = m.zones[static_cast<size_t>(i)];
    m.zones.insert(m.zones.begin() + i + 1, z);
    ed.sel = i + 1;
    commitMap(a, t, m, "duplicate zone");
  }
  if (ImGui::MenuItem(i18n::tr("Delete zone"))) {
    m.zones.erase(m.zones.begin() + i);
    ed.sel = std::max(0, i - 1);
    commitMap(a, t, m, "delete zone");
  }
  if (ImGui::MenuItem(i18n::tr("Full keyboard"))) {
    m.zones[static_cast<size_t>(i)].lo = 0, m.zones[static_cast<size_t>(i)].hi = 127;
    commitMap(a, t, m, "zone full range");
  }
  if (ImGui::MenuItem(i18n::tr("Full velocity"))) {
    m.zones[static_cast<size_t>(i)].vlo = 0, m.zones[static_cast<size_t>(i)].vhi = 127;
    commitMap(a, t, m, "zone full velocity");
  }
  if (ImGui::MenuItem(i18n::tr("Root from file name"))) {
    int n = noteFromName(m.zones[static_cast<size_t>(i)].path);
    if (n >= 0) {
      m.zones[static_cast<size_t>(i)].root = n;
      commitMap(a, t, m, "zone root from name");
    } else {
      a.notify("No note name in this file name");
    }
  }
  if (ImGui::MenuItem(i18n::tr("Replace sample..."))) {
    PluginTarget tt = t;
    int zi = i;
    openDialog(a, "Replace sample", kAudioExts, [&a, tt, zi](const std::string& p) {
      const PluginState* cur = tt.state(a.doc.song());
      if (!cur) return;
      ZoneMap mm = currentMap(*cur);
      if (zi >= static_cast<int>(mm.zones.size())) return;
      mm.zones[static_cast<size_t>(zi)].path = p;
      mm.zones[static_cast<size_t>(zi)].name.clear();
      commitMap(a, tt, mm, "replace sample");
    });
  }
}

void msZoneList(App& a, const PluginTarget& t, ZoneMap& m, MsEd& ed, ImVec2 size) {
  const auto& P = pal();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::BeginChild("##zonelist", size, ImGuiChildFlags_None);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (m.zones.empty()) {
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("  No zones. Drop samples here,"));
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("  or use Add sample / SoundFont."));
  }
  for (size_t i = 0; i < m.zones.size(); ++i) {
    const Zone& z = m.zones[i];
    ImGui::PushID(static_cast<int>(i));
    ImVec2 q = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    bool sel = static_cast<int>(i) == ed.sel;
    if (ImGui::InvisibleButton("row", ImVec2(w, 20))) ed.sel = static_cast<int>(i);
    bool hov = ImGui::IsItemHovered();
    if (sel || hov) dl->AddRectFilled(q, ImVec2(q.x + w, q.y + 20), sel ? theme::withAlpha(P.accent, 0.35f) : P.surface800, 3);
    dl->AddRectFilled(ImVec2(q.x + 2, q.y + 4), ImVec2(q.x + 4, q.y + 16), sel ? P.accent : P.lineStrong);
    std::string lab = zoneLabel(z);
    dl->PushClipRect(q, ImVec2(q.x + w - 104, q.y + 20), true);
    dl->AddText(ImVec2(q.x + 9, q.y + 3), P.text, lab.c_str());
    dl->PopClipRect();
    char r[48];
    std::snprintf(r, sizeof r, "%s..%s %3d..%-3d", noteName(z.lo).c_str(), noteName(z.hi).c_str(), z.vlo, z.vhi);
    ImGui::PushFont(a.fontMono, 10.0f);
    ImVec2 ts = ImGui::CalcTextSize(r);
    dl->AddText(ImVec2(q.x + w - ts.x - 4, q.y + 4), P.textDim, r);
    ImGui::PopFont();
    if (hov) a.setHint(lab + "   keys " + noteName(z.lo) + "–" + noteName(z.hi) + " · velocity " + std::to_string(z.vlo) + "–" +
                       std::to_string(z.vhi) + " · root " + noteName(z.root) + "   (right-click: zone menu)");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      ed.sel = static_cast<int>(i);
      ImGui::OpenPopup("##zmenu");
    }
    if (ImGui::BeginPopup("##zmenu")) {
      zoneMenu(a, t, m, ed, static_cast<int>(i));
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  ImGui::Dummy(ImVec2(0, std::max(0.0f, size.y - ImGui::GetCursorPosY() - 4)));
  ImGui::EndChild();
  ImGui::PopStyleColor();
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
      std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (isAudioFile(path)) addFiles(a, t, {path}, false, ed.autoMapOnAdd);
    }
    ImGui::EndDragDropTarget();
  }
}

void msZoneMap(App& a, const PluginTarget& t, ZoneMap& m, MsEd& ed, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float kbH = 30;
  const float H = size.y - kbH - 2, W = size.x;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##zonemap", ImVec2(W, H));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImGuiIO& io = ImGui::GetIO();
  const float keyW = W / ed.viewKeys;
  auto kx = [&](double key) { return p.x + static_cast<float>((key - ed.viewLo) * keyW); };
  auto vy = [&](double vel) { return p.y + H - static_cast<float>(vel / 128.0) * H; };
  auto keyAt = [&](float x) { return std::clamp(static_cast<int>(std::floor(ed.viewLo + (x - p.x) / keyW)), 0, 127); };
  auto velAt = [&](float y) { return std::clamp(static_cast<int>((p.y + H - y) / H * 128.0f), 0, 127); };
  // Zoom / scroll.
  if (hov && io.MouseWheel != 0) {
    if (io.KeyShift) {
      ed.viewLo -= io.MouseWheel * 4;
    } else {
      float mk = ed.viewLo + (io.MousePos.x - p.x) / keyW;
      float nk = std::clamp(ed.viewKeys * (io.MouseWheel > 0 ? 0.85f : 1.0f / 0.85f), 24.0f, 128.0f);
      ed.viewLo = mk - (io.MousePos.x - p.x) / W * nk;
      ed.viewKeys = nk;
    }
    ed.viewLo = std::clamp(ed.viewLo, 0.0f, 128.0f - ed.viewKeys);
  }
  dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), P.surface950, 6);
  dl->PushClipRect(p, ImVec2(p.x + W, p.y + H), true);
  for (int k = static_cast<int>(ed.viewLo); k <= static_cast<int>(ed.viewLo + ed.viewKeys); ++k) {
    if (isBlack(k)) dl->AddRectFilled(ImVec2(kx(k), p.y), ImVec2(kx(k + 1), p.y + H), theme::withAlpha(P.surface800, 0.6f));
    if (k % 12 == 0) dl->AddLine(ImVec2(kx(k), p.y), ImVec2(kx(k), p.y + H), P.line);
  }
  for (int v = 32; v < 128; v += 32) dl->AddLine(ImVec2(p.x, vy(v)), ImVec2(p.x + W, vy(v)), theme::withAlpha(P.line, 0.6f));
  ImGui::PushFont(a.fontMono, 9.5f);
  for (int v = 32; v < 128; v += 32) dl->AddText(ImVec2(p.x + W - 22, vy(v) - 12), P.textFaint, std::to_string(v).c_str());
  ImGui::PopFont();
  // Hit test: topmost zone under the mouse (selected first), with edge detection.
  int hitZone = -1, hitEdge = 0;
  if (hov && (!act || ImGui::IsItemActivated())) {
    auto test = [&](int i) {
      const Zone& z = m.zones[static_cast<size_t>(i)];
      float x0 = kx(z.lo), x1 = kx(z.hi + 1), y0 = vy(z.vhi + 1), y1 = vy(z.vlo);
      ImVec2 mp = io.MousePos;
      if (mp.x < x0 - 3 || mp.x > x1 + 3 || mp.y < y0 - 3 || mp.y > y1 + 3) return false;
      hitZone = i;
      hitEdge = 1;
      if (std::fabs(mp.x - x0) < 4) hitEdge = 2;
      else if (std::fabs(mp.x - x1) < 4) hitEdge = 3;
      else if (std::fabs(mp.y - y0) < 4) hitEdge = 4;
      else if (std::fabs(mp.y - y1) < 4) hitEdge = 5;
      return true;
    };
    if (!(ed.sel >= 0 && ed.sel < static_cast<int>(m.zones.size()) && test(ed.sel)))
      for (int i = static_cast<int>(m.zones.size()) - 1; i >= 0; --i)
        if (test(i)) break;
    if (hitEdge == 2 || hitEdge == 3) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (hitEdge == 4 || hitEdge == 5) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  }
  if (ImGui::IsItemActivated()) {
    if (hitZone >= 0) {
      ed.sel = hitZone;
      ed.drag = hitEdge;
      ed.dragZone = hitZone;
      ed.orig = m.zones[static_cast<size_t>(hitZone)];
      ed.start = io.MousePos;
    } else {
      ed.drag = 0;
    }
  }
  if (act && ed.drag && ed.dragZone >= 0 && ed.dragZone < static_cast<int>(m.zones.size())) {
    int dk = static_cast<int>(std::lround((io.MousePos.x - ed.start.x) / keyW));
    int dv = static_cast<int>(std::lround(-(io.MousePos.y - ed.start.y) / H * 128.0f));
    Zone z = ed.orig;
    switch (ed.drag) {
      case 1: {
        dk = std::clamp(dk, -z.lo, 127 - z.hi);
        dv = std::clamp(dv, -z.vlo, 127 - z.vhi);
        z.lo += dk, z.hi += dk, z.root = std::clamp(z.root + dk, 0, 127);
        z.vlo += dv, z.vhi += dv;
        break;
      }
      case 2: z.lo = std::clamp(z.lo + dk, 0, z.hi); break;
      case 3: z.hi = std::clamp(z.hi + dk, z.lo, 127); break;
      case 4: z.vhi = std::clamp(z.vhi + dv, z.vlo, 127); break;
      case 5: z.vlo = std::clamp(z.vlo + dv, 0, z.vhi); break;
      default: break;
    }
    if (!(z == m.zones[static_cast<size_t>(ed.dragZone)])) {
      m.zones[static_cast<size_t>(ed.dragZone)] = z;
      commitMap(a, t, m, "edit zone mapping", mergeKey(t, 1000 + ed.drag));
    }
  }
  if (ImGui::IsItemDeactivated()) ed.drag = 0;
  // Zones (selected on top).
  auto drawZone = [&](int i, bool sel) {
    const Zone& z = m.zones[static_cast<size_t>(i)];
    ImVec2 a0(kx(z.lo) + 1, vy(z.vhi + 1) + 1), a1(kx(z.hi + 1) - 1, vy(z.vlo) - 1);
    ImU32 fill = sel ? theme::withAlpha(P.accent, 0.38f) : theme::withAlpha(P.text, i == hitZone ? 0.16f : 0.09f);
    dl->AddRectFilled(a0, a1, fill, 3);
    dl->AddRect(a0, a1, sel ? P.accent : theme::withAlpha(P.text, 0.35f), 3, 0, sel ? 1.5f : 1.0f);
    // Root key tick.
    float rx = kx(z.root + 0.5);
    if (rx > a0.x && rx < a1.x) dl->AddLine(ImVec2(rx, a1.y - 6), ImVec2(rx, a1.y), sel ? P.paper : P.textDim, 2);
    if (a1.x - a0.x > 28 && a1.y - a0.y > 14) {
      dl->PushClipRect(a0, a1, true);
      ImGui::PushFont(nullptr, 11.0f);
      dl->AddText(ImVec2(a0.x + 4, a0.y + 2), sel ? P.paper : P.textDim, zoneLabel(z).c_str());
      ImGui::PopFont();
      dl->PopClipRect();
    }
  };
  for (int i = 0; i < static_cast<int>(m.zones.size()); ++i)
    if (i != ed.sel) drawZone(i, false);
  if (ed.sel >= 0 && ed.sel < static_cast<int>(m.zones.size())) drawZone(ed.sel, true);
  if (hov) {
    float x = io.MousePos.x;
    dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + H), theme::withAlpha(P.text, 0.15f));
    a.setHint("Key " + noteName(keyAt(x)) + " · velocity " + std::to_string(velAt(io.MousePos.y)) +
              "   drag a zone to move · drag edges to resize · wheel: zoom · Shift+wheel: scroll · drop samples · right-click: menu");
  }
  dl->PopClipRect();
  // Right-click menu on a zone.
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hitZone >= 0) {
    ed.sel = hitZone;
    ImGui::OpenPopup("##mapmenu");
  }
  if (ImGui::BeginPopup("##mapmenu")) {
    zoneMenu(a, t, m, ed, ed.sel);
    ImGui::EndPopup();
  }
  // Drop a sample on a key.
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
      std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (isAudioFile(path)) addFiles(a, t, {path}, false, false, keyAt(io.MousePos.x));
    }
    ImGui::EndDragDropTarget();
  }

  // Keyboard strip aligned with the map columns.
  ImVec2 kp(p.x, p.y + H + 2);
  ImGui::SetCursorScreenPos(kp);
  ImGui::InvisibleButton("##keys", ImVec2(W, kbH));
  bool khov = ImGui::IsItemHovered();
  dl->PushClipRect(kp, ImVec2(kp.x + W, kp.y + kbH), true);
  int sel = ed.sel;
  for (int k = static_cast<int>(ed.viewLo); k <= static_cast<int>(ed.viewLo + ed.viewKeys) && k < 128; ++k) {
    float x0 = kx(k), x1 = kx(k + 1);
    bool black = isBlack(k), held = ed.heldKey == k;
    ImU32 c = held ? P.accent : black ? P.surface700 : theme::mix(P.text, P.surface900, 0.15f);
    dl->AddRectFilled(ImVec2(x0, kp.y), ImVec2(x1 - 1, kp.y + kbH), c, 1);
    for (size_t i = 0; i < m.zones.size(); ++i)
      if (m.zones[i].root == k) {
        bool s = static_cast<int>(i) == sel;
        dl->AddTriangleFilled(ImVec2((x0 + x1) * 0.5f, kp.y + 3 + 5), ImVec2(x0 + 1, kp.y + 3), ImVec2(x1 - 1, kp.y + 3),
                              s ? P.accent : (black ? P.text : P.surface800));
      }
    if (k % 12 == 0 && keyW * 12 > 34) {
      ImGui::PushFont(a.fontMono, 9.5f);
      dl->AddText(ImVec2(x0 + 2, kp.y + kbH - 13), P.surface800, noteName(k).c_str());
      ImGui::PopFont();
    }
  }
  dl->PopClipRect();
  if (ImGui::IsItemActivated()) {
    int k = keyAt(io.MousePos.x);
    float vel = std::clamp(0.35f + 0.65f * (io.MousePos.y - kp.y) / kbH, 0.1f, 1.0f);
    ed.heldKey = k;
    preview(a, t, k, vel, true);
  }
  if (ImGui::IsItemDeactivated() && ed.heldKey >= 0) {
    preview(a, t, ed.heldKey, 0, false);
    ed.heldKey = -1;
  }
  if (khov) a.setHint("Click to play " + noteName(keyAt(io.MousePos.x)) + " (lower = louder) · triangles mark root keys");
}

void msSampleTab(App& a, const PluginTarget& t, ZoneMap& m, MsEd& ed, float width) {
  const auto& P = pal();
  if (ed.sel < 0 || ed.sel >= static_cast<int>(m.zones.size())) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Select a zone to edit its sample, loop and tuning."));
    return;
  }
  Zone& z = m.zones[static_cast<size_t>(ed.sel)];
  SamplePtr smp = resolveSample(z.path);
  const float waveW = width - 280, waveH = 132;
  ImGui::BeginGroup();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##wave", ImVec2(waveW, waveH));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  dl->AddRectFilled(p, ImVec2(p.x + waveW, p.y + waveH), P.surface950, 6);
  const int64_t n = smp ? static_cast<int64_t>(smp->frames()) : 0;
  ImGuiIO& io = ImGui::GetIO();
  if (n > 1) {
    auto fx = [&](int64_t f) { return p.x + static_cast<float>(static_cast<double>(f) / static_cast<double>(n) * waveW); };
    int64_t endF = z.end > 0 ? std::min<int64_t>(z.end, n) : n;
    int64_t ls = std::min<int64_t>(z.loopStart, n), le = z.loopEnd > 0 ? std::min<int64_t>(z.loopEnd, n) : n;
    bool loopOn = z.loop == LoopForward || z.loop == LoopSustain || z.loop == LoopBounce;
    if (loopOn) dl->AddRectFilled(ImVec2(fx(ls), p.y), ImVec2(fx(le), p.y + waveH), theme::withAlpha(P.accent, 0.14f));
    drawWave(dl, smp.get(), 0, n, p, ImVec2(waveW, waveH), P.text);
    dl->AddRectFilled(p, ImVec2(fx(z.start), p.y + waveH), theme::withAlpha(P.surface950, 0.65f));
    dl->AddRectFilled(ImVec2(fx(endF), p.y), ImVec2(p.x + waveW, p.y + waveH), theme::withAlpha(P.surface950, 0.65f));
    struct Mk {
      int64_t f;
      const char* tag;
      bool on;
    } mk[4] = {{z.start, "S", true}, {ls, "L", loopOn}, {le, "E", loopOn}, {endF, "END", true}};
    for (int k = 0; k < 4; ++k) {
      if (!mk[k].on) continue;
      float x = fx(mk[k].f);
      ImU32 c = k == 1 || k == 2 ? P.accent : P.paper;
      dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + waveH), c, ed.marker == k ? 2.0f : 1.2f);
      ImGui::PushFont(a.fontMono, 9.5f);
      ImVec2 ts = ImGui::CalcTextSize(mk[k].tag);
      float tx = k == 3 || k == 2 ? x - ts.x - 6 : x;
      dl->AddRectFilled(ImVec2(tx, p.y + (k == 1 || k == 2 ? waveH - 14 : 0)), ImVec2(tx + ts.x + 6, p.y + (k == 1 || k == 2 ? waveH : 14)), c, 2);
      dl->AddText(ImVec2(tx + 3, p.y + (k == 1 || k == 2 ? waveH - 13 : 1)), P.ink, mk[k].tag);
      ImGui::PopFont();
    }
    // Marker drag.
    if (hov && (!act || ImGui::IsItemActivated())) {
      int near = -1;
      float best = 7;
      for (int k = 0; k < 4; ++k)
        if (mk[k].on && std::fabs(io.MousePos.x - fx(mk[k].f)) < best) best = std::fabs(io.MousePos.x - fx(mk[k].f)), near = k;
      if (near >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      if (ImGui::IsItemActivated()) ed.marker = near;
      a.setHint("Drag S / END (start, end) and L / E (loop) markers · click elsewhere to audition the zone");
    }
    if (ImGui::IsItemActivated() && ed.marker < 0) {
      ed.heldKey = z.root;
      preview(a, t, z.root, 0.8f, true);
    }
    if (act && ed.marker >= 0) {
      int64_t f = std::clamp<int64_t>(static_cast<int64_t>((io.MousePos.x - p.x) / waveW * static_cast<float>(n)), 0, n);
      Zone o = z;
      switch (ed.marker) {
        case 0: z.start = std::clamp<int64_t>(f, 0, endF - 2); break;
        case 1: z.loopStart = std::clamp<int64_t>(f, 0, le - 2); break;
        case 2: z.loopEnd = std::clamp<int64_t>(f, ls + 2, endF); if (z.loopEnd >= n) z.loopEnd = 0; break;
        default: z.end = std::clamp<int64_t>(f, z.start + 2, n); if (z.end >= n) z.end = 0; break;
      }
      if (!(o == z)) commitMap(a, t, m, "move sample marker", mergeKey(t, 2000 + ed.marker));
    }
    if (ImGui::IsItemDeactivated()) {
      ed.marker = -1;
      if (ed.heldKey >= 0) preview(a, t, ed.heldKey, 0, false), ed.heldKey = -1;
    }
    char info[160];
    std::snprintf(info, sizeof info, "%s   %.3f s · %.0f Hz · %lld frames", zoneLabel(z).c_str(), smp->seconds(), smp->sampleRate,
                  static_cast<long long>(n));
    ImGui::PushFont(a.fontMono, 10.0f);
    dl->AddText(ImVec2(p.x + 8, p.y + 16), P.textDim, info);
    ImGui::PopFont();
  } else {
    const char* msg = "Sample not found";
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p.x + (waveW - ts.x) * 0.5f, p.y + (waveH - ts.y) * 0.5f), P.danger, msg);
  }
  // Loop mode, reverse, crossfade.
  ImGui::Dummy(ImVec2(0, 2));
  smallCaps(a, "LOOP");
  ImGui::SameLine(0, 8);
  int lm = segmented("loopmode", {"Off", "One-shot", "Forward", "Sustain", "Ping-pong"}, z.loop, 360);
  if (lm >= 0 && lm != z.loop) {
    z.loop = lm;
    if (lm >= LoopForward && z.loopEnd == 0 && z.loopStart == 0 && n > 4) z.loopStart = n / 4;
    commitMap(a, t, m, "zone loop mode");
  }
  ImGui::SameLine(0, 10);
  if (ui::TextButton("Reverse", z.reverse, 70, "Play the sample backwards")) {
    z.reverse = !z.reverse;
    commitMap(a, t, m, "zone reverse");
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, 10);
  ImGui::BeginGroup();
  double rate = smp ? smp->sampleRate : 44100.0;
  double v;
  v = z.root;
  if (valueKnob(a, "root", v, pd("Root key", "note", 0, 127, 60), "Root", 62)) z.root = static_cast<int>(std::lround(v)), commitMap(a, t, m, "zone root", mergeKey(t, 1));
  ImGui::SameLine(0, 0);
  v = z.tune;
  if (valueKnob(a, "tune", v, pd("Tune", "ct", -1200, 1200, 0), "Tune", 62)) z.tune = static_cast<float>(std::round(v)), commitMap(a, t, m, "zone tune", mergeKey(t, 2));
  ImGui::SameLine(0, 0);
  v = z.gain;
  if (valueKnob(a, "gain", v, pd("Gain", "dB", -48, 12, 0), "Gain", 62)) z.gain = static_cast<float>(v), commitMap(a, t, m, "zone gain", mergeKey(t, 3));
  ImGui::SameLine(0, 0);
  v = z.pan;
  if (valueKnob(a, "pan", v, pd("Pan", "pan", -1, 1, 0), "Pan", 62)) z.pan = static_cast<float>(v), commitMap(a, t, m, "zone pan", mergeKey(t, 4));
  v = static_cast<double>(z.xfade) / rate * 1000.0;
  if (valueKnob(a, "xfade", v, pd("Loop crossfade", "ms", 0, 500, 0), "Crossfade", 62))
    z.xfade = static_cast<int64_t>(v / 1000.0 * rate), commitMap(a, t, m, "zone loop crossfade", mergeKey(t, 5));
  ImGui::SameLine(0, 0);
  v = z.keyTrack;
  if (valueKnob(a, "kt", v, pd("Key tracking", "%", 0, 2, 1), "Key track", 62)) z.keyTrack = static_cast<float>(v), commitMap(a, t, m, "zone key tracking", mergeKey(t, 6));
  ImGui::EndGroup();
}

// ---------------------------------------------------------------------------
// Multisampler in the reference wave-player layout (J-2). Reference form
// (left part, browser omitted) 740 × 627:
//   top bar (7..735, 13..31): disk / content / help buttons, mode label,
//     channel selector (415, 14, 288×16)
//   zone grid panel (7, 61, 722×331): zone list + key × velocity zone map
//   page selector (7, 396, 23×225) and the page area (30, 396, 660×225):
//     SAMPLE (waveform + start / loop), PROGRAM (play mode, volume, glide, a
//     grid of 30 px wheels in five columns x 94 / 220 / 348 / 503 / 629, rows
//     y 82 / 116 / 150 / 184), ZONE (tabbed panels: main | filter | envelope |
//     LFO | modulation at x 5 / 131 / 257 / 413 / 539, y 23, 122..152 × 140)
//   output column (691, 400, 26×217).
constexpr float kDwW = 740, kDwH = 627;

iskin::Palette dwPalette() {
  iskin::Palette p;
  p.bgTop = iskin::rgb(0x2A3B42);
  p.bgBottom = iskin::rgb(0x1E2C32);
  p.panel = iskin::rgb(0x343434);
  p.panelLine = iskin::rgb(0x15191B);
  p.rail = iskin::rgb(0x43454A);
  p.fill = iskin::rgb(0xA8C61F);
  p.fill2 = iskin::rgb(0xD4D5C9);
  p.fill3 = iskin::rgb(0x6781A0);
  p.cap = iskin::rgb(0x3A3D42);
  p.handle = iskin::rgb(0x575350);
  p.label = iskin::rgb(0x9BA6AC);
  p.text = iskin::rgb(0xD4D5C9);
  p.textDim = iskin::rgb(0x9BA6AC);
  p.lcdBg = iskin::rgb(0x25272C);
  p.lcdText = iskin::rgb(0xB9BFC3);
  p.led = iskin::rgb(0xA8C61F);
  p.ledOff = iskin::rgb(0x444842);
  return p;
}

struct DwUi {
  int page = 2;     // 0 sample, 1 program, 2 zone
  int zoneTab[5] = {0, 0, 0, 0, 0};
};
std::map<uint64_t, DwUi> gDw;

void dwTabs(const iskin::Skin& k, const char* id, float x, float y, float w, const std::vector<const char*>& items, int& cur) {
  const float tw = w / static_cast<float>(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (iskin::button(k, id, x + tw * static_cast<float>(i), y, tw - 1, 18, items[i], cur == static_cast<int>(i), 8.5f, iskin::rgb(0x6781A0)))
      cur = static_cast<int>(i);
    ImGui::PopID();
  }
}

void dwProgramPage(App& a, const PluginTarget& t, const iskin::Skin& k, float ox, float oy) {
  auto W = [&](int param, float x, float y, const char* cap) {
    iskin::wheel(a, t, k, param, ox + x, oy + y, 30, k.c.fill, iskin::WheelBigLine);
    iskin::text(k, ox + x - 80, oy + y + 8, 76, 14, cap, 8.5f, k.c.textDim, 2, false, true);
  };
  auto head = [&](float x, const char* s) {
    k.dl()->AddLine(k.at(ox + x - 82, oy + 74), k.at(ox + x + 32, oy + 74), iskin::rgb(0x3E414A));
    iskin::text(k, ox + x - 82, oy + 62, 114, 11, s, 8.5f, k.c.fill2, 0, false, true);
  };
  // Row 0: play mode, volume, glide, filter type, polyphony.
  iskin::text(k, ox + 14, oy + 22, 82, 11, "PLAY MODE", 8.5f, k.c.textDim, 0, false, true);
  iskin::digi(a, t, k, MsPlayMode, ox + 14, oy + 36, 82, 17, {}, 9);
  W(MsVolume, 181, 24, "VOLUME");
  W(MsGlide, 320, 24, "GLIDE");
  iskin::text(k, ox + 380, oy + 22, 100, 11, "ROUND ROBIN", 8.5f, k.c.textDim, 0, false, true);
  iskin::digi(a, t, k, MsRoundRobin, ox + 380, oy + 36, 100, 17, {}, 9);
  iskin::text(k, ox + 500, oy + 22, 60, 11, "VOICES", 8.5f, k.c.textDim, 0, false, true);
  iskin::digi(a, t, k, MsPolyphony, ox + 500, oy + 36, 44, 17, [](double v) { return std::to_string(static_cast<int>(std::lround(v))); }, 9);
  iskin::text(k, ox + 560, oy + 22, 90, 11, "QUALITY", 8.5f, k.c.textDim, 0, false, true);
  iskin::digi(a, t, k, MsInterp, ox + 560, oy + 36, 90, 17, {}, 9);
  // Five columns.
  head(94, "LFO");
  W(MsLfoRate, 94, 82, "RATE");
  W(MsLfoDelay, 94, 116, "FADE IN");
  W(MsTranspose, 94, 150, "TRANSPOSE");
  W(MsFine, 94, 184, "FINE");
  iskin::digi(a, t, k, MsLfoShape, ox + 14, oy + 198, 76, 15, {}, 8.5f);
  head(220, "LFO >");
  W(MsLfoPitch, 220, 82, "PITCH");
  W(MsLfoCutoff, 220, 116, "CUTOFF");
  W(MsLfoAmp, 220, 150, "VOLUME");
  W(MsLfoPan, 220, 184, "PAN");
  head(348, "AMP ENV");
  W(MsAttack, 348, 82, "ATTACK");
  W(MsHold, 348, 116, "HOLD");
  W(MsDecay, 348, 150, "DECAY");
  W(MsRelease, 348, 184, "RELEASE");
  iskin::slider(a, t, k, MsSustain, ox + 386, oy + 96, 19, 116, k.c.fill, false);
  iskin::text(k, ox + 380, oy + 212, 32, 10, "SUS", 7.5f, k.c.textDim, 1, false, true);
  head(503, "FILTER");
  iskin::digi(a, t, k, MsFilterType, ox + 424, oy + 62, 76, 11, {}, 8);
  W(MsCutoff, 503, 82, "CUTOFF");
  W(MsResonance, 503, 116, "RESO");
  W(MsKeyTrack, 503, 150, "KEY TRACK");
  W(MsVelCutoff, 503, 184, "VELOCITY");
  head(629, "MOD ENV");
  W(MsModAttack, 629, 82, "ATTACK");
  W(MsModDecay, 629, 116, "DECAY");
  W(MsModSustain, 629, 150, "SUSTAIN");
  W(MsModRelease, 629, 184, "RELEASE");
  W(MsEnvAmount, 629, 24, "> CUTOFF");
}

void dwZonePage(App& a, const PluginTarget& t, const iskin::Skin& k, float ox, float oy, ZoneMap& m, MsEd& ed, DwUi& ui) {
  if (ed.sel < 0 || ed.sel >= static_cast<int>(m.zones.size())) {
    iskin::text(k, ox, oy, 660, 225, "Select a zone in the map above", 10, k.c.textDim, 1);
    return;
  }
  Zone& z = m.zones[static_cast<size_t>(ed.sel)];
  const ImU32 grn = k.c.fill;
  auto panel = [&](float x, float w) { iskin::bevel(k, ox + x, oy + 23, w, 140, iskin::rgb(0x2F3237), k.c.panelLine, 2); };
  auto V = [&](const char* id, double& v, const ParamDesc& d, float x, float y, const char* cap) {
    iskin::text(k, ox + x - 14, oy + y + 31, 58, 10, cap, 7.5f, k.c.textDim, 1, false, true);
    return iskin::wheelValue(a, k, id, v, d, ox + x, oy + y, 30, grn, iskin::WheelBigLine);
  };
  double v;
  // MAIN | TIME | TRIGGER
  dwTabs(k, "##zt0", ox + 4, oy + 5, 124, {"MAIN", "TIME", "TRIGGER"}, ui.zoneTab[0]);
  panel(5, 122);
  if (ui.zoneTab[0] == 0) {
    v = z.tune;
    if (V("ztune", v, pd("Zone pitch", "ct", -1200, 1200, 0), 18, 34, "PITCH")) z.tune = static_cast<float>(std::round(v)), commitMap(a, t, m, "zone pitch", mergeKey(t, 2));
    v = z.gain;
    if (V("zgain", v, pd("Zone gain", "dB", -48, 12, 0), 76, 34, "GAIN")) z.gain = static_cast<float>(v), commitMap(a, t, m, "zone gain", mergeKey(t, 3));
    v = z.pan;
    if (V("zpan", v, pd("Zone pan", "pan", -1, 1, 0), 18, 90, "PAN")) z.pan = static_cast<float>(v), commitMap(a, t, m, "zone pan", mergeKey(t, 4));
    v = z.keyTrack;
    if (V("zkt", v, pd("Zone key tracking", "%", 0, 2, 1), 76, 90, "KEY TRK")) z.keyTrack = static_cast<float>(v), commitMap(a, t, m, "zone key tracking", mergeKey(t, 6));
  } else if (ui.zoneTab[0] == 1) {
    v = z.root;
    iskin::text(k, ox + 12, oy + 30, 100, 10, "ROOT KEY", 7.5f, k.c.textDim, 0, false, true);
    if (iskin::digiValue(a, k, "zroot", v, pd("Root key", "note", 0, 127, 60), ox + 12, oy + 42, 50, 17,
                         [](double x) { return noteName(static_cast<int>(std::lround(x))); }, 9))
      z.root = static_cast<int>(std::lround(v)), commitMap(a, t, m, "zone root", mergeKey(t, 1));
    iskin::text(k, ox + 12, oy + 66, 100, 10, "LOOP", 7.5f, k.c.textDim, 0, false, true);
    static const char* lm[5] = {"OFF", "1-SHOT", "FWD", "SUST", "PING"};
    for (int i = 0; i < 5; ++i) {
      ImGui::PushID(i);
      if (iskin::button(k, "##zlm", ox + 10 + (i % 3) * 38, oy + 78 + (i / 3) * 20, 36, 18, lm[i], z.loop == i, 7.5f)) {
        z.loop = i;
        commitMap(a, t, m, "zone loop mode");
      }
      ImGui::PopID();
    }
    if (iskin::button(k, "##zrev", ox + 86, oy + 98, 36, 18, "REV", z.reverse, 7.5f)) z.reverse = !z.reverse, commitMap(a, t, m, "zone reverse");
  } else {
    auto D = [&](const char* id, int& field, int lo, int hi, float x, float y, const char* cap, bool note, int mk) {
      double dv = field;
      iskin::text(k, ox + x, oy + y - 11, 56, 10, cap, 7.5f, k.c.textDim, 0, false, true);
      if (iskin::digiValue(a, k, id, dv, pd(cap, "", lo, hi, lo), ox + x, oy + y, 52, 16,
                           [note](double x2) { return note ? noteName(static_cast<int>(std::lround(x2))) : std::to_string(static_cast<int>(std::lround(x2))); }, 8.5f)) {
        field = static_cast<int>(std::lround(dv));
        commitMap(a, t, m, cap, mergeKey(t, mk));
      }
    };
    D("zlo", z.lo, 0, 127, 10, 42, "KEY LO", true, 10);
    D("zhi", z.hi, 0, 127, 66, 42, "KEY HI", true, 11);
    D("zvlo", z.vlo, 0, 127, 10, 78, "VEL LO", false, 12);
    D("zvhi", z.vhi, 0, 127, 66, 78, "VEL HI", false, 13);
    D("zgrp", z.group, 0, kMaxGroups - 1, 10, 114, "GROUP", false, 14);
    D("zcut", z.choke, 0, 32, 66, 114, "CUT", false, 15);
    z.lo = std::min(z.lo, z.hi), z.vlo = std::min(z.vlo, z.vhi);
  }
  // FILTER
  dwTabs(k, "##zt1", ox + 130, oy + 5, 124, {"FILTER"}, ui.zoneTab[1]);
  panel(131, 122);
  v = z.cutoff > 0 ? z.cutoff : 20000;
  if (V("zfc", v, pd("Zone low-pass", "Hz", 20, 20000, 20000, true), 176, 40, "CUTOFF")) {
    z.cutoff = v >= 19900 ? 0.f : static_cast<float>(v);
    commitMap(a, t, m, "zone low-pass", mergeKey(t, 16));
  }
  iskin::text(k, ox + 136, oy + 100, 112, 14, "ZONE LOW-PASS", 7.5f, k.c.textDim, 1, false, true);
  iskin::text(k, ox + 136, oy + 116, 112, 14, "(program filter: PROGRAM)", 7.5f, k.c.textDim, 1);
  // AMP envelope (zone's own, or the program's)
  dwTabs(k, "##zt2", ox + 256, oy + 5, 154, {"AMP"}, ui.zoneTab[2]);
  panel(257, 152);
  if (iskin::button(k, "##zownenv", ox + 262, oy + 27, 142, 15, z.ownEnv ? "ZONE ENVELOPE" : "PROGRAM ENVELOPE", z.ownEnv, 7.5f)) {
    z.ownEnv = !z.ownEnv;
    commitMap(a, t, m, "zone envelope");
  }
  {
    double e[5] = {z.attack, z.hold, z.decay, z.sustain, z.release};
    ParamDesc ds[5] = {pd("Zone attack", "s", 0.001, 10, 0.002, true), pd("Zone hold", "s", 0, 5, 0), pd("Zone decay", "s", 0.01, 20, 0.5, true),
                       pd("Zone sustain", "%", 0, 1, 1), pd("Zone release", "s", 0.005, 20, 0.3, true)};
    const char* ids[5] = {"zea", "zeh", "zed", "zes", "zer"};
    const char* caps[5] = {"A", "H", "D", "S", "R"};
    for (int i = 0; i < 5; ++i) {
      iskin::text(k, ox + 264 + i * 28, oy + 146, 22, 12, caps[i], 8, k.c.textDim, 1, false, true);
      if (!z.ownEnv) continue;
      if (iskin::sliderValue(a, k, ids[i], e[i], ds[i], ox + 266 + i * 28, oy + 46, 18, 98, grn)) {
        z.attack = static_cast<float>(e[0]), z.hold = static_cast<float>(e[1]), z.decay = static_cast<float>(e[2]);
        z.sustain = static_cast<float>(e[3]), z.release = static_cast<float>(e[4]);
        commitMap(a, t, m, "zone envelope", mergeKey(t, 20 + i));
      }
    }
    if (!z.ownEnv) iskin::text(k, ox + 260, oy + 50, 146, 90, "follows the program\namplitude envelope", 8, k.c.textDim, 1);
  }
  // LFO / MOD: program-level in Gridline (summaries + jump).
  dwTabs(k, "##zt3", ox + 412, oy + 5, 124, {"LFO"}, ui.zoneTab[3]);
  panel(413, 122);
  iskin::wheel(a, t, k, MsLfoRate, ox + 430, oy + 40, 30, grn, iskin::WheelBigLine);
  iskin::wheel(a, t, k, MsLfoPitch, ox + 486, oy + 40, 30, grn, iskin::WheelBigLine);
  iskin::wheel(a, t, k, MsLfoCutoff, ox + 430, oy + 96, 30, grn, iskin::WheelBigLine);
  iskin::wheel(a, t, k, MsLfoAmp, ox + 486, oy + 96, 30, grn, iskin::WheelBigLine);
  iskin::text(k, ox + 416, oy + 71, 64, 10, "RATE", 7.5f, k.c.textDim, 1, false, true);
  iskin::text(k, ox + 472, oy + 71, 64, 10, "> PITCH", 7.5f, k.c.textDim, 1, false, true);
  iskin::text(k, ox + 416, oy + 127, 64, 10, "> CUTOFF", 7.5f, k.c.textDim, 1, false, true);
  iskin::text(k, ox + 472, oy + 127, 64, 10, "> VOLUME", 7.5f, k.c.textDim, 1, false, true);
  dwTabs(k, "##zt4", ox + 538, oy + 5, 124, {"MOD ENV"}, ui.zoneTab[4]);
  panel(539, 122);
  iskin::wheel(a, t, k, MsModAttack, ox + 556, oy + 40, 30, grn, iskin::WheelBigLine);
  iskin::wheel(a, t, k, MsModDecay, ox + 612, oy + 40, 30, grn, iskin::WheelBigLine);
  iskin::wheel(a, t, k, MsModPitch, ox + 556, oy + 96, 30, grn, iskin::WheelBigLine);
  iskin::wheel(a, t, k, MsEnvAmount, ox + 612, oy + 96, 30, grn, iskin::WheelBigLine);
  iskin::text(k, ox + 542, oy + 71, 64, 10, "ATTACK", 7.5f, k.c.textDim, 1, false, true);
  iskin::text(k, ox + 598, oy + 71, 64, 10, "DECAY", 7.5f, k.c.textDim, 1, false, true);
  iskin::text(k, ox + 542, oy + 127, 64, 10, "> PITCH", 7.5f, k.c.textDim, 1, false, true);
  iskin::text(k, ox + 598, oy + 127, 64, 10, "> CUTOFF", 7.5f, k.c.textDim, 1, false, true);
  // Bottom strip: zone label + path.
  std::string info = zoneLabel(z) + "   " + noteName(z.lo) + "-" + noteName(z.hi) + "  vel " + std::to_string(z.vlo) + "-" + std::to_string(z.vhi) +
                     "  root " + noteName(z.root);
  iskin::text(k, ox + 8, oy + 170, 640, 14, info.c_str(), 9, k.c.text, 0);
  std::string pathTxt = z.path.rfind("tone:", 0) == 0 ? "built-in tone" : z.path;
  iskin::text(k, ox + 8, oy + 186, 640, 12, pathTxt.c_str(), 8, k.c.textDim, 0, true, false);
}

void editorDirectWave(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  MsEd& ed = gMs[t.key()];
  DwUi& ui = gDw[t.key()];
  ZoneMap m = currentMap(st);
  if (ed.sel >= static_cast<int>(m.zones.size())) ed.sel = static_cast<int>(m.zones.size()) - 1;
  if (ed.sel < 0 && !m.zones.empty()) ed.sel = 0;
  ImGui::PushID("dw");
  iskin::Skin k = iskin::begin(width, kDwW, kDwH, dwPalette());
  iskin::background(k, 0, 0, kDwW, kDwH, k.c.bgTop, k.c.bgBottom, 4);
  // ---- top bar: disk operations (add / folder / soundfont), program name, mapping tools.
  PluginTarget tt = t;
  if (iskin::button(k, "##dwadd", 7, 13, 44, 18, "ADD", false, 8.5f)) {
    bool am = ed.autoMapOnAdd;
    openDialog(a, "Add sample", kAudioExts, [&a, tt, am](const std::string& p) { addFiles(a, tt, {p}, false, am); });
  }
  if (iskin::button(k, "##dwfolder", 53, 13, 56, 18, "FOLDER", false, 8.5f)) {
    openDialog(a, "Load folder of samples", {"/"}, [&a, tt](const std::string& dir) {
      std::vector<std::string> files;
      std::error_code ec;
      for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (isAudioFile(it->path().string())) files.push_back(it->path().string());
      std::sort(files.begin(), files.end());
      if (files.empty()) a.notify("No audio files in " + dir, true);
      addFiles(a, tt, files, true, true);
    });
  }
  if (iskin::button(k, "##dwsf2", 111, 13, 44, 18, "SF2", false, 8.5f)) {
    openDialog(a, "Load SoundFont", {".sf2"}, [&a, tt](const std::string& p) {
      std::string err;
      ZoneMap nm = sf2PresetMap(p, 0, &err);
      if (nm.sf2Path.empty()) {
        a.notify(err.empty() ? "Cannot read " + p : err, true);
        return;
      }
      gMs[tt.key()].sel = 0;
      commitMap(a, tt, nm, "load soundfont");
    });
  }
  {
    const bool blank = st.data.find_first_not_of(" \t\r\n") == std::string::npos;
    std::string title;
    if (blank) title = "Factory: " + factorySetNames()[static_cast<size_t>(std::clamp(static_cast<int>(val(st, MsFactorySet) + 0.5f), 0, 4))];
    else if (!m.sf2Path.empty()) title = m.sf2Name + "  (" + fs::path(m.sf2Path).filename().string() + ")";
    else title = std::to_string(m.zones.size()) + (m.zones.size() == 1 ? " zone" : " zones");
    iskin::lcd(k, 159, 13, 250, 18, title.c_str(), k.c.lcdBg, k.c.lcdText, 9);
    if (blank) iskin::digi(a, t, k, MsFactorySet, 415, 13, 100, 18, {}, 8.5f);
    if (!m.sf2Path.empty()) {
      auto f = loadSf2(m.sf2Path);
      int cur = -1;
      if (f)
        for (size_t i = 0; i < f->presets.size(); ++i)
          if (f->presets[i].bank == m.sf2Bank && f->presets[i].program == m.sf2Program) cur = static_cast<int>(i);
      auto pick = [&](int i) {
        if (!f || i < 0 || i >= static_cast<int>(f->presets.size())) return;
        ZoneMap nm = sf2PresetMap(m.sf2Path, static_cast<size_t>(i));
        ed.sel = 0;
        commitMap(a, t, nm, "change soundfont preset");
      };
      if (iskin::button(k, "##sfp", 415, 13, 22, 18, "<", false, 9)) pick(cur - 1);
      if (iskin::button(k, "##sfn", 439, 13, 22, 18, ">", false, 9)) pick(cur + 1);
    }
  }
  if (iskin::button(k, "##dwauto", 525, 13, 66, 18, "AUTO-MAP", false, 8)) autoMap(m.zones), commitMap(a, t, m, "auto-map zones");
  if (iskin::button(k, "##dwdrum", 593, 13, 46, 18, "DRUMS", false, 8)) mapAsDrumKit(m.zones, 36), commitMap(a, t, m, "map zones as drum kit");
  if (iskin::button(k, "##dwclear", 641, 13, 44, 18, "CLEAR", false, 8)) {
    ZoneMap e;
    commitMap(a, t, e, "clear zones");
  }
  if (iskin::button(k, "##dwfact", 687, 13, 46, 18, "FACTORY", false, 7.5f) && !st.data.empty()) {
    PluginState ns = st;
    ns.data.clear();
    setPluginState(a, t, ns, "use factory multisample");
  }
  // ---- zone grid panel: list + map.
  iskin::bevel(k, 7, 40, 722, 352, iskin::rgb(0x282B2E), k.c.panelLine, 3);
  iskin::text(k, 14, 42, 200, 16, "ZONES", 9, k.c.label, 0, false, true);
  ImGui::SetCursorScreenPos(k.at(14, 60));
  msZoneList(a, t, m, ed, ImVec2(k.px(222), k.px(326)));
  ImGui::SetCursorScreenPos(k.at(242, 60));
  ImGui::BeginGroup();
  msZoneMap(a, t, m, ed, ImVec2(k.px(482), k.px(326)));
  ImGui::EndGroup();
  // ---- page selector + pages.
  static const char* pages[3] = {"SAMPLE", "PROGRAM", "ZONE"};
  for (int p = 0; p < 3; ++p) {
    const float py = 396 + p * 75.0f;
    ImGui::PushID(p);
    ImGui::SetCursorScreenPos(k.at(7, py));
    if (ImGui::InvisibleButton("##page", ImVec2(k.px(23), k.px(73)))) ui.page = p;
    ImGui::PopID();
    iskin::bevel(k, 7, py, 23, 73, ui.page == p ? iskin::rgb(0x6781A0) : iskin::rgb(0x343434), k.c.panelLine, 2);
    const int n = static_cast<int>(std::strlen(pages[p]));
    for (int i = 0; i < n; ++i) {
      char ch[2] = {pages[p][i], 0};
      iskin::text(k, 7, py + 36 - n * 4.5f + i * 9, 23, 9, ch, 8, ui.page == p ? k.c.text : k.c.textDim, 1, false, true);
    }
  }
  iskin::bevel(k, 30, 396, 660, 225, iskin::rgb(0x282B2E), k.c.panelLine, 0);
  if (ui.page == 0) {
    ImGui::SetCursorScreenPos(k.at(36, 401));
    ImGui::BeginGroup();
    msSampleTab(a, t, m, ed, k.px(648));
    ImGui::EndGroup();
  } else if (ui.page == 1) {
    dwProgramPage(a, t, k, 30, 396);
  } else {
    dwZonePage(a, t, k, 30, 396, m, ed, ui);
  }
  // ---- output column: master volume + velocity.
  iskin::bevel(k, 691, 396, 38, 225, iskin::rgb(0x25272C), k.c.panelLine, 2);
  iskin::text(k, 691, 398, 38, 12, "OUT", 8, k.c.label, 1, false, true);
  iskin::slider(a, t, k, MsVolume, 697, 412, 26, 150, k.c.fill, false);
  iskin::wheel(a, t, k, MsVelAmp, 697, 572, 26, k.c.fill);
  iskin::text(k, 691, 600, 38, 12, "VEL", 7.5f, k.c.label, 1, false, true);
  iskin::end(k, kDwH);
  ImGui::PopID();
}

// =========================================================================
// Scratch Sampler
// =========================================================================
struct ScEd {
  int move = 0, dragPt = -1, heldKey = -1, marker = -1;
  bool snap = true;
  ScratchMove clip;
  bool hasClip = false;
};
std::map<uint64_t, ScEd> gSc;

void commitScratch(App& a, const PluginTarget& t, const ScratchData& d, const char* label, uint64_t merge = 0) {
  const PluginState* cur = t.state(a.doc.song());
  if (!cur) return;
  PluginState ns = *cur;
  ns.data = serializeScratch(d);
  if (ns.data != cur->data) setPluginState(a, t, ns, label, merge);
}

void scCurveEditor(App& a, const PluginTarget& t, ScratchData& d, ScEd& ed, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ScratchMove& mv = d.moves[static_cast<size_t>(ed.move)];
  const float laneH = 16, H = size.y - laneH - 4, W = size.x;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##curve", ImVec2(W, H));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImGuiIO& io = ImGui::GetIO();
  auto px = [&](float tt) { return p.x + 8 + tt * (W - 16); };
  auto py = [&](float y) { return p.y + 8 + (1 - y) * (H - 16); };
  auto tAt = [&](float x) { return std::clamp((x - p.x - 8) / (W - 16), 0.0f, 1.0f); };
  auto yAt = [&](float y) { return std::clamp(1 - (y - p.y - 8) / (H - 16), 0.0f, 1.0f); };
  dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), P.surface950, 6);
  for (int s = 0; s <= kMuteSteps; ++s) {
    float x = px(static_cast<float>(s) / kMuteSteps);
    dl->AddLine(ImVec2(x, p.y + 4), ImVec2(x, p.y + H - 4), s % 4 == 0 ? P.line : theme::withAlpha(P.line, 0.45f));
  }
  for (int k = 1; k < 4; ++k) dl->AddLine(ImVec2(p.x + 4, py(k / 4.f)), ImVec2(p.x + W - 4, py(k / 4.f)), theme::withAlpha(P.line, 0.45f));
  // Muted steps shaded.
  for (int s = 0; s < kMuteSteps; ++s)
    if ((mv.mutes >> s) & 1u)
      dl->AddRectFilled(ImVec2(px(static_cast<float>(s) / kMuteSteps), p.y + 1), ImVec2(px(static_cast<float>(s + 1) / kMuteSteps), p.y + H - 1),
                        theme::withAlpha(P.danger, 0.10f));
  // Curve.
  ImVec2 prev;
  for (int x = 0; x <= static_cast<int>(W - 16); x += 2) {
    float tt = static_cast<float>(x) / (W - 16);
    ImVec2 q(px(tt), py(evalMove(mv, tt)));
    if (x) dl->AddLine(prev, q, P.text, 2.0f);
    prev = q;
  }
  // Points: hit test.
  int hit = -1;
  for (size_t i = 0; i < mv.pts.size(); ++i) {
    ImVec2 q(px(mv.pts[i].t), py(mv.pts[i].y));
    float dx = io.MousePos.x - q.x, dy = io.MousePos.y - q.y;
    if (dx * dx + dy * dy < 49) hit = static_cast<int>(i);
  }
  if (ImGui::IsItemActivated()) {
    if (hit >= 0) {
      ed.dragPt = hit;
    } else {
      float tt = tAt(io.MousePos.x);
      if (ed.snap) tt = std::round(tt * kMuteSteps) / kMuteSteps;
      size_t at = 0;
      while (at < mv.pts.size() && mv.pts[at].t <= tt) ++at;
      if (at > 0 && at < mv.pts.size()) {
        ScratchPoint np{tt, yAt(io.MousePos.y), mv.pts[at - 1].curve};
        mv.pts.insert(mv.pts.begin() + static_cast<long>(at), np);
        ed.dragPt = static_cast<int>(at);
        commitScratch(a, t, d, "add move point", mergeKey(t, 3000));
      }
    }
  }
  if (act && ed.dragPt >= 0 && ed.dragPt < static_cast<int>(mv.pts.size())) {
    ScratchPoint& pt = mv.pts[static_cast<size_t>(ed.dragPt)];
    ScratchPoint o = pt;
    pt.y = yAt(io.MousePos.y);
    size_t i = static_cast<size_t>(ed.dragPt);
    if (i > 0 && i + 1 < mv.pts.size()) {
      float tt = tAt(io.MousePos.x);
      if (ed.snap) tt = std::round(tt * kMuteSteps) / kMuteSteps;
      pt.t = std::clamp(tt, mv.pts[i - 1].t + 0.005f, mv.pts[i + 1].t - 0.005f);
    }
    if (o.t != pt.t || o.y != pt.y) commitScratch(a, t, d, "move point", mergeKey(t, 3001));
  }
  if (ImGui::IsItemDeactivated()) ed.dragPt = -1;
  for (size_t i = 0; i < mv.pts.size(); ++i) {
    ImVec2 q(px(mv.pts[i].t), py(mv.pts[i].y));
    bool h = static_cast<int>(i) == hit || static_cast<int>(i) == ed.dragPt;
    dl->AddCircleFilled(q, h ? 6.0f : 4.5f, h ? P.accent : P.paper);
  }
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hit > 0 && hit + 1 < static_cast<int>(mv.pts.size())) {
    mv.pts.erase(mv.pts.begin() + hit);
    ed.dragPt = -1;
    commitScratch(a, t, d, "delete move point");
  }
  static int menuPt = -1;
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit >= 0) {
    menuPt = hit;
    ImGui::OpenPopup("##ptmenu");
  }
  if (ImGui::BeginPopup("##ptmenu")) {
    if (menuPt >= 0 && menuPt < static_cast<int>(mv.pts.size())) {
      static const char* names[] = {"Linear", "Smooth", "Ease in", "Ease out", "Hold"};
      ImGui::TextDisabled("%s", i18n::tr("Segment shape"));
      for (int c = 0; c <= CurveHold; ++c)
        if (ImGui::MenuItem(names[c], nullptr, mv.pts[static_cast<size_t>(menuPt)].curve == c)) {
          mv.pts[static_cast<size_t>(menuPt)].curve = c;
          commitScratch(a, t, d, "point shape");
        }
      if (menuPt > 0 && menuPt + 1 < static_cast<int>(mv.pts.size()) && ImGui::MenuItem(i18n::tr("Delete point"))) {
        mv.pts.erase(mv.pts.begin() + menuPt);
        commitScratch(a, t, d, "delete move point");
      }
    }
    ImGui::EndPopup();
  }
  if (hov)
    a.setHint("Move curve: time → , position ↑ (between Start and End)   click: add point · drag · double-click: delete · right-click: segment shape");
  ImGui::PushFont(a.fontMono, 10.0f);
  dl->AddText(ImVec2(p.x + 10, p.y + 6), P.textFaint, factoryMoveName(ed.move));
  ImGui::PopFont();

  // Mute lane.
  ImVec2 lp(p.x, p.y + H + 4);
  ImGui::SetCursorScreenPos(lp);
  ImGui::InvisibleButton("##mutes", ImVec2(W, laneH));
  if (ImGui::IsItemClicked()) {
    int s = std::clamp(static_cast<int>(tAt(io.MousePos.x) * kMuteSteps), 0, kMuteSteps - 1);
    mv.mutes ^= 1u << s;
    commitScratch(a, t, d, "toggle mute step");
  }
  if (ImGui::IsItemHovered()) a.setHint("Mute lane: click a step to cut the sound there (crossfader cuts)");
  for (int s = 0; s < kMuteSteps; ++s) {
    bool mu = (mv.mutes >> s) & 1u;
    float x0 = px(static_cast<float>(s) / kMuteSteps) + 1, x1 = px(static_cast<float>(s + 1) / kMuteSteps) - 1;
    dl->AddRectFilled(ImVec2(x0, lp.y), ImVec2(x1, lp.y + laneH), mu ? P.danger : P.surface800, 2);
  }
}

// Reference family: the turntable scratcher's panel (slate panel, light
// wheels, orange platter accent); the move editor itself has no reference
// counterpart, so its layout stays and takes the palette.
const skin::Skin& scratchSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x2A3033, 0x5F696D, 0xFFA830, 0x667279, 0xB9BFC3);
    s.select = skin::hex(0xF8895A);
    s.lcdBg = skin::hex(0x232829);
    s.graphBg = skin::hex(0x232829);
    s.handle = skin::hex(0x6A7579);
    return s;
  }();
  return k;
}

void editorScratch(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  skin::Backdrop backdrop(scratchSkin(), width, t.key() ^ 0x5C2A7Cull, 4);
  const PluginState st = *sp;
  const auto& P = pal();
  ScEd& ed = gSc[t.key()];
  ScratchData d = parseScratch(st.data);
  SamplePtr smp = d.path.empty() ? scratchDefaultSample() : resolveSample(d.path);
  ImGui::PushID("sc");
  const float leftW = 300, vertW = 64, gap = 8, curveW = width - leftW - vertW - gap * 2, topH = 250;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // ---- left column: sample, region, move keyboard
  ImGui::BeginGroup();
  PluginTarget tt = t;
  if (ui::TextButton("Load sample...", false, 0, "Sample to scratch")) {
    openDialog(a, "Load sample", kAudioExts, [&a, tt](const std::string& p) {
      const PluginState* cur = tt.state(a.doc.song());
      if (!cur) return;
      ScratchData dd = parseScratch(cur->data);
      dd.path = p;
      if (!resolveSample(p)) a.notify("Cannot decode " + p, true);
      commitScratch(a, tt, dd, "load sample");
    });
  }
  ImGui::SameLine(0, 6);
  ImGui::PushFont(a.fontMono, 10.5f);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
  std::string nm = d.path.empty() ? "built-in phrase" : fs::path(d.path).filename().string();
  ImGui::TextColored(theme::vec(smp ? P.textDim : P.danger), "%s", nm.c_str());
  ImGui::PopFont();
  ImVec2 wp = ImGui::GetCursorScreenPos();
  const float wH = 86;
  ImGui::InvisibleButton("##scwave", ImVec2(leftW, wH));
  bool whov = ImGui::IsItemHovered(), wact = ImGui::IsItemActive();
  dl->AddRectFilled(wp, ImVec2(wp.x + leftW, wp.y + wH), P.surface950, 6);
  float s0 = val(st, ScStart), s1 = val(st, ScEnd, 0.5f);
  if (smp) {
    float xa = wp.x + std::min(s0, s1) * leftW, xb = wp.x + std::max(s0, s1) * leftW;
    dl->AddRectFilled(ImVec2(xa, wp.y), ImVec2(xb, wp.y + wH), theme::withAlpha(P.accent, 0.16f));
    drawWave(dl, smp.get(), 0, static_cast<int64_t>(smp->frames()), wp, ImVec2(leftW, wH), P.text);
    dl->AddLine(ImVec2(wp.x + s0 * leftW, wp.y), ImVec2(wp.x + s0 * leftW, wp.y + wH), P.paper, 1.5f);
    dl->AddLine(ImVec2(wp.x + s1 * leftW, wp.y), ImVec2(wp.x + s1 * leftW, wp.y + wH), P.accent, 1.5f);
    ImGui::PushFont(a.fontMono, 9.5f);
    dl->AddText(ImVec2(wp.x + s0 * leftW + 3, wp.y + 2), P.paper, "START");
    dl->AddText(ImVec2(wp.x + s1 * leftW + 3, wp.y + wH - 13), P.accent, "END");
    ImGui::PopFont();
  }
  ImGuiIO& io = ImGui::GetIO();
  if (whov && !wact) {
    a.setHint("Drag the START / END lines: the part of the sample the moves travel through (drop a sample here)");
    if (std::fabs(io.MousePos.x - (wp.x + s0 * leftW)) < 6 || std::fabs(io.MousePos.x - (wp.x + s1 * leftW)) < 6)
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }
  if (ImGui::IsItemActivated()) {
    float d0 = std::fabs(io.MousePos.x - (wp.x + s0 * leftW)), d1 = std::fabs(io.MousePos.x - (wp.x + s1 * leftW));
    ed.marker = d0 <= d1 ? 0 : 1;
  }
  if (wact && ed.marker >= 0) {
    double f = std::clamp((io.MousePos.x - wp.x) / leftW, 0.0f, 1.0f);
    a.doc.setParam(t.pid(ed.marker == 0 ? ScStart : ScEnd), f, true);
  }
  if (ImGui::IsItemDeactivated()) ed.marker = -1;
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
      std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (isAudioFile(path)) {
        d.path = path;
        commitScratch(a, t, d, "load sample");
      }
    }
    ImGui::EndDragDropTarget();
  }
  ImGui::Dummy(ImVec2(0, 2));
  float c = leftW / 4;
  pui::knobCell(a, t, ScStart, "Start", c, 14);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, ScEnd, "End", c, 14);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, ScLength, val(st, ScSync, 1) >= 0.5f ? "Beats" : "Seconds", c, 14);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, ScSpeed, "Speed", c, 14);
  // Move keyboard (one octave: one move per key).
  smallCaps(a, "MOVES  (one per key, C to B)");
  ImVec2 kp = ImGui::GetCursorScreenPos();
  const float kW = leftW / 12, kH = 34;
  ImGui::InvisibleButton("##movekeys", ImVec2(leftW, kH));
  bool khov = ImGui::IsItemHovered();
  int hk = khov ? std::clamp(static_cast<int>((io.MousePos.x - kp.x) / kW), 0, 11) : 0;  // no mouse: -FLT_MAX
  for (int k = 0; k < 12; ++k) {
    float x0 = kp.x + k * kW;
    bool black = isBlack(k), sel = ed.move == k;
    ImU32 col = sel ? P.accent : black ? P.surface700 : theme::mix(P.text, P.surface900, 0.15f);
    dl->AddRectFilled(ImVec2(x0, kp.y), ImVec2(x0 + kW - 1, kp.y + kH), col, 2);
    const ScratchMove& mv = d.moves[static_cast<size_t>(k)];
    // Tiny curve preview.
    ImVec2 pr;
    for (int s = 0; s <= 8; ++s) {
      float tt = s / 8.f;
      ImVec2 q(x0 + 3 + tt * (kW - 7), kp.y + kH - 6 - evalMove(mv, tt) * (kH - 12));
      if (s) dl->AddLine(pr, q, sel ? P.paper : (black ? P.textDim : P.surface800), 1.2f);
      pr = q;
    }
  }
  if (ImGui::IsItemActivated()) {
    ed.move = hk;
    int root = static_cast<int>(val(st, ScRoot, 60) + 0.5f);
    int key = (root / 12) * 12 + hk;
    ed.heldKey = key;
    preview(a, t, key, 0.9f, true);
  }
  if (ImGui::IsItemDeactivated() && ed.heldKey >= 0) {
    preview(a, t, ed.heldKey, 0, false);
    ed.heldKey = -1;
  }
  if (khov) a.setHint(std::string("Move ") + noteName((static_cast<int>(val(st, ScRoot, 60) + 0.5f) / 12) * 12 + hk) + ": " + factoryMoveName(hk) +
                      "   click: select and play");
  ImGui::EndGroup();
  ImGui::SameLine(0, gap);
  // ---- curve editor + mute lane
  ImGui::BeginGroup();
  scCurveEditor(a, t, d, ed, ImVec2(curveW, topH));
  ImGui::EndGroup();
  ImGui::SameLine(0, gap);
  // ---- vertical waveform of the region (position axis matches the curve's)
  ImGui::BeginGroup();
  ImVec2 vp = ImGui::GetCursorScreenPos();
  const float vH = topH - 20;
  ImGui::Dummy(ImVec2(vertW, vH));
  dl->AddRectFilled(vp, ImVec2(vp.x + vertW, vp.y + vH), P.surface950, 6);
  if (smp && smp->frames() > 1) {
    const int64_t n = static_cast<int64_t>(smp->frames());
    int64_t a0 = static_cast<int64_t>(s0 * static_cast<float>(n - 1)), a1 = static_cast<int64_t>(s1 * static_cast<float>(n - 1));
    int rows = static_cast<int>(vH - 16);
    for (int r = 0; r < rows; ++r) {
      // row 0 = top = position 1 (End)
      double y0 = 1.0 - static_cast<double>(r) / rows, y1 = 1.0 - static_cast<double>(r + 1) / rows;
      int64_t f0 = a0 + static_cast<int64_t>((a1 - a0) * y1), f1 = a0 + static_cast<int64_t>((a1 - a0) * y0);
      if (f0 > f1) std::swap(f0, f1);
      f1 = std::max(f1, f0 + 1);
      float lo = 0, hi = 0;
      int64_t step = std::max<int64_t>(1, (f1 - f0) / 32);
      for (int64_t i = std::max<int64_t>(0, f0); i < f1 && i < n; i += step) {
        float v = smp->left[static_cast<size_t>(i)];
        lo = std::min(lo, v), hi = std::max(hi, v);
      }
      float cx = vp.x + vertW * 0.5f, yy = vp.y + 8 + static_cast<float>(r);
      dl->AddLine(ImVec2(cx + lo * vertW * 0.45f, yy), ImVec2(cx + hi * vertW * 0.45f + 1, yy), P.textDim);
    }
  }
  ImGui::PushFont(a.fontMono, 9.0f);
  dl->AddText(ImVec2(vp.x + 4, vp.y + 2), P.accent, "END");
  dl->AddText(ImVec2(vp.x + 4, vp.y + vH - 13), P.paper, "START");
  ImGui::PopFont();
  if (ui::TextButton("Snap", ed.snap, vertW, "Snap points to the 16-step grid")) ed.snap = !ed.snap;
  ImGui::EndGroup();

  // ---- bottom panels
  ImGui::Dummy(ImVec2(0, 4));
  const float w1 = 360, w2 = 300, w3 = width - w1 - w2 - gap * 2;
  pui::beginSection("PLAYBACK", w1, 128);
  pui::choiceButtons(a, t, ScKeyMode, 200);
  ImGui::SameLine(0, 8);
  pui::choiceButtons(a, t, ScMotion, w1 - 228);
  float cc = (w1 - 20) / 4;
  pui::knobCell(a, t, ScRoot, "Root", cc, 13);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, ScPitchFollow, "Pitch follow", cc, 13);
  ImGui::SameLine(0, 0);
  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(0, 6));
  pui::toggleButton(a, t, ScSync, "Tempo sync");
  pui::toggleButton(a, t, ScLoopMove, "Loop move");
  ImGui::EndGroup();
  pui::endSection();
  ImGui::SameLine(0, gap);
  pui::beginSection("SHAPE", w2, 128);
  cc = (w2 - 20) / 4;
  pui::knobCell(a, t, ScSmoothing, "Smoothing", cc, 14);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, ScAttack, "Attack", cc, 14);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, ScRelease, "Release", cc, 14);
  ImGui::SameLine(0, 0);
  pui::knobCell(a, t, ScVolume, "Volume", cc, 14);
  pui::choiceButtons(a, t, ScInterp, w2 - 20);
  pui::endSection();
  ImGui::SameLine(0, gap);
  pui::beginSection("MOVE", w3, 128);
  {
    ScratchMove& mv = d.moves[static_cast<size_t>(ed.move)];
    ImGui::PushFont(a.fontBold, 14.0f);
    ImGui::Text("%s  ·  %s", noteName((static_cast<int>(val(st, ScRoot, 60) + 0.5f) / 12) * 12 + ed.move).c_str(), factoryMoveName(ed.move));
    ImGui::PopFont();
    ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("%zu points · %d muted steps"), mv.pts.size(), static_cast<int>(std::bitset<32>(mv.mutes).count()));
    if (ui::TextButton("Reset", false, 0, "Restore this key's factory move")) {
      mv = factoryMove(ed.move);
      commitScratch(a, t, d, "reset move");
    }
    ImGui::SameLine(0, 4);
    if (ui::TextButton("Copy", false, 0, "Copy this move")) ed.clip = mv, ed.hasClip = true;
    ImGui::SameLine(0, 4);
    if (ui::TextButton("Paste", false, 0, "Paste the copied move onto this key") && ed.hasClip) {
      mv = ed.clip;
      commitScratch(a, t, d, "paste move");
    }
    if (ui::TextButton("Mirror", false, 0, "Flip the move upside down")) {
      for (auto& p : mv.pts) p.y = 1 - p.y;
      commitScratch(a, t, d, "mirror move");
    }
    ImGui::SameLine(0, 4);
    if (ui::TextButton("Clear mutes", false, 0, "Unmute all steps")) {
      mv.mutes = 0;
      commitScratch(a, t, d, "clear mutes");
    }
  }
  pui::endSection();
  ImGui::PopID();
}

}  // namespace

void drawWaveProgramEditor(App& a, const PluginTarget& t, float width);  // dwave_ui.cpp (J-33)

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (st->type == "gl.waveprogram") {
    drawWaveProgramEditor(a, t, width);
    return true;
  }
  if (st->type == "gl.multisampler") {
    editorDirectWave(a, t, width);
    return true;
  }
  if (st->type == "gl.scratch") {
    editorScratch(a, t, width);
    return true;
  }
  return false;
}

}  // namespace gl::packs::samplers
