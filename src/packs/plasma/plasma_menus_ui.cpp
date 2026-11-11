// Gridline — "plasma" pack: the display order and names of the filter types
// and the morph (warp) modes. Gridline groups them by what they do, in its
// own words; the stored index of every entry is unchanged (projects, presets
// and automation keep their values).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include "../../core/I18n.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {
// Gridline's name of each filter type, by stored index.
const char* const kFilterName[] = {
    "Warm low pass 6", "Warm low pass 12", "Warm low pass 18", "Warm low pass 24",                      // 0..3
    "Low pass 6", "Low pass 12", "Low pass 18", "Low pass 24",                                          // 4..7
    "High pass 6", "High pass 12", "High pass 18", "High pass 24",                                      // 8..11
    "Band pass 12", "Band pass 24", "Resonant peak 12", "Resonant peak 24", "Notch 12", "Notch 24",     // 12..17
    "Low to high 6", "Low to high 12", "Low to band", "Low to peak", "Low to notch", "High to band",     // 18..23
    "High to peak", "High to notch", "Band to peak", "Band to notch", "Peak to peak", "Peak to notch",  // 24..29
    "Notch to notch", "Low, band, high 12", "Low, band, high 24", "Low, peak, high 12",                 // 30..33
    "Low, peak, high 24", "Low, notch, high 12", "Low, notch, high 24", "Band, peak, notch 12",         // 34..37
    "Band, peak, notch 24",                                                                             // 38
    "Comb plus", "Comb minus", "Comb plus, low", "Comb minus, low", "Comb plus, high", "Comb minus, high",  // 39..44
    "Comb plus, band", "Comb minus, band",                                                              // 45..46
    "Flange plus", "Flange minus", "Flange plus, low", "Flange minus, low", "Flange plus, high",        // 47..51
    "Flange minus, high", "Flange plus, band", "Flange minus, band",                                    // 52..54
    "Phase 12 plus", "Phase 12 minus", "Phase 24 plus", "Phase 24 minus", "Phase 36 plus",              // 55..59
    "Phase 36 minus", "Phase 48 plus", "Phase 48 minus", "Phase 48 plus, low", "Phase 48 minus, low",   // 60..64
    "Phase 48 plus, high", "Phase 48 minus, high", "Phase 48 plus, band", "Phase 48 minus, band",      // 65..68
    "Flange and phase plus", "Flange and phase minus",                                                  // 69..70
    "Low shelf 6", "Low shelf 12", "Bell 12", "High shelf 6", "High shelf 12",                          // 71..75
    "Ring", "Ring double", "Step hold", "Step hold 2", "Comb swarm", "Phase swarm", "Little room",      // 76..82
    "Overdrive tone", "Diode low pass", "Bass boost", "Vowel 1", "Vowel 2", "Vowel 3", "Band cut",      // 83..89
    "Driven comb 1, low", "Driven comb 1, band", "Driven comb 2, low", "Driven comb 2, band",          // 90..93
    "Overdrive 3, low", "Overdrive 3, band", "Breath", "Crossfader", "Smear", "Ladder", "Ladder squelch",  // 94..100
    "Ladder bright", "Ladder dirty", "Pole and zero", "Comb 2", "Curved low pass", "Curved band pass",  // 101..106
};
constexpr int kFilterNames = static_cast<int>(sizeof kFilterName / sizeof kFilterName[0]);

const char* const kOther[6] = {"other layer 1", "other layer 2", "texture", "root", "filter 1", "filter 2"};
}  // namespace

std::string filterDisplayName(int type) {
  if (type >= 0 && type < kFilterNames) return kFilterName[type];
  const auto& n = filterNames();
  return type >= 0 && type < static_cast<int>(n.size()) ? n[static_cast<size_t>(type)] : std::string("?");
}

