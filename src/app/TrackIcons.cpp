// Gridline — in-house mixer track icons. Simple line drawings on a unit grid
// (-1..1), scaled to the requested size; original artwork.
#include "TrackIcons.h"
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
  const float r = size * 0.5f;
  const float t = std::max(1.0f, size / 11.0f);  // stroke
  auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
  auto line = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), col, t); };
  switch (icon) {
    case 1:  // kick: drum head with beater dot
      dl->AddCircle(c, r * 0.85f, col, 20, t);
      dl->AddCircleFilled(c, r * 0.28f, col, 12);
      break;
    case 2:  // snare: shallow cylinder
      dl->AddEllipse(P(0, -0.45f), ImVec2(r * 0.85f, r * 0.28f), col, 0, 20, t);
      line(-0.85f, -0.45f, -0.85f, 0.45f);
      line(0.85f, -0.45f, 0.85f, 0.45f);
      dl->PathArcTo(P(0, 0.45f), r * 0.85f, 0.0f, 3.14159f, 12);
      dl->PathStroke(col, 0, t);
      break;
    case 3:  // hats: two cymbals on a stand
      dl->AddEllipseFilled(P(0, -0.45f), ImVec2(r * 0.9f, r * 0.16f), col, 0, 16);
      dl->AddEllipse(P(0, -0.15f), ImVec2(r * 0.9f, r * 0.16f), col, 0, 16, t);
      line(0, -0.6f, 0, 0.9f);
      line(-0.45f, 0.9f, 0.45f, 0.9f);
      break;
    case 4:  // percussion: shaker with dots
      dl->AddRect(P(-0.4f, -0.85f), P(0.4f, 0.85f), col, r * 0.3f, 0, t);
      for (float y : {-0.4f, 0.0f, 0.4f}) dl->AddCircleFilled(P(y * 0.5f, y), t * 1.1f, col, 8);
      break;
    case 5: {  // bass: deep wave
      ImVec2 pts[17];
      for (int i = 0; i < 17; ++i) {
        float x = -0.9f + 1.8f * static_cast<float>(i) / 16.0f;
        pts[i] = P(x, 0.55f * std::sin(x * 3.14159f));
      }
      dl->AddPolyline(pts, 17, col, 0, t * 1.4f);
      break;
    }
    case 6:  // keys: three white keys, two black keys
      dl->AddRect(P(-0.9f, -0.7f), P(0.9f, 0.7f), col, 1, 0, t);
      line(-0.3f, -0.7f, -0.3f, 0.7f);
      line(0.3f, -0.7f, 0.3f, 0.7f);
      dl->AddRectFilled(P(-0.42f, -0.7f), P(-0.18f, 0.1f), col);
      dl->AddRectFilled(P(0.18f, -0.7f), P(0.42f, 0.1f), col);
      break;
    case 7: {  // synth: saw wave
      ImVec2 pts[] = {P(-0.9f, 0.5f), P(-0.3f, -0.5f), P(-0.3f, 0.5f), P(0.3f, -0.5f), P(0.3f, 0.5f), P(0.9f, -0.5f)};
      dl->AddPolyline(pts, 6, col, 0, t * 1.2f);
      break;
    }
    case 8: {  // pad: soft layered waves
      for (int k = 0; k < 2; ++k) {
        ImVec2 pts[13];
        for (int i = 0; i < 13; ++i) {
          float x = -0.9f + 1.8f * static_cast<float>(i) / 12.0f;
          pts[i] = P(x, -0.25f + 0.5f * static_cast<float>(k) + 0.22f * std::sin(x * 3.14159f * 1.5f));
        }
        dl->AddPolyline(pts, 13, col, 0, t);
      }
      break;
    }
    case 9:  // guitar: body, sound hole, neck
      dl->AddCircle(P(-0.3f, 0.35f), r * 0.55f, col, 16, t);
      dl->AddCircleFilled(P(-0.3f, 0.35f), r * 0.15f, col, 8);
      line(-0.05f, 0.1f, 0.85f, -0.8f);
      break;
    case 10:  // vocal: microphone
      dl->AddRect(P(-0.3f, -0.9f), P(0.3f, 0.15f), col, r * 0.3f, 0, t);
      dl->PathArcTo(P(0, 0.05f), r * 0.55f, 0.0f, 3.14159f, 12);
      dl->PathStroke(col, 0, t);
      line(0, 0.6f, 0, 0.9f);
      line(-0.35f, 0.9f, 0.35f, 0.9f);
      break;
    case 11: {  // effects: four-point spark
      ImVec2 pts[] = {P(0, -0.95f), P(0.22f, -0.22f), P(0.95f, 0), P(0.22f, 0.22f), P(0, 0.95f), P(-0.22f, 0.22f), P(-0.95f, 0), P(-0.22f, -0.22f)};
      dl->AddConvexPolyFilled(pts, 8, col);
      break;
    }
    case 12:  // reverb: expanding arcs
      dl->AddCircleFilled(P(-0.7f, 0), t * 1.3f, col, 8);
      for (float rr : {0.5f, 0.95f, 1.4f}) {
        dl->PathArcTo(P(-0.7f, 0), r * rr, -0.9f, 0.9f, 10);
        dl->PathStroke(col, 0, t);
      }
      break;
    case 13:  // delay: fading echoes
      for (int k = 0; k < 4; ++k)
        dl->AddCircleFilled(P(-0.7f + 0.47f * static_cast<float>(k), 0), r * (0.32f - 0.06f * static_cast<float>(k)),
                            theme::withAlpha(col, 1.0f - 0.22f * static_cast<float>(k)), 10);
      break;
    case 14:  // bus: three lines merging into one
      line(-0.9f, -0.7f, 0.1f, 0);
      line(-0.9f, 0, 0.1f, 0);
      line(-0.9f, 0.7f, 0.1f, 0);
      line(0.1f, 0, 0.9f, 0);
      dl->AddCircleFilled(P(0.1f, 0), t * 1.4f, col, 8);
      break;
    case 15:  // sidechain: arrow into a key bracket
      line(-0.9f, 0, 0.2f, 0);
      line(-0.1f, -0.3f, 0.2f, 0);
      line(-0.1f, 0.3f, 0.2f, 0);
      line(0.5f, -0.8f, 0.85f, -0.8f);
      line(0.85f, -0.8f, 0.85f, 0.8f);
      line(0.85f, 0.8f, 0.5f, 0.8f);
      break;
    case 16: {  // output: speaker
      ImVec2 pts[] = {P(-0.85f, -0.3f), P(-0.4f, -0.3f), P(0.1f, -0.8f), P(0.1f, 0.8f), P(-0.4f, 0.3f), P(-0.85f, 0.3f)};
      dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
      dl->PathArcTo(P(0.25f, 0), r * 0.45f, -0.8f, 0.8f, 8);
      dl->PathStroke(col, 0, t);
      dl->PathArcTo(P(0.25f, 0), r * 0.75f, -0.8f, 0.8f, 8);
      dl->PathStroke(col, 0, t);
      break;
    }
    default: break;
  }
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
      dl->AddLine(ImVec2(p.x + 8, p.y + cell - 8), ImVec2(p.x + cell - 8, p.y + 8), P.textFaint, 1.5f);
    else
      draw(dl, i, ImVec2(p.x + cell * 0.5f, p.y + cell * 0.5f), cell * 0.55f, hov || i == current ? P.text : P.textDim);
    if (hov) ImGui::SetTooltip("%s", name(i));
    ImGui::PopID();
  }
  return picked;
}

}  // namespace gl::trackicons
