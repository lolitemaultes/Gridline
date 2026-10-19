// Gridline — "dynamics" pack: editor for Transient Split (gl.transplit).
// Overdrive / Squeeze: dist_fl_ui.cpp; Clip Dist (gl.fastdist): app/PluginEditorsK3.cpp.
#include "dyn_widgets.h"

namespace gl::packs::dynamics {

using namespace ui;

// ===================================================================== gl.transplit
void editorTransplit(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float gain = 0, in = -120;
  if (auto rt = runtime(a, t)) {
    gain = rt->meter.load();
    in = rt->input.load();
  }
  float outPk = tapPeak(t.tap(a), 800);
  History& h = history(t.key() ^ 0x5452414eull);
  if (h.due(60)) {
    float v[3] = {in, toDb(outPk), gain};
    h.push(v, 3);
  }
  const float topH = 190, mw = 14;
  ImVec2 m0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(mw, topH));
  levelBar(dl, m0, ImVec2(m0.x + mw, m0.y + topH), in, -48);
  ImGui::SameLine(0, 6);
  float inner = width - 2 * (mw + 6);
  float wSplit = 78, wDrive = 78, wOut = 78;
  float wBig = (inner - wSplit - wDrive - wOut - 24) / 2;
  pui::beginSection("SPLIT", wSplit, topH);
  {
    double sf = val(st, TsSplitFreq);
    char txt[24];
    if (sf <= 20.5)
      std::snprintf(txt, sizeof txt, "Full");
    else if (sf >= 1000)
      std::snprintf(txt, sizeof txt, "%.1f kHz", sf / 1000);
    else
      std::snprintf(txt, sizeof txt, "%.0f Hz", sf);
    knobText(a, t, TsSplitFreq, "Split", txt, 58, 15);
  }
  pui::knobCell(a, t, TsBalance, "Balance", 58, 15);
  pui::endSection();
  ImGui::SameLine(0, 6);
  pui::beginSection("ATTACK", wBig, topH);
  pui::knobCell(a, t, TsAttack, "Attack", wBig - 20, 36);
  ImGui::PushFont(nullptr, 11.0f);
  pui::choiceButtons(a, t, TsAttackShape, wBig - 20);
  ImGui::PopFont();
  pui::endSection();
  ImGui::SameLine(0, 6);
  pui::beginSection("DRIVE", wDrive, topH);
  pui::knobCell(a, t, TsDrive, "Drive", 58, 15);
  pui::knobCell(a, t, TsMix, "Mix", 58, 15);
  pui::endSection();
  ImGui::SameLine(0, 6);
  pui::beginSection("SUSTAIN", wBig, topH);
  pui::knobCell(a, t, TsRelease, "Sustain", wBig - 20, 36);
  ImGui::PushFont(nullptr, 11.0f);
  pui::choiceButtons(a, t, TsReleaseShape, wBig - 20);
  ImGui::PopFont();
  pui::endSection();
  ImGui::SameLine(0, 6);
  pui::beginSection("OUTPUT", wOut, topH);
  pui::knobCell(a, t, TsGain, "Gain", 58, 15);
  pui::endSection();
  ImGui::SameLine(0, 6);
  m0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(mw, topH));
  levelBar(dl, m0, ImVec2(m0.x + mw, m0.y + topH), toDb(outPk), -48);

  // ---- scrolling meter graph
  ImGui::Dummy(ImVec2(0, 2));
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImVec2 sz(width, 132), p1(p0.x + sz.x, p0.y + sz.y);
  ImGui::Dummy(sz);
  dl->AddRectFilled(p0, p1, P.surface950, 8);
  dl->PushClipRect(p0, p1, true);
  float mid = p0.y + sz.y * 0.62f;
  auto Y = [&](float db) { return mid - (std::clamp(db, -48.0f, 0.0f) + 48.0f) / 48.0f * (mid - p0.y - 6); };
  const int cols = std::min(History::N, static_cast<int>(sz.x / 1.5f));
  float dx = sz.x / cols;
  for (int i = 0; i < cols; ++i) {
    float x = p1.x - (i + 1) * dx;
    dl->AddRectFilled(ImVec2(x, Y(h.at(0, i))), ImVec2(x + dx + 0.5f, mid), theme::withAlpha(P.text, 0.12f));
    dl->AddRectFilled(ImVec2(x, Y(h.at(1, i))), ImVec2(x + dx + 0.5f, mid), theme::withAlpha(P.text, 0.22f));
    float g = h.at(2, i);
    float gy = std::clamp(g / 18.0f, -1.0f, 1.0f) * (p1.y - mid - 6);
    if (std::fabs(gy) > 0.3f) dl->AddRectFilled(ImVec2(x, mid), ImVec2(x + dx + 0.5f, mid + gy), g > 0 ? theme::withAlpha(P.accent, 0.7f) : theme::withAlpha(P.danger, 0.6f));
  }
  dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), P.gridBeat);
  caption(a, dl, ImVec2(p0.x + 8, p0.y + 6), P.textFaint, "LEVEL  IN / OUT");
  caption(a, dl, ImVec2(p0.x + 8, p1.y - 18), P.textFaint, "GAIN  BOOST / CUT");
  dl->PopClipRect();
}

}  // namespace gl::packs::dynamics