std::string morphDisplayName(int m) {
  static const char* const first[] = {"Off",          "Hard sync",     "Bend up",      "Bend down",    "Bend both ways", "Pulse width",
                                      "Lean up",      "Lean down",     "Lean both ways", "Turn over",  "Mirror",         "Curve 1",
                                      "Curve 2",      "Curve 3",       "Curve 4",      "Steps",        "Odd and even",   "Smooth low",
                                      "Smooth high",  "Valve",         "Soft clip",    "Hard clip",    "Diode A",        "Diode B",
                                      "Fold straight", "Fold round",   "Square to zero", "Uneven",     "Full wave",      "Sine shape",
                                      "Pedal",        "Tape",          "Gentle drive"};
  static const char* const spec[] = {"Detune partials", "Blur",           "Spread partials",    "Add overtones",       "Add undertones",
                                     "Partial gate",    "Robot",          "Shift partials",     "Flip spectrum",       "Follow peak",
                                     "Peak octave up",  "Peak octave down", "Peak overtone up", "Peak overtone down",  "Peak overtone sweep",
                                     "Endless rise, narrow", "Endless rise, filter", "Partial comb", "Repitch",         "Repitch, keep vowels",
                                     "Phase spin",      "Vowel shift"};
  if (m >= 0 && m <= 32) return first[m];
  auto from = [](const char* kind, int i) { return std::string(kind) + " from " + kOther[i]; };
  if (m >= 33 && m < 39) return from("Frequency", m - 33);
  if (m >= 39 && m < 45) return from("Frequency curved", m - 39);
  if (m >= 45 && m < 51) return from("Frequency straight", m - 45);
  if (m >= 51 && m < 57) return from("Phase", m - 51);
  if (m == 57) return "Phase from itself";
  if (m >= 58 && m < 64) return from("Level", m - 58);
  if (m >= 64 && m < 70) return from("Ring", m - 64);
  if (m >= 70 && m < 92) return spec[m - 70];
  if (m >= 92 && m < 98) return from("Spectral mask", m - 92);
  if (m >= 98 && m < 104) return from("Spectral voice", m - 98);
  const auto& n = warpNames();
  return m >= 0 && m < static_cast<int>(n.size()) ? n[static_cast<size_t>(m)] : std::string("?");
}

const std::vector<MenuGroup>& filterMenuGroups() {
  static const std::vector<MenuGroup> g = {
      {"Smooth", {4, 5, 6, 7, 105, 8, 9, 10, 11, 12, 13, 106, 14, 15, 16, 17, 89}},
      {"Vintage", {0, 1, 2, 3, 84, 99, 100, 101, 102}},
      {"Vowel", {86, 87, 88, 96}},
      {"Comb and echo", {39, 40, 41, 42, 43, 44, 45, 46, 104, 47, 48, 49, 50, 51, 52, 53, 54, 80, 90, 91, 92, 93}},
      {"Phase", {55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 81, 98}},
      {"Shelf and bell", {71, 72, 74, 75, 73, 85}},
      {"Lo-fi", {78, 79, 76, 77, 83, 94, 95}},
      {"Special", {18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 97, 82, 103}},
  };
  return g;
}

const std::vector<MenuGroup>& morphMenuGroups() {
  static const std::vector<MenuGroup> g = [] {
    std::vector<MenuGroup> v = {
        {"Timing", {1, 2, 3, 4}},
        {"Fold and mirror", {9, 10, 24, 25, 28, 16}},
        {"Pulse and shape", {5, 6, 7, 8, 26, 27, 29}},
        {"Remap curves", {11, 12, 13, 14, 15, 17, 18}},
        {"Drive and clip", {19, 32, 31, 30, 20, 21, 22, 23}},
        {"Cross-layer frequency", {}},
        {"Cross-layer phase", {}},
        {"Cross-layer level and ring", {}},
        {"Spectral shape", {}},
        {"Spectral cross-layer", {}},
    };
    for (int i = 33; i < 51; ++i) v[5].items.push_back(i);
    for (int i = 51; i < 58; ++i) v[6].items.push_back(i);
    for (int i = 58; i < 70; ++i) v[7].items.push_back(i);
    for (int i = 70; i < 92; ++i) v[8].items.push_back(i);
    for (int i = 92; i < 104; ++i) v[9].items.push_back(i);
    return v;
  }();
  return g;
}

