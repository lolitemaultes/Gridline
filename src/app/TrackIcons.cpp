// Gridline: in-house mixer track icons, original line drawings on a unit grid
// (-1..1), drawn as SVG icons (assets/icons/ui/track-*.svg).
#include "TrackIcons.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Theme.h"
#include "Widgets.h"

namespace gl::trackicons {

namespace {
const char* const kNames[] = {"Kick", "Snare", "Hats", "Percussion", "Bass", "Keys", "Synth", "Pad",
                              "Guitar", "Vocal", "Effects", "Reverb", "Delay", "Bus", "Sidechain", "Output"};
constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));
}  // namespace

int count() { return kCount; }

const char* name(int icon) { return icon >= 1 && icon <= kCount ? kNames[icon - 1] : "None"; }

void draw(ImDrawList* dl, int icon, ImVec2 c, float size, ImU32 col) {
  if (icon < 1 || icon > kCount) return;
  // SVG icons (assets/icons/ui/track-*.svg) on the old unit grid: -1..1 spans
  // `size`, fitted to the old line drawings' ink (stroke max(1, size / 11)).
  static const ui::Icon kIcons[] = {ui::Icon::TrackKick,  ui::Icon::TrackSnare,  ui::Icon::TrackHats,   ui::Icon::TrackPercussion,
                                    ui::Icon::TrackBass,  ui::Icon::TrackKeys,   ui::Icon::TrackSynth,  ui::Icon::TrackPad,
                                    ui::Icon::TrackGuitar, ui::Icon::TrackVocal, ui::Icon::TrackEffects, ui::Icon::TrackReverb,
                                    ui::Icon::TrackDelay, ui::Icon::TrackBus,    ui::Icon::TrackSidechain, ui::Icon::TrackOutput};
  ui::drawIconGlyph(dl, kIcons[icon - 1], c, size * 0.5f, std::max(1.0f, size / 11.0f), col);
}

bool picker(int current, int* out) {
  const auto& P = theme::pal();
  bool picked = false;
  const float cell = 30.0f;
  const int perRow = 8;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int i = 0; i <= kCount; ++i) {
    if (i % perRow) ImGui::SameLine(0, 2);
    ImGui::PushID(i);
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("ic", ImVec2(cell, cell))) {
      *out = i;
      picked = true;
      ImGui::CloseCurrentPopup();
    }
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), i == current ? P.surface600 : hov ? P.surface700 : P.field, theme::kRadiusSm);
    if (i == 0)
      ui::drawIconGlyph(dl, ui::Icon::Slash, ImVec2(p.x + cell * 0.5f, p.y + cell * 0.5f), (cell * 0.5f - 8) / 0.8f, 1.5f, P.textFaint);  // the old 14 px stroke (slash.svg spans 0.8 units)
    else
      draw(dl, i, ImVec2(p.x + cell * 0.5f, p.y + cell * 0.5f), cell * 0.55f, hov || i == current ? P.text : P.textDim);
    if (hov) ImGui::SetTooltip("%s", name(i));
    ImGui::PopID();
  }
  return picked;
}

}  // namespace gl::trackicons
