// Gridline — "flex" pack (J-53): the stand-in player editor. Layout after the
// preset player's control page (pitch | filter envelope | filter | volume
// envelope on top, post filter | delay | reverb | limiter below, the eight
// macros along the bottom), a banner that says what plays, and a side panel
// for the stand-in (category, tone / level / octave, convert).
#include <cmath>
#include <cstdio>
#include <string>
#include "../../core/I18n.h"
#include "../../app/App.h"
#include "../../app/PluginSkin.h"
#include "../../core/FlexStandin.h"
#include "../PacksUi.h"

namespace gl::packs::flexpack {

namespace {

using namespace gl::flex;

const skin::Skin& flexSkin() {
  static const skin::Skin k = [] {
    // Slate panel, dark wells, orange lit values (FL colour roles).
    skin::Skin s = skin::Skin::fl(0x5F686D, 0x3B4246, 0x2C3134, 0xF2A33A, 0x485156, 0xDCE3E6);
    s.textDim = skin::hex(0xB4BEC2);
    s.lcdBg = skin::hex(0x232425);
    s.lcdText = skin::hex(0xF2A33A);
    return s;
  }();
  return k;
}

constexpr float kW = 1080, kH = 432;

float val(const PluginState& st, int k, float d = 0.5f) {
  audit::stateRead(k);  // J-92 binding audit
  return k < static_cast<int>(st.params.size()) ? st.params[static_cast<size_t>(k)] : d; }

// On / off button over a FLEX switch (stored as a 0..1 value).
void onButton(App& a, const PluginTarget& t, const skin::Form& F, const skin::Skin& K, int param, float x, float y, float w, const char* text) {
  const PluginState* st = t.state(a.doc.song());
  const bool on = st && val(*st, param, 0) >= 0.5f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = F.p(x, y), p1 = F.p(x + w, y + 16);
  dl->AddRectFilled(p0, p1, on ? skin::hex(0x575350) : skin::hex(0x3A3836), 2 * F.s);
  skin::ledDot(dl, ImVec2(p0.x + 8 * F.s, (p0.y + p1.y) * 0.5f), 3.2f * F.s, on, K, skin::hex(0x7BE05A));
  skin::text(dl, F.p(x + 15, y + 2), on ? K.text : K.textDim, text, F.font(9));
  ImGui::SetCursorScreenPos(p0);
  ImGui::PushID(param);
  if (ImGui::InvisibleButton("##on", ImVec2(p1.x - p0.x, p1.y - p0.y))) a.doc.setParam(t.pid(param), on ? 0.0 : 1.0, false);
  if (ImGui::IsItemHovered()) a.setHint(std::string(text) + ": " + (on ? "on" : "off"));
  ImGui::PopID();
}

void group(ImDrawList* dl, const skin::Form& F, const skin::Skin& K, float x0, float y0, float x1, float y1, const char* title) {
  skin::well(dl, F.p(x0, y0), F.p(x1, y1), K, 4 * F.s);
  skin::text(dl, F.p(x0 + 8, y0 + 4), K.textDim, title, F.font(9));
}

void wheelAt(App& a, const PluginTarget& t, const skin::Form& F, const skin::Skin& K, int param, float x, float y, float size, const char* label,
             bool centred = true) {
  skin::labeledWheel(a, t, param, F, x, y, size, K, label, skin::WheelBigLine | (centred ? skin::WheelCentered : 0));
}

}  // namespace

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st0 = t.state(a.doc.song());
  if (!st0 || st0->type != kType) return false;
  const PluginState st = *st0;  // (edits below may replace the state)
  const skin::Skin& K = flexSkin();
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  ImGui::PushID("gl.flex");

  const Identity id = identity(st.data);
  std::string why;
  const int detected = classify(id.pack, id.preset, &why);
  const int chosen = static_cast<int>(std::lround(val(st, PTemplate, 0)));
  const int cat = chosen > CatAuto && chosen < CatCount ? chosen : detected;

