// Gridline — Spectra Morph editor (the reference morphing synth ships a
// custom-drawn panel without a form file; this follows its grouping): the
// preset name and master controls on top; the four corner programs around the
// X / Y morph square (each corner shows its timbre and zone count, click a
// corner to view it) with the morph motion controls; the selected program's
// spectral frames over time (darker = quieter, the sustain loop marked); the
// amp envelope, noise / drive, playback and the effects row.
#include "../../app/App.h"
#include "../../core/I18n.h"
#include "metal_widgets.h"

namespace gl::packs::models::metal {

using namespace ui;

namespace {

constexpr float kW = 760, kH = 400;

const skin::Skin& smSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3A3F4A, 0x2A2E36, 0x23272E, 0xE58F4B, 0x50565F, 0xD5D9E0);
    s.select = skin::hex(0xE58F4B);
    s.accent2 = skin::hex(0x6FC3D8);
    s.graphBg = skin::hex(0x191C21);
    s.graphGrid = skin::hex(0x272B32);
    s.graphGridStrong = skin::hex(0x353A43);
    s.graphCurve = skin::hex(0xE58F4B);
    s.lcdBg = skin::hex(0x14171B);
    s.lcdText = skin::hex(0xF0C9A4);
    return s;
  }();
  return k;
}

struct SmUi {
  int prog = 0;
};

}  // namespace

