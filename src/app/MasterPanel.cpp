// Gridline — "Master" quick panel (RN-09): the master chain's macros (tilt,
// glue, width, drive, ceiling), its loudness readout against a target, and
// the chain's effects. The chain itself is core/Mastering (Gridline effects
// in master slots), so everything here is undoable and saved with the song.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../core/Mastering.h"
#include "../core/PluginInfo.h"
#include "../packs/utility/utility_rt.h"
#include "App.h"
#include "HostedUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

using theme::pal;

namespace {

packs::utility::LevelState* meterLevels(App& a, const Song& s, int first) {
  if (s.mixer.empty() || first < 0) return nullptr;
  auto rt = a.builder.effectRuntime(s.mixer[0].id, first + mastering::kChainSlots - 1);
  auto* fx = rt && rt->fx ? dynamic_cast<packs::utility::UtilityFx*>(rt->fx.get()) : nullptr;
  return fx ? fx->levels() : nullptr;
}

std::string lufsText(float v) {
  char b[24];
  if (v <= packs::utility::kNoLoudness + 1) return "--.-";
  std::snprintf(b, sizeof b, "%.1f", static_cast<double>(v));
  return b;
}

// LCD readout with a caption above (FL-style dark field, lit digits).
void readout(App& a, const char* caption, const std::string& value, const char* unit, float w, bool lit) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddText(a.fontRegular, theme::kFontLabel, p, P.textDim, caption);
  const ImVec2 f0(p.x, p.y + 15), f1(p.x + w, p.y + 15 + 28);
  ui::Lcd(dl, f0, ImVec2(w, 28), (value + " " + unit).c_str(), false, lit);
  ImGui::Dummy(ImVec2(w, 15 + 28 + 4));
}

}  // namespace

