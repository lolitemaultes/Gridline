// Gridline — "modulation" pack: Envelope Controller editor for the
// articulator model (J-41). Output scopes and articulator selector on the
// left, the articulator's amounts / polarity / key range on top, part tabs
// (envelope, LFO and the five maps) over a point editor, and the part's
// settings underneath. Point lists live in PluginState::data (envc.h); every
// edit goes through EditTarget::set so undo works.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include "../../core/AutomationEdit.h"
#include "../../core/Automation.h"
#include "../../core/FlCurves.h"
#include "envc.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "modulation_editors.h"
#include "modulation_skin.h"

namespace gl::packs::modulation {

namespace {

struct UiState {
  int art = 0, part = 0;
  int drag = -1;           // point being dragged
  int tensionDrag = -1;    // segment whose tension is being dragged
  float dragY0 = 0, tension0 = 0;
  int menuPoint = -1;
};
UiState& ui(uint64_t key) {
  static std::map<uint64_t, UiState> m;
  return m[key];
}

const char* const kParts[7] = {"ENVELOPE", "LFO", "KEY", "VELOCITY", "MOD X", "MOD Y", "RANDOM"};
const char* const kInputs[5] = {"note pitch", "note velocity", "mod X", "mod Y", "random per note"};

flart::Record& partRecord(EnvcArt& a, int part) {
  if (part == 0) return a.env;
  if (part == 1) return a.lfo;
  return a.maps[static_cast<size_t>(std::clamp(part - 2, 0, kEnvcMaps - 1))];
}

void commit(EditTarget& e, const EnvcModel& m, const char* what, uint64_t merge) {
  if (!e.st) return;
  PluginState st = *e.st;
  st.data = writeEnvc(m);
  e.set(st, what, merge);
}

// Gridline name of an FL shape number.
const char* shapeName(int fl) { return autoShapeName(autoedit::shapeFromFlMode(fl)); }

double segShape(const flart::Record& r, size_t k, double x) {
  const flart::Point& p = r.pts[k];
  return flcurve::shape(p.shape, p.tension, x);
}

// Point editor of one record. Maps run over 0..1; envelopes over their
// total time (pre-delay included).
void pointEditor(EditTarget& e, EnvcModel& m, UiState& u, ImVec2 p0, ImVec2 size, const skin::Skin& k, double centre) {
  EnvcArt& art = m.arts[static_cast<size_t>(u.art)];
  flart::Record& r = partRecord(art, u.part);
  const bool isMap = u.part >= 2;
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##pts", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::graphWell(dl, p0, ImVec2(p0.x + size.x, p0.y + size.y), k);
  const float pad = 10;
  const float x0 = p0.x + pad, y0 = p0.y + pad, gw = size.x - 2 * pad, gh = size.y - 2 * pad;
  const size_t n = r.pts.size();
  std::vector<double> at(n);
  double total = 0;
  for (size_t i = 0; i < n; ++i) {
    total += std::isfinite(r.pts[i].time) ? std::max(0.0, r.pts[i].time) : 0.0;
    at[i] = total;
  }
  const double view = isMap ? 1.0 : std::max(1.0, total * 1.15);
  auto X = [&](double t) { return x0 + static_cast<float>(t / view) * gw; };
  auto Y = [&](double v) { return y0 + gh - static_cast<float>(std::clamp(v, 0.0, 1.0)) * gh; };
  // Grid: beats / half seconds (1.0 time units) and quarter levels.
  const double step = isMap ? 0.125 : (view > 8 ? 1.0 : 0.25);
  for (double s = 0; s <= view + 1e-9; s += step)
    dl->AddLine(ImVec2(X(s), y0), ImVec2(X(s), y0 + gh), std::fmod(s + 1e-9, isMap ? 0.5 : 1.0) < 2e-9 ? k.graphGridStrong : k.graphGrid);
  for (int g = 0; g <= 4; ++g) dl->AddLine(ImVec2(x0, y0 + gh * g / 4), ImVec2(x0 + gw, y0 + gh * g / 4), g == 2 ? k.graphGridStrong : k.graphGrid);
  if (centre > 0) dl->AddLine(ImVec2(x0, Y(centre)), ImVec2(x0 + gw, Y(centre)), theme::withAlpha(k.graphText, 0.5f));
  // Curve.
  std::vector<ImVec2> line;
  for (size_t i = 0; i < n; ++i) {
    const double v1 = std::isfinite(r.pts[i].value) ? r.pts[i].value : (i ? r.pts[i - 1].value : 0.0);
    if (i == 0) {
      line.push_back(ImVec2(X(0), Y(v1)));
      line.push_back(ImVec2(X(at[0]), Y(v1)));
      continue;
    }
    const double v0 = r.pts[i - 1].value, t0 = at[i - 1], t1 = at[i];
    const int steps = std::clamp(static_cast<int>((X(t1) - X(t0)) / 3.0f), 1, 96);
    for (int s = 1; s <= steps; ++s) {
      const double x = static_cast<double>(s) / steps;
      line.push_back(ImVec2(X(t0 + (t1 - t0) * x), Y(v0 + (v1 - v0) * segShape(r, i, x))));
    }
  }
  if (line.size() >= 2) dl->AddPolyline(line.data(), static_cast<int>(line.size()), r.on() || isMap ? k.graphCurve : k.graphText, 0, 2.0f);
  // Markers (envelopes / LFO amplitude): decay, loop start, sustain.
  if (!isMap) {
    ImGui::PushFont(e.a->fontMono, 10.0f);
    auto marker = [&](int idx, const char* label, ImU32 col) {
      if (idx < 0 || static_cast<size_t>(idx) >= n) return;
      const float sx = X(at[static_cast<size_t>(idx)]);
      for (float y = y0; y < y0 + gh; y += 8) dl->AddLine(ImVec2(sx, y), ImVec2(sx, std::min(y + 4, y0 + gh)), col);
      dl->AddText(ImVec2(sx + 3, y0 + (label[0] == 'D' ? 12.f : 0.f)), col, label);
    };
    marker(r.decay, "DECAY", k.graphText);
    marker(r.loop, "LOOP", theme::withAlpha(k.accent2C(), 0.9f));
    marker(r.sustain, "SUSTAIN", k.graphText);
    ImGui::PopFont();
  }
  // Points and tension handles.
  ImGuiIO& io = ImGui::GetIO();
  int hover = -1, hoverTen = -1;
  for (size_t i = 0; i < n; ++i) {
    ImVec2 c(X(at[i]), Y(r.pts[i].value));
    if (std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y) < 8) hover = static_cast<int>(i);
  }
  for (size_t i = 1; i < n && hover < 0; ++i) {
    const double xm = 0.5;
    ImVec2 c(X(at[i - 1] + (at[i] - at[i - 1]) * xm), Y(r.pts[i - 1].value + (r.pts[i].value - r.pts[i - 1].value) * segShape(r, i, xm)));
    if (std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y) < 6) hoverTen = static_cast<int>(i);
  }
  for (size_t i = 0; i < n; ++i) {
    ImVec2 c(X(at[i]), Y(r.pts[i].value));
    const bool hot = static_cast<int>(i) == hover || static_cast<int>(i) == u.drag;
    dl->AddCircleFilled(c, hot ? 6.0f : 4.5f, hot ? skin::lighten(k.fill, 0.4f) : k.graphBg);
    dl->AddCircle(c, hot ? 6.0f : 4.5f, k.graphCurve, 16, 1.6f);
    if (i > 0) {
      ImVec2 h(X(at[i - 1] + (at[i] - at[i - 1]) * 0.5), Y(r.pts[i - 1].value + (r.pts[i].value - r.pts[i - 1].value) * segShape(r, i, 0.5)));
      const bool th = static_cast<int>(i) == hoverTen || static_cast<int>(i) == u.tensionDrag;
      dl->AddCircle(h, th ? 4.5f : 3.0f, theme::withAlpha(k.graphCurve, 0.8f), 12, 1.2f);
    }
  }
  // Output level of this articulator.
  const float lv = e.output(u.art);
  dl->AddLine(ImVec2(x0, Y(lv)), ImVec2(x0 + gw, Y(lv)), theme::withAlpha(k.accent2C(), 0.6f), 1.0f);
  const uint64_t mergeKey = e.key ^ 0x4152544Eull ^ (static_cast<uint64_t>(u.art) << 8) ^ static_cast<uint64_t>(u.part);
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    u.drag = hover;
    u.tensionDrag = hover < 0 ? hoverTen : -1;
    u.dragY0 = io.MousePos.y;
    if (u.tensionDrag > 0) u.tension0 = r.pts[static_cast<size_t>(u.tensionDrag)].tension;
  }
  if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (u.drag >= 0 && static_cast<size_t>(u.drag) < n) {
      flart::Point& p = r.pts[static_cast<size_t>(u.drag)];
      const double prev = u.drag > 0 ? at[static_cast<size_t>(u.drag - 1)] : 0.0;
      double t = (io.MousePos.x - x0) / gw * view - prev;
      if (isMap) {  // map positions stay inside 0..1 and in order
        const double next = static_cast<size_t>(u.drag + 1) < n ? at[static_cast<size_t>(u.drag + 1)] : 1.0;
        t = std::clamp(t, 0.0, std::max(0.0, next - prev));
        if (u.drag == 0) t = 0;
        if (static_cast<size_t>(u.drag) == n - 1) t = std::max(0.0, 1.0 - prev);
      }
      const double oldT = p.time;
      p.time = std::max(0.0, t);
      if (isMap && static_cast<size_t>(u.drag + 1) < n) r.pts[static_cast<size_t>(u.drag + 1)].time += oldT - p.time;  // keep later points in place
      p.value = std::clamp(static_cast<double>((y0 + gh - io.MousePos.y) / gh), 0.0, 1.0);
      commit(e, m, "edit articulator point", mergeKey ^ static_cast<uint64_t>(u.drag));
    } else if (u.tensionDrag > 0 && static_cast<size_t>(u.tensionDrag) < n) {
      flart::Point& p = r.pts[static_cast<size_t>(u.tensionDrag)];
      const bool rising = p.value >= r.pts[static_cast<size_t>(u.tensionDrag - 1)].value;
      const float d = (u.dragY0 - io.MousePos.y) / gh * 2.0f * (rising ? 1.0f : -1.0f);
      p.tension = std::clamp(u.tension0 + d, -1.0f, 1.0f);
      commit(e, m, "edit articulator tension", mergeKey ^ 0x544Eull ^ static_cast<uint64_t>(u.tensionDrag));
    }
  }
  if (ImGui::IsItemDeactivated()) {
    u.drag = u.tensionDrag = -1;
    e.a->doc.endGesture();
  }
  // Double-click: add a point (empty area) / remove it (on a point).
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    if (hover > 0 && n > 2) {
      const size_t h = static_cast<size_t>(hover);
      if (isMap && h + 1 < n) r.pts[h + 1].time += r.pts[h].time;
      r.pts.erase(r.pts.begin() + static_cast<std::ptrdiff_t>(h));
      auto fix = [&](int32_t& mk) {
        if (mk == hover) mk = -1;
        else if (mk > hover) --mk;
      };
      fix(r.decay), fix(r.loop), fix(r.sustain);
      commit(e, m, "delete articulator point", 0);
    } else if (hover < 0) {
      const double t = std::clamp((io.MousePos.x - x0) / gw * view, 0.0, view);
      size_t i = 0;
      while (i < n && at[i] < t) ++i;
      flart::Point p;
      p.value = std::clamp(static_cast<double>((y0 + gh - io.MousePos.y) / gh), 0.0, 1.0);
      const double prev = i > 0 ? at[i - 1] : 0.0;
      p.time = t - prev;
      if (i < n) r.pts[i].time = std::max(0.0, at[i] - t);
      r.pts.insert(r.pts.begin() + static_cast<std::ptrdiff_t>(i), p);
      auto fix = [&](int32_t& mk) {
        if (mk >= static_cast<int>(i)) ++mk;
      };
      fix(r.decay), fix(r.loop), fix(r.sustain);
      commit(e, m, "add articulator point", 0);
    }
  }
  // Right-click on a point: shape and marker menu.
  if (hovered && hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    u.menuPoint = hover;
    ImGui::OpenPopup("##artpt");
  }
  if (ImGui::BeginPopup("##artpt")) {
    const int pi = u.menuPoint;
    if (pi >= 0 && static_cast<size_t>(pi) < r.pts.size()) {
      ImGui::TextDisabled("Segment into point %d", pi + 1);
      for (int s = 0; s < flcurve::kShapeCount; ++s)
        if (ImGui::MenuItem(shapeName(s), nullptr, r.pts[static_cast<size_t>(pi)].shape == s)) {
          r.pts[static_cast<size_t>(pi)].shape = static_cast<uint8_t>(s);
          commit(e, m, "articulator point shape", 0);
        }
      if (!isMap) {
        ImGui::Separator();
        if (ImGui::MenuItem("Sustain here", nullptr, r.sustain == pi)) r.sustain = r.sustain == pi ? -1 : pi, commit(e, m, "articulator sustain", 0);
        if (ImGui::MenuItem("Loop start here", nullptr, r.loop == pi)) r.loop = r.loop == pi ? -1 : pi, commit(e, m, "articulator loop", 0);
        if (u.part == 0 && ImGui::MenuItem("Decay starts here", nullptr, r.decay == pi))
          r.decay = r.decay == pi ? -1 : pi, commit(e, m, "articulator decay point", 0);
      }
      if (n > 2 && pi > 0 && ImGui::MenuItem("Delete point")) {
        r.pts.erase(r.pts.begin() + pi);
        for (int32_t* mk : {&r.decay, &r.loop, &r.sustain}) *mk = *mk == pi ? -1 : (*mk > pi ? *mk - 1 : *mk);
        commit(e, m, "delete articulator point", 0);
      }
    }
    ImGui::EndPopup();
  }
  if (hovered) {
    char buf[200];
    if (hover >= 0)
      std::snprintf(buf, sizeof buf, "Point %d: %s %.3f, level %.0f%%, %s   (drag; right-click: shape and markers; double-click: delete)",
                    hover + 1, isMap ? "at" : "after", r.pts[static_cast<size_t>(hover)].time, r.pts[static_cast<size_t>(hover)].value * 100.0,
                    shapeName(r.pts[static_cast<size_t>(hover)].shape));
    else if (hoverTen > 0)
      std::snprintf(buf, sizeof buf, "Tension %.2f — drag up / down to bend the segment", r.pts[static_cast<size_t>(hoverTen)].tension);
    else
      std::snprintf(buf, sizeof buf, isMap ? "Map over %s (0..1 across)   (double-click: add a point)"
                                           : "%s: time in %s   (double-click: add a point)",
                    isMap ? kInputs[u.part - 2] : kParts[u.part], (r.flags & flart::kTempoTime) ? "beats" : "units of 0.5 s");
    e.a->setHint(buf);
  }
}