void drawSpectraMorph(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = smSkin();
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  flk::Ctx c{a, t, F, K, dl};
  SmUi& u = stateFor<SmUi>(t);
  SmData d;
  bool any = false;
  if (parseMorph(st.data, d))
    for (const auto& pr : d.prog) any = any || !pr.zones.empty();
  if (!any) d = factoryMorph(), d.name = "(built-in programs)";
  ImGui::PushID("gl.spectramorph");
  // ---- top bar
  if (!pui::titleWordmark(dl, "gl.spectramorph", F.p(12, 14), F.font(13)))  // J-136: the wordmark
    skin::text(dl, F.p(12, 14), K.select, "SPECTRA MORPH", F.font(13));
  flk::lcd(c, 150, 12, 230, 20, d.name.empty() ? "(untitled)" : d.name.c_str());
  flk::wheel(c, SmVolume, 400, 6, 30, "Volume");
  flk::wheel(c, SmTranspose, 448, 6, 30, "Trans", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, SmFine, 496, 6, 30, "Fine", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, SmGlide, 544, 6, 30, "Glide");
  flk::wheel(c, SmVelSens, 592, 6, 30, "Vel");
  flk::text(c, 646, 8, "VOICES");
  flk::digi(c, SmVoices, 646, 22, 40, 16);
  // ---- morph square with the four corners
  flk::section(c, 6, 52, 300, 250, "Morph");
  const float sx = 70, sy = 92, ss = 170;
  skin::xyPad(a, t, SmMorphX, SmMorphY, F.p(sx, sy), F.sz(ss, ss), K, nullptr);
  const float cx[4] = {sx - 58, sx + ss + 6, sx - 58, sx + ss + 6}, cy[4] = {sy + ss - 50, sy + ss - 50, sy, sy};
  for (int p = 0; p < kSmPrograms; ++p) {
    const ImVec2 q0 = F.p(cx[p], cy[p]), q1 = F.p(cx[p] + 52, cy[p] + 50);
    const auto& zs = d.prog[static_cast<size_t>(p)].zones;
    dl->AddRectFilled(q0, q1, K.graphBg, 3);
    if (p == u.prog) dl->AddRect(q0, q1, K.select, 3.0f, ImDrawFlags_None, 2.0f);
    if (!zs.empty() && !zs[0].frames.empty()) {
      const auto& fr = zs[0].frames[std::min<size_t>(zs[0].frames.size() - 1, 2)];
      float mx = 1e-6f;
      for (float v : fr.amp) mx = std::max(mx, v);
      const int nb = std::min<int>(24, static_cast<int>(fr.amp.size()));
      for (int k = 0; k < nb; ++k) {
        const float h = std::sqrt(fr.amp[static_cast<size_t>(k)] / mx) * (q1.y - q0.y - 12);
        const float bx = q0.x + 3 + (q1.x - q0.x - 6) * k / std::max(1, nb);
        dl->AddRectFilled(ImVec2(bx, q1.y - 3 - h), ImVec2(bx + std::max(1.0f, (q1.x - q0.x - 6) / std::max(1, nb) - 1), q1.y - 3), K.fill);
      }
    }
    char lbl[32];
    std::snprintf(lbl, sizeof lbl, "%c %zu", 'A' + p, zs.size());
    skin::text(dl, ImVec2(q0.x + 3, q0.y + 2), K.textDim, lbl, F.font(7.5f));
    ImGui::SetCursorScreenPos(q0);
    ImGui::PushID(p);
    ImGui::InvisibleButton("##corner", ImVec2(std::max(1.0f, q1.x - q0.x), std::max(1.0f, q1.y - q0.y)));
    if (ImGui::IsItemClicked(0)) u.prog = p;
    if (ImGui::IsItemHovered()) a.setHint("Corner program: timbre and zone count · click to show its frames");
    ImGui::PopID();
  }
  flk::text(c, 14, 274, "MOTION");
  flk::menu(c, SmMorphMode, 60, 270, 110, 18);
  flk::wheel(c, SmMorphRate, 196, 262, 28, "Rate");
  flk::wheel(c, SmMorphX, 240, 262, 28, "X");
  // ---- frames of the selected program
  flk::section(c, 312, 52, 442, 250, "Frames");
  {
    const ImVec2 g0 = F.p(318, 72), g1 = F.p(748, 296);
    skin::graphWell(dl, g0, g1, K);
    const auto& zs = d.prog[static_cast<size_t>(u.prog)].zones;
    if (!zs.empty() && !zs[0].frames.empty()) {
      const SmZone& z = zs[0];
      double total = 0;
      for (const auto& f : z.frames) total += std::max(1, f.hold);
      float mx = 1e-6f;
      for (const auto& f : z.frames)
        for (float v : f.amp) mx = std::max(mx, v * f.level);
      const int rows = 48;
      const float W = g1.x - g0.x - 4, H = g1.y - g0.y - 4;
      double t0 = 0;
      for (const auto& f : z.frames) {
        const float x0 = g0.x + 2 + static_cast<float>(t0 / total) * W;
        t0 += std::max(1, f.hold);
        const float x1 = g0.x + 2 + static_cast<float>(t0 / total) * W;
        for (int k = 0; k < rows && k < static_cast<int>(f.amp.size()); ++k) {
          const float v = std::sqrt(std::clamp(f.amp[static_cast<size_t>(k)] * f.level / mx, 0.0f, 1.0f));
          if (v < 0.02f) continue;
          const float y1 = g1.y - 2 - H * k / rows, y0 = g1.y - 2 - H * (k + 1) / rows;
          dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x0 + 1, x1), y1), skin::hex(0xE58F4B, v));
        }
      }
      if (z.loopStart >= 0 && z.loopEnd > z.loopStart && total > 0) {
        const float la = g0.x + 2 + static_cast<float>(z.loopStart / total) * W, lb = g0.x + 2 + static_cast<float>(z.loopEnd / total) * W;
        dl->AddLine(ImVec2(la, g0.y), ImVec2(la, g1.y), K.accent2C(), 2);
        dl->AddLine(ImVec2(lb, g0.y), ImVec2(lb, g1.y), K.accent2C(), 2);
      }
      char info[96];
      std::snprintf(info, sizeof info, "PROGRAM %c · %zu ZONES · %zu FRAMES · %.2f S", 'A' + u.prog, zs.size(), z.frames.size(),
                    total * kSmStepSec);
      skin::text(dl, F.p(324, 76), K.textDim, info, F.font(7.5f));
    } else {
      skin::text(dl, F.p(324, 76), K.textDim, "EMPTY PROGRAM", F.font(8));
    }
  }
  // ---- envelope / playback / fx
  flk::section(c, 6, 308, 236, 86, "Amp");
  flk::wheel(c, SmAttack, 18, 328, 30, "Att");
  flk::wheel(c, SmDecay, 72, 328, 30, "Dec");
  flk::wheel(c, SmSustain, 126, 328, 30, "Sus");
  flk::wheel(c, SmRelease, 180, 328, 30, "Rel");
  flk::section(c, 248, 308, 236, 86, "Play");
  flk::wheel(c, SmSpeed, 260, 328, 30, "Speed");
  flk::menu(c, SmLoop, 304, 334, 90, 18);
  flk::wheel(c, SmNoise, 402, 328, 30, "Noise");
  flk::wheel(c, SmNoiseColor, 444, 328, 30, "Color");
  flk::section(c, 490, 308, 264, 86, "Effects");
  flk::tog(c, SmChorusOn, 498, 326, 54, 14, "CHORUS");
  flk::wheel(c, SmChorusDepth, 502, 346, 24, "Dep");
  flk::wheel(c, SmChorusRate, 529, 346, 24, i18n::tr("Rate"));  // J-92: was missing
  flk::tog(c, SmDelayOn, 560, 326, 50, 14, "DELAY");
  flk::wheel(c, SmDelayTime, 560, 346, 24, "Time");
  flk::wheel(c, SmDelayFb, 588, 346, 24, "Fb");
  flk::tog(c, SmReverbOn, 618, 326, 56, 14, "REVERB");
  flk::wheel(c, SmReverbDecay, 620, 346, 24, "Dec");
  flk::wheel(c, SmReverbMix, 648, 346, 24, "Mix");
  flk::wheel(c, SmDrive, 708, 340, 30, "Drive");
  ImGui::PopID();
  F.end(kW, kH);
}

}  // namespace gl::packs::models::metal