void drawMasterPanel(App& a) {
  if (!a.showMaster) return;
  if (!ui::BeginDockWindow("Master chain", &a.showMaster, ImVec2(820, 300))) {
    ui::EndDockWindow();
    return;
  }
  const auto& P = pal();
  const Song& s = a.doc.song();
  const int first = mastering::chainSlot(s);
  mastering::Macros m;
  const bool has = mastering::readMasterChain(s, m);

  // ---- toolbar: presets, add / remove ----
  ui::BeginToolbar("##mastertb");
  {
    static int preset = 1;
    const auto& ps = mastering::presets();
    preset = std::clamp(preset, 0, static_cast<int>(ps.size()) - 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "Preset");
    ImGui::SameLine();
    if (ui::BeginCombo("##mpreset", ps[static_cast<size_t>(preset)].name, 170)) {
      for (int i = 0; i < static_cast<int>(ps.size()); ++i)
        if (ImGui::Selectable(ps[static_cast<size_t>(i)].name, i == preset)) {
          preset = i;
          std::string why;
          if (has) mastering::setMacros(a.doc, ps[static_cast<size_t>(i)].macros);
          else if (!mastering::installMasterChain(a.doc, ps[static_cast<size_t>(i)].macros, &why)) a.notify(why, true);
        }
      ImGui::EndCombo();
    }
    ImGui::SameLine(0, theme::kS3);
    if (!has) {
      if (ui::TextButton("Add to master", true, 0, "Put the chain (EQ, compressor, stereo shaper, limiter, meter) on the master track")) {
        std::string why;
        if (!mastering::installMasterChain(a.doc, ps[static_cast<size_t>(preset)].macros, &why)) a.notify(why, true);
      }
    } else if (ui::TextButton("Remove", false, 0, "Take the chain's five effects off the master")) {
      mastering::removeMasterChain(a.doc);
    }
  }
  ui::EndToolbar();

  if (!has) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 90.0f)), "No master chain",
                   "Choose a preset or \"Add to master\": five of Gridline's effects on the master, driven by the knobs here");
    ImGui::Dummy(av);
    ui::EndDockWindow();
    return;
  }

  // ---- macros ----
  auto desc = [](const char* name, const char* unit, double lo, double hi, double def) {
    ParamDesc d;
    d.name = name, d.unit = unit, d.min = lo, d.max = hi, d.def = def;
    return d;
  };
  static const ParamDesc kTilt = desc("Tilt", "dB", -6, 6, 0), kGlue = desc("Glue", "%", 0, 1, 0.3), kWidth = desc("Width", "%", 0, 2, 1),
                         kDrive = desc("Drive", "dB", 0, 12, 0), kCeil = desc("Ceiling", "dB", -6, 0, -1);
  struct K {
    const ParamDesc* d;
    float* v;
    bool bipolar;
    const char* hint;
  } knobs[] = {{&kTilt, &m.tilt, true, "Tilt: brighter (right) or warmer (left) around 1 kHz"},
               {&kGlue, &m.glue, false, "Glue: slow bus compression that holds the mix together"},
               {&kWidth, &m.width, false, "Width of the sides (bass below 120 Hz stays mono)"},
               {&kDrive, &m.drive, false, "Drive into the limiter: louder, until it squashes"},
               {&kCeil, &m.ceiling, false, "Highest peak the limiter lets through"}};
  const float cell = 86, kr = 20;
  pui::beginPanel("MACROS##mmacros", cell * 5 + 20, 0);
  bool changed = false, released = false;
  static int gesture = 0;
  for (int i = 0; i < 5; ++i) {
    ImGui::PushID(i);
    if (i) ImGui::SameLine(0, 0);
    ImGui::BeginGroup();
    const float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x0 + (cell - 2 * kr) * 0.5f);
    double v = *knobs[i].v;
    bool rel = false;
    if (ui::Knob("##k", &v, *knobs[i].d, kr, nullptr, knobs[i].bipolar, &rel)) {
      *knobs[i].v = static_cast<float>(v);
      changed = true;
      if (!gesture) gesture = i + 1;
    }
    released = released || rel;
    ui::Hint(knobs[i].hint);
    char val[32];
    if (i == 1 || i == 2) std::snprintf(val, sizeof val, "%.0f%%", v * 100.0);
    else std::snprintf(val, sizeof val, "%+.1f dB", v);
    const float tw = ImGui::CalcTextSize(val).x;
    ImGui::SetCursorPosX(x0 + (cell - tw) * 0.5f);
    ImGui::TextColored(theme::vec(P.text), "%s", val);
    const float lw = ImGui::CalcTextSize(knobs[i].d->name.c_str()).x;
    ImGui::SetCursorPosX(x0 + (cell - lw) * 0.5f);
    ImGui::TextColored(theme::vec(P.textDim), "%s", knobs[i].d->name.c_str());
    ImGui::SetCursorPosX(x0);
    ImGui::Dummy(ImVec2(cell, 0));
    ImGui::EndGroup();
    ImGui::PopID();
  }
  if (changed) mastering::setMacros(a.doc, m, 0x4D415354ull + static_cast<uint64_t>(gesture));  // one undo step per drag
  if (released) {
    a.doc.endGesture();
    gesture = 0;
  }
  pui::endPanel();

  // ---- loudness ----
  ImGui::SameLine(0, theme::kS2);
  packs::utility::LevelState* lv = meterLevels(a, s, first);
  const float shortTerm = lv ? lv->shortTerm.load() : packs::utility::kNoLoudness;
  const float integrated = lv ? lv->integrated.load() : packs::utility::kNoLoudness;
  double target = a.settings.getNumber("master.targetLufs");
  pui::beginPanel("LOUDNESS##mloud", std::max(200.0f, ImGui::GetContentRegionAvail().x), 0);
  {
    const float w = 124;
    ImGui::BeginGroup();
    readout(a, "Short-term", lufsText(shortTerm), "LUFS", w, shortTerm > -70);
    ImGui::SameLine(0, theme::kS2);
    readout(a, "Integrated", lufsText(integrated), "LUFS", w, integrated > -70);
    ImGui::EndGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "Target");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(92);
    float t = static_cast<float>(target);
    if (ImGui::DragFloat("##target", &t, 0.1f, -30.0f, -5.0f, "%.1f LUFS")) a.settings.set("master.targetLufs", static_cast<double>(t)), target = t;
    ui::Hint("Loudness the mix should reach (streaming platforms play around -14 LUFS)");
    const float ref = integrated > -70 ? integrated : shortTerm;
    if (ref > -70) {
      ImGui::SameLine();
      const double diff = ref - target;
      ImGui::TextColored(theme::vec(std::fabs(diff) < 1.0 ? P.led : P.hot), "%+.1f LU", diff);
    }
    if (ui::TextButton("Match target", false, 0, "Set the drive so the mix reaches the target (play the loudest part, then press)") && ref > -70) {
      mastering::Macros k = m;
      k.drive = mastering::driveForTarget(m, ref, target);
      mastering::setMacros(a.doc, k);
      if (lv) lv->resetRequest.store(true);
    }
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Reset meter", false, 0, "Start the integrated measurement again") && lv) lv->resetRequest.store(true);
  }
  pui::endPanel();

  // ---- the chain ----
  const MixerTrack& master = s.mixer[0];
  pui::beginPanel("CHAIN##mchain", ImGui::GetContentRegionAvail().x, 0, "master slots");
  for (int k = 0; k < mastering::kChainSlots; ++k) {
    const int slot = first + k;
    const EffectSlot& es = master.slots[static_cast<size_t>(slot)];
    const PluginInfo* info = findPlugin(es.plugin.type);
    ImGui::PushID(slot);
    if (k) ImGui::SameLine(0, theme::kS2);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(96.0f, (ImGui::GetContentRegionAvail().x - theme::kS2 * static_cast<float>(4 - k)) / static_cast<float>(5 - k));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hov = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + 24));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 24), hov ? P.surface700 : P.field, theme::kRadiusSm);
    ui::Led(dl, ImVec2(p.x + 10, p.y + 12), 4.0f, es.enabled);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x + 20, p.y + 5), w - 24, es.enabled ? P.text : P.textFaint,
                        info ? info->name.c_str() : es.plugin.type.c_str());
    ImGui::InvisibleButton("##slot", ImVec2(w, 24));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      if (ImGui::GetIO().MousePos.x < p.x + 18) a.doc.setParam(slotEnableParam(master.id, slot), es.enabled ? 0.0 : 1.0, false);
      else togglePluginWindow(a, PluginTarget{master.id, slot});
    }
    ui::Hint("Click: open the effect \xC2\xB7 click the lamp: bypass / enable");
    ImGui::PopID();
  }
  pui::endPanel();
  ui::EndDockWindow();
}

}  // namespace gl