namespace {
bool matches(const std::string& name, const char* q) {
  if (!q || !*q) return true;
  std::string a = name, b = q;
  for (char& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  for (char& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return a.find(b) != std::string::npos;
}
// The response of filter type `type` as a small picture (the item's tooltip).
void previewCurve(Ctx& c, int f, int type) {
  ImGui::BeginTooltip();
  const ImVec2 sz(c.s(180), c.s(70));
  const ImVec2 a = ImGui::GetCursorScreenPos();
  ImGui::Dummy(sz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + sz.x, a.y + sz.y), c.P.well, c.s(3));
  constexpr int N = 64;
  float hz[N], mag[N];
  for (int i = 0; i < N; ++i) hz[i] = 20.f * std::pow(1000.f, static_cast<float>(i) / (N - 1));
  Filter::responseCurve(type, 48000, cutoffHz(val(c, filtP(f, F_Freq))), val(c, filtP(f, F_Res)), val(c, filtP(f, F_Var)), hz, mag, N);
  ImVec2 pts[N];
  for (int i = 0; i < N; ++i) {
    const float db = mag[i] > 1e-6f ? 20.f * std::log10(mag[i]) : -60.f;
    const float yn = std::clamp((db + 36.f) / 60.f, 0.f, 1.f);
    pts[i] = ImVec2(a.x + sz.x * i / (N - 1), a.y + sz.y - yn * sz.y);
  }
  dl->AddPolyline(pts, N, c.P.hot, 0, std::max(1.5f, c.s(1.6f)));
  ImGui::TextUnformatted(filterDisplayName(type).c_str());
  ImGui::EndTooltip();
}
}  // namespace

void filterPicker(Ctx& c, int k) {
  static char q[48] = "";
  if (ImGui::IsWindowAppearing()) q[0] = 0, ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(c.s(220));
  ImGui::InputTextWithHint("##fq", i18n::tr("Search filter types"), q, sizeof q);
  const int cur = vali(c, k);
  const int f = k >= filtP(1, 0) ? 1 : 0;
  ImGui::BeginChild("##fl", ImVec2(c.s(300), c.s(380)));
  for (const auto& g : filterMenuGroups()) {
    bool header = false;
    for (int i : g.items) {
      const std::string nm = filterDisplayName(i);
      if (!matches(nm, q) && !matches(g.name, q)) continue;
      if (!header) {
        ImGui::SeparatorText(i18n::tr(g.name));
        header = true;
      }
      ImGui::PushID(i);
      if (ImGui::Selectable(nm.c_str(), i == cur)) {
        setv(c, k, i);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::IsItemHovered()) previewCurve(c, f, i);
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

void morphPicker(Ctx& c, int o, int slot) {
  const int type = vali(c, oscP(o, O_Type));
  const int km = oscP(o, slot == 0 ? O_Warp1Mode : O_Warp2Mode);
  const int cur = vali(c, km);
  if (ImGui::Selectable(i18n::tr("Off"), cur == 0)) setv(c, km, 0);
  for (const auto& g : morphMenuGroups()) {
    bool any = false;
    for (int i : g.items) any = any || warpAllowed(type, i);
    if (!any) continue;
    if (ImGui::BeginMenu(i18n::tr(g.name))) {
      for (int i : g.items)
        if (warpAllowed(type, i) && ImGui::MenuItem(morphDisplayName(i).c_str(), nullptr, i == cur)) setv(c, km, i);
      ImGui::EndMenu();
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Swap the two morph slots"))) {
    const int a = vali(c, oscP(o, O_Warp1Mode)), b = vali(c, oscP(o, O_Warp2Mode));
    const double va = val(c, oscP(o, O_Warp1)), vb = val(c, oscP(o, O_Warp2));
    setv(c, oscP(o, O_Warp1Mode), b), setv(c, oscP(o, O_Warp2Mode), a);
    setv(c, oscP(o, O_Warp1), vb), setv(c, oscP(o, O_Warp2), va);
  }
}

}  // namespace gl::packs::plasma::ui