  // ---- banner
  dl->AddRectFilled(F.p(6, 6), F.p(kW - 6, 40), skin::hex(0x242323), 3 * F.s);
  dl->AddRectFilled(F.p(6, 6), F.p(12, 40), skin::hex(0xF2A33A), 2 * F.s);
  {
    const std::string preset = id.preset.empty() ? "(none selected)" : id.preset;
    const std::string pack = id.pack.empty() ? "unknown" : id.pack;
    const std::string line = "FLEX preset " + preset + " (pack " + pack + "): playing a Gridline stand-in; opens with the original sound in FL Studio";
    skin::text(dl, F.p(20, 10), skin::hex(0xF4F6F7), line.c_str(), F.font(11));
    char sub[200];
    std::snprintf(sub, sizeof sub, "Stand-in: %s on %s. Macros, envelopes, filter, effects, volume and pitch are kept for FL Studio.", catName(cat),
                  engineName(engineOf(cat)));
    skin::text(dl, F.p(20, 25), K.textDim, sub, F.font(9));
  }

  // ---- top row: pitch | filter envelope | filter | volume envelope
  constexpr float y0 = 48, y1 = 150;
  group(dl, F, K, 8, y0, 92, y1, "PITCH");
  wheelAt(a, t, F, K, Pitch, 26, y0 + 22, 46, "PITCH");
  {
    char b[32];
    std::snprintf(b, sizeof b, "%+.1f st", (val(st, Pitch) - 0.5) * 48.0);
    skin::lcd(dl, F.p(18, y0 + 82), F.p(82, y0 + 98), K, b, F.font(9));
  }
  group(dl, F, K, 100, y0, 348, y1, "FILTER ENVELOPE");
  const char* env[5] = {"ATT", "HOLD", "DEC", "SUS", "REL"};
  for (int k = 0; k < 5; ++k) wheelAt(a, t, F, K, FltA + k, 112 + k * 47, y0 + 34, 38, env[k]);
  group(dl, F, K, 356, y0, 560, y1, "FILTER");
  wheelAt(a, t, F, K, EnvAmt, 372, y0 + 34, 38, "ENV");
  wheelAt(a, t, F, K, Cutoff, 428, y0 + 24, 54, "CUTOFF");
  wheelAt(a, t, F, K, Reso, 500, y0 + 34, 38, "RES");
  group(dl, F, K, 568, y0, 838, y1, "VOLUME ENVELOPE");
  for (int k = 0; k < 5; ++k) wheelAt(a, t, F, K, VolA + k, 582 + k * 49, y0 + 34, 38, env[k]);

  // ---- middle row: post filter | delay | reverb | limiter
  constexpr float m0 = 158, m1 = 290;
  group(dl, F, K, 8, m0, 170, m1, "POST FILTER");
  onButton(a, t, F, K, PostOn, 18, m0 + 20, 70, "ON");
  wheelAt(a, t, F, K, PostCut, 22, m0 + 46, 52, "CUTOFF", false);
  wheelAt(a, t, F, K, PostReso, 104, m0 + 56, 38, "RES");
  group(dl, F, K, 178, m0, 420, m1, "DELAY");
  onButton(a, t, F, K, DlyOn, 188, m0 + 20, 70, "ON");
  wheelAt(a, t, F, K, DlyMix, 190, m0 + 46, 52, "MIX", false);
  wheelAt(a, t, F, K, DlyTime, 258, m0 + 40, 36, "TIME", false);
  wheelAt(a, t, F, K, DlyFb, 312, m0 + 40, 36, "FEEDBACK", false);
  wheelAt(a, t, F, K, DlyColor, 366, m0 + 40, 36, "TONE");
  wheelAt(a, t, F, K, DlyMod, 312, m0 + 92, 30, "MOD", false);
  group(dl, F, K, 428, m0, 686, m1, "REVERB");
  onButton(a, t, F, K, RevOn, 438, m0 + 20, 70, "ON");
  wheelAt(a, t, F, K, RevMix, 440, m0 + 46, 52, "MIX", false);
  wheelAt(a, t, F, K, RevSize, 508, m0 + 40, 36, "SIZE", false);
  wheelAt(a, t, F, K, RevDecay, 562, m0 + 40, 36, "DECAY", false);
  wheelAt(a, t, F, K, RevColor, 616, m0 + 40, 36, "TONE");
  wheelAt(a, t, F, K, RevMod, 562, m0 + 92, 30, "MOD", false);
  wheelAt(a, t, F, K, RevModSpeed, 616, m0 + 92, 30, "RATE", false);
  group(dl, F, K, 694, m0, 838, m1, "LIMITER");
  onButton(a, t, F, K, LimOn, 704, m0 + 20, 70, "ON");
  wheelAt(a, t, F, K, LimPre, 706, m0 + 48, 38, "INPUT");
  wheelAt(a, t, F, K, OutVol, 770, m0 + 48, 38, "OUTPUT", false);
  wheelAt(a, t, F, K, BandMix, 738, m0 + 96, 28, "BANDS", false);