// LED check over a bit of a record field.
bool flagCheck(EditTarget& e, EnvcModel& m, int32_t& field, int32_t bit, bool onIsNonZero, ImVec2 tl, ImVec2 sz, const skin::Skin& k,
               const char* text) {
  const bool on = onIsNonZero ? field != 0 : (field & bit) != 0;
  if (!sk::button(text, tl, sz, k, text, on)) return false;
  if (onIsNonZero) field = on ? 0 : -1;
  else field = on ? (field & ~bit) : (field | bit);
  commit(e, m, text, 0);
  return true;
}

}  // namespace

// Reference panel 737 × 420.
void envArtEditor(EditTarget& e) {
  const skin::Skin& K = sk::controller();
  const skin::Skin& L = sk::controllerLfo();
  if (!e.st) return;
  EnvcModel m = envcOf(e.st->data);
  UiState& u = ui(e.key);
  u.art = std::clamp(u.art, 0, kArts - 1);
  u.part = std::clamp(u.part, 0, 6);
  for (int a = 0; a < kArts; ++a) w::pushHistory(e.key, a, e.output(a));
  constexpr float kW = 737, kH = 420;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  // Output scopes A..H.
  dl->AddRectFilled(F.p(0, 0), F.p(126, 104), K.panelLo, 4 * F.s, ImDrawFlags_RoundCornersTopLeft);
  for (int a = 0; a < kArts; ++a) {
    ImVec2 a0 = F.p(4, 3 + a * 12.6f), a1 = F.p(122, 14 + a * 12.6f);
    ImGui::SetCursorScreenPos(a0);
    ImGui::PushID(a);
    if (ImGui::InvisibleButton("##scope", ImVec2(a1.x - a0.x, a1.y - a0.y))) u.art = a;
    ImGui::PopID();
    skin::graphWell(dl, a0, a1, K);
    w::drawHistory(dl, ImVec2(a0.x + 12 * F.s, a0.y + 1), ImVec2(a1.x - a0.x - 14 * F.s, a1.y - a0.y - 2), e.key, a,
                   a == u.art ? K.fill : K.graphText, 1.2f);
    char nm[2] = {static_cast<char>('A' + a), 0};
    sk::text(dl, F, 7, 4.5f + a * 12.6f, K, nm, 7.0f, false, a == u.art ? K.fill : K.graphText);
  }
  // Articulator selector (2 × 4).
  dl->AddRectFilled(F.p(130, 4), F.p(176, 100), K.panelLo, 3);
  for (int a = 0; a < kArts; ++a) {
    const float cx = 132 + (a / 4) * 22.f, cy = 6 + (a % 4) * 23.5f;
    char nm[2] = {static_cast<char>('A' + a), 0};
    ImGui::PushID(a + 20);
    if (sk::button("##sel", F.p(cx, cy), F.sz(20, 21), K, nm, a == u.art)) u.art = a;
    ImGui::PopID();
  }
  EnvcArt& art = m.arts[static_cast<size_t>(u.art)];
  ImGui::PushID(u.art);
  // Amounts and polarity.
  sk::wheelL(e, artParam(u.art, AfBase), F, 184, 6, 30, K, "BASE");
  sk::wheelL(e, artParam(u.art, AfEnvAmt), F, 220, 6, 30, K, "ENV", skin::WheelBigLine | skin::WheelCentered);
  sk::wheelL(e, artParam(u.art, AfLfoAmt), F, 256, 6, 30, K, "LFO", skin::WheelBigLine | skin::WheelCentered);
  {
    const bool bi = art.bipolar();
    if (sk::button("##pol", F.p(182, 52), F.sz(140, 20), K, bi ? "SUM (BIPOLAR)" : "PRODUCT (UNIPOLAR)", true)) {
      art.flags ^= 1;
      // Maps keep their meaning: neutral is 0.5 (sum) / 1.0 (product).
      commit(e, m, "articulator polarity", 0);
    }
    if (ImGui::IsItemHovered())
      e.a->setHint("Sum: envelope, LFO and maps add around the base (centre 50%). Product: they multiply (neutral 100%)");
  }
  sk::check(e, EcContinuous, F.p(182, 78), F.sz(145, 18), K, "CONTINUOUS");
  // Key range.
  sk::panelBox(dl, F, 331, 7, 316, 91, K);
  {
    const int lo = std::clamp(art.keyLo, 0, 127), hi = std::clamp(art.keyHi, 0, 127);
    ImVec2 kp = F.p(337, 12), ks = F.sz(304, 52);
    const int key = skin::keyboard("##kb", kp, ks, 36, 95, K, nullptr, lo, hi, -1);
    ImGuiIO& io = ImGui::GetIO();
    if (key >= 0) {
      art.keyLo = key;
      if (art.keyHi < key) art.keyHi = key;
      commit(e, m, "articulator key range", 0);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      const float fx = std::clamp((io.MousePos.x - kp.x) / ks.x, 0.0f, 0.999f);
      static const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
      int whites = 0;
      for (int nn = 36; nn <= 95; ++nn) whites += black[nn % 12] ? 0 : 1;
      int wi = static_cast<int>(fx * whites), nn = 36;
      for (int c = 0; nn <= 95; ++nn)
        if (!black[nn % 12] && c++ == wi) break;
      art.keyHi = std::max(art.keyLo, std::min(nn, 95));
      commit(e, m, "articulator key range", 0);
    }
    if (ImGui::IsItemHovered()) e.a->setHint("Left-click: lowest key, right-click: highest key that triggers this articulator");
    char buf[48];
    if (art.keyLo <= 0 && art.keyHi >= 127) std::snprintf(buf, sizeof buf, "ALL KEYS");
    else std::snprintf(buf, sizeof buf, "%s .. %s", w::noteName(art.keyLo).c_str(), w::noteName(art.keyHi).c_str());
    skin::lcd(dl, F.p(337, 70), F.p(480, 92), K, buf, F.font(9.0f));
    if (sk::button("##allkeys", F.p(486, 70), F.sz(60, 22), K, "ALL")) {
      art.keyLo = 0, art.keyHi = 256;
      commit(e, m, "articulator key range", 0);
    }
  }
  // Global modulation X / Y.
  sk::wheelL(e, EcModX, F, 656, 10, 30, K, "MOD X");
  sk::wheelL(e, EcModY, F, 694, 10, 30, K, "MOD Y");
  // Part tabs.
  for (int pt = 0; pt < 7; ++pt) {
    const flart::Record& r = partRecord(art, pt);
    bool used;
    if (pt < 2) used = r.on();
    else {
      used = false;
      const double neutral = art.bipolar() ? 0.5 : 1.0;
      for (const auto& p : r.pts) used = used || p.value != neutral;
    }
    char label[24];
    std::snprintf(label, sizeof label, "%s%s", kParts[pt], used ? " *" : "");
    ImGui::PushID(pt + 40);
    if (sk::button("##part", F.p(4 + pt * 104.f, 106), F.sz(102, 20), pt == 1 ? L : K, label, pt == u.part)) u.part = pt;
    ImGui::PopID();
  }
  pointEditor(e, m, u, F.p(4, 130), F.sz(729, 190), u.part == 1 ? L : K, u.part == 1 ? 0.5 : (art.bipolar() ? 0.5 : 0.0));
  // Part settings.
  flart::Record& r = partRecord(art, u.part);
  if (u.part == 0) {
    sk::panelBox(dl, F, 4, 326, 729, 90, K, "ENVELOPE");
    flagCheck(e, m, r.enabled, 0, true, F.p(12, 346), F.sz(110, 22), K, "ENABLED");
    flagCheck(e, m, r.flags, flart::kTempoTime, false, F.p(12, 374), F.sz(110, 22), K, "TEMPO TIME");
    sk::wheelL(e, artParam(u.art, AfAttack), F, 160, 342, 30, K, "ATTACK");
    sk::wheelL(e, artParam(u.art, AfDecay), F, 206, 342, 30, K, "DECAY");
    sk::wheelL(e, artParam(u.art, AfSustain), F, 252, 342, 30, K, "SUSTAIN", skin::WheelBigLine | skin::WheelCentered);
    sk::wheelL(e, artParam(u.art, AfRelease), F, 298, 342, 30, K, "RELEASE");
    sk::text(dl, F, 350, 346, K, "Time scalers multiply the attack, decay / loop and release", 7.5f);
    sk::text(dl, F, 350, 360, K, "segments; sustain moves the decay target and loop levels.", 7.5f);
    sk::text(dl, F, 350, 380, K, "Right-click a point: shape, sustain, loop start, decay start.", 7.5f);
  } else if (u.part == 1) {
    sk::panelBox(dl, F, 4, 326, 729, 90, L, "LFO");
    flagCheck(e, m, r.enabled, 0, true, F.p(12, 342), F.sz(110, 20), L, "ENABLED");
    flagCheck(e, m, r.flags, flart::kTempoTime, false, F.p(12, 366), F.sz(110, 20), L, "TEMPO TIME");
    flagCheck(e, m, r.flags, flart::kShared, false, F.p(12, 390), F.sz(110, 20), L, "GLOBAL PHASE");
    flagCheck(e, m, r.flags, flart::kFrozen, false, F.p(128, 342), F.sz(110, 20), L, "FROZEN");
    flagCheck(e, m, r.flags, flart::kBipolarTen, false, F.p(128, 366), F.sz(110, 20), L, "BIPOLAR BEND");
    sk::wheelL(e, artParam(u.art, AfLfoSpeed), F, 260, 342, 30, L, "SPEED");
    sk::wheelL(e, artParam(u.art, AfLfoTension), F, 306, 342, 30, L, "BEND", skin::WheelBigLine | skin::WheelCentered);
    sk::wheelL(e, artParam(u.art, AfLfoSkew), F, 352, 342, 30, L, "SKEW", skin::WheelBigLine | skin::WheelCentered);
    sk::wheelL(e, artParam(u.art, AfLfoPw), F, 398, 342, 30, L, "WIDTH");
    const int speed = static_cast<int>(std::lround(e.p(artParam(u.art, AfLfoSpeed), 0.624f) * 65536.0));
    const double rate = flart::lfoRate(speed);
    char buf[64];
    if (r.flags & flart::kTempoTime) std::snprintf(buf, sizeof buf, "%.3f cycles / beat", rate);
    else std::snprintf(buf, sizeof buf, "%.3f Hz", rate * 2.0);
    skin::lcd(dl, F.p(450, 346), F.p(600, 368), L, buf, F.font(9.0f));
    sk::text(dl, F, 450, 378, L, "The curve above is the LFO's amplitude (50% = none).", 7.5f);
  } else {
    sk::panelBox(dl, F, 4, 326, 729, 90, K, "MAP");
    char buf[96];
    std::snprintf(buf, sizeof buf, "Input: %s, 0..1 across. Neutral level: %s.", kInputs[u.part - 2], art.bipolar() ? "50%" : "100%");
    sk::text(dl, F, 12, 346, K, buf, 8.0f);
    if (sk::button("##reset", F.p(12, 372), F.sz(110, 22), K, "RESET")) {
      r = flart::defaultMap(art.bipolar() ? 0.5 : 1.0);
      commit(e, m, "reset articulator map", 0);
    }
  }
  ImGui::PopID();
  F.end(0, kH);
}

}  // namespace gl::packs::modulation