  // ---- bottom row: macros
  constexpr float b0 = 298, b1 = 424;
  group(dl, F, K, 8, b0, 838, b1, "MACROS");
  for (int k = 0; k < 8; ++k) {
    char lb[12];
    std::snprintf(lb, sizeof lb, "MACRO %d", k + 1);
    wheelAt(a, t, F, K, Macro1 + k, 30 + k * 102.0f, b0 + 26, 56, lb, false);
  }
  skin::text(dl, F.p(20, b1 - 16), K.textDim, "Macro roles in the stand-in: 1 tone, 2 character, 3 attack, 4 length, 5 movement, 6 width, 7 drive, 8 space",
             F.font(8.5f));

  // ---- side panel: the stand-in
  const float sx = 846;
  group(dl, F, K, sx, y0, kW - 8, b1, "GRIDLINE STAND-IN");
  {
    char line[160];
    if (chosen > CatAuto && chosen < CatCount)
      std::snprintf(line, sizeof line, "Chosen: %s (detected %s)", catName(chosen), catName(detected));
    else if (!why.empty())
      std::snprintf(line, sizeof line, "Detected: %s (from \"%s\")", catName(detected), why.c_str());
    else
      std::snprintf(line, sizeof line, "Detected: %s (no category words)", catName(detected));
    skin::text(dl, F.p(sx + 10, y0 + 22), K.text, line, F.font(9));
    skin::text(dl, F.p(sx + 10, y0 + 40), K.textDim, "Stand-in category", F.font(8.5f));
    skin::menuBox(a, t, PTemplate, F.p(sx + 10, y0 + 54), F.sz(206, 20), K, F.font(10));
    std::snprintf(line, sizeof line, "Engine: %s", engineName(engineOf(cat)));
    skin::text(dl, F.p(sx + 10, y0 + 80), K.textDim, line, F.font(9));
  }
  wheelAt(a, t, F, K, PTone, sx + 16, y0 + 104, 40, "TONE");
  wheelAt(a, t, F, K, PLevel, sx + 88, y0 + 104, 40, "LEVEL", false);
  wheelAt(a, t, F, K, POctave, sx + 160, y0 + 104, 40, "OCTAVE");
  skin::text(dl, F.p(sx + 10, y0 + 166), K.textDim, "These three change the stand-in only;", F.font(8.5f));
  skin::text(dl, F.p(sx + 10, y0 + 179), K.textDim, "FL Studio keeps the FLEX preset as is.", F.font(8.5f));
  if (skin::button("##convert", F.p(sx + 10, b1 - 44), F.sz(206, 26), K, "Convert to Gridline instrument", false, F.font(10)))
    ImGui::OpenPopup("##flexconvert");
  if (ImGui::IsItemHovered()) a.setHint("Make this channel a normal Gridline instrument (the FLEX preset data is dropped)");
  if (ImGui::BeginPopupModal("##flexconvert", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::Text(i18n::tr("Convert to a Gridline %s?"), engineName(engineOf(cat)));
    ImGui::TextUnformatted(i18n::tr("The channel keeps the stand-in sound and becomes a normal Gridline instrument."));
    ImGui::TextUnformatted(i18n::tr("The FLEX preset data is dropped: an FL Studio export no longer opens the original sound."));
    if (ImGui::Button(i18n::tr("Convert"))) {
      setPluginState(a, t, standInState(st), "Convert to Gridline instrument");
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  F.end(kW, kH);
  ImGui::PopID();
  return true;
}

}  // namespace gl::packs::flexpack
