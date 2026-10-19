// Gridline — helpers shared by the J-19 instrument editors
// (PluginEditorsFlSynth.cpp / PluginEditorsFlSynth2.cpp): form fitting,
// labelled wheels, captioned panels, offline note previews, an audition
// keyboard and sample waveforms, all in the plugin's skin.
#pragma once
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../engine/Generators.h"
#include "../engine/Sample.h"
#include "InstrumentSkin.h"
#include "PluginEditorsCommon.h"
#include "PluginSkin.h"

namespace gl::flsyn {

// Form scaled so the reference panel (kW × kH) fills `width`, but no taller
// than maxH; centred horizontally inside `width`.
inline skin::Form fitForm(float width, float kW, float kH, float maxH) {
  float s = std::min(width / kW, maxH / kH);
  ImVec2 c = ImGui::GetCursorScreenPos();
  return skin::Form{ImVec2(std::round(c.x + (width - kW * s) * 0.5f), c.y), s};
}
// Label / value font for a reference size of `px` form pixels: reference
// panels use tiny bitmap fonts, so text is set at least 1.3× and 10 px.
inline float lf(const skin::Form& F, float px) { return std::clamp(px * std::max(F.s, 1.3f), 10.0f, 15.0f); }
// Reserves the editor's full width × the form's height.
inline void endForm(const skin::Form& F, float width, float kH) {
  ImGui::SetCursorScreenPos(ImVec2(F.o.x, F.o.y));
  ImGui::Dummy(ImVec2(std::min(width, ImGui::GetContentRegionAvail().x + 1), kH * F.s));
}

// Wheel drawn with `kw` (cap / ring / indicator) and labelled underneath in
// `k`'s label colour (reference panels often use light caps with dark lines).
inline bool wheelL(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float size, const skin::Skin& k,
                   const skin::Skin& kw, const char* label, int flags = skin::WheelBigLine) {
  bool ch = skin::wheel(a, t, param, F.p(x, y), size * F.s, kw, flags);
  if (label && *label)
    skin::label(ImGui::GetWindowDrawList(), F.p(x + size * 0.5f, 0).x, F.p(0, y + size + 2).y, label, k.textDim, lf(F, 7.5f));
  return ch;
}

// Captioned panel ("sober" reference panel): well + caption at the top left.
inline void panelBox(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k, const char* caption) {
  skin::well(dl, F.p(x, y), F.p(x + w, y + h), k, 3 * F.s);
  if (caption && *caption) skin::text(dl, F.p(x + 6, y + 3), k.text, caption, lf(F, 8.0f));
}

// Caption pill (reference "rounded subtle" label) at form coordinates.
inline void pillAt(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k, const char* text) {
  skin::pill(dl, F.p(x, y), F.p(x + w, y + h), k, text, lf(F, 7.5f));
}

// Offline preview of one note of an instrument, cached per target by parameter values.
inline const std::vector<float>& renderPreview(const PluginTarget& t, const PluginState& st, double seconds, double note = 60) {
  struct Preview {
    std::vector<float> params;
    std::vector<float> wave;
    double seconds = 0;
  };
  static std::map<uint64_t, Preview> previews;
  Preview& pv = previews[t.key()];
  if (pv.params == st.params && pv.seconds == seconds && !pv.wave.empty()) return pv.wave;
  pv.params = st.params;
  pv.seconds = seconds;
  auto g = createGenerator(st.type);
  const int sr = 22050, n = std::max(64, static_cast<int>(seconds * sr));
  pv.wave.assign(static_cast<size_t>(n), 0.f);
  if (!g) return pv.wave;
  std::vector<float> r(static_cast<size_t>(n), 0.f);
  ChannelAux aux;
  ProcessContext ctx;
  ctx.sampleRate = sr;
  g->noteOn(1, note, 0.9f, 0);
  for (int p = 0; p < n; p += 512) {
    int kk = std::min(512, n - p);
    g->process(st, aux, ctx, pv.wave.data() + p, r.data() + p, kk);
  }
  return pv.wave;
}

// Min/max waveform of a buffer inside a graph well.
inline void waveWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& k, const std::vector<float>& w, const char* caption = nullptr) {
  skin::graphWell(dl, p0, p1, k);
  const float W = p1.x - p0.x - 6, H = p1.y - p0.y - 6;
  const float mid = (p0.y + p1.y) * 0.5f;
  dl->AddLine(ImVec2(p0.x + 2, mid), ImVec2(p1.x - 2, mid), k.graphGrid);
  if (!w.empty() && W > 2) {
    float pk = 1e-6f;
    for (float v : w) pk = std::max(pk, std::fabs(v));
    float scale = 0.48f / std::max(0.25f, pk);
    int cols = static_cast<int>(W);
    for (int x = 0; x < cols; ++x) {
      size_t a0 = w.size() * static_cast<size_t>(x) / static_cast<size_t>(cols);
      size_t a1 = std::max(a0 + 1, w.size() * static_cast<size_t>(x + 1) / static_cast<size_t>(cols));
      float lo = 0, hi = 0;
      for (size_t i = a0; i < a1 && i < w.size(); ++i) lo = std::min(lo, w[i]), hi = std::max(hi, w[i]);
      dl->AddLine(ImVec2(p0.x + 3 + x, mid - hi * H * scale), ImVec2(p0.x + 3 + x, mid - lo * H * scale + 1), k.graphCurve);
    }
  }
  if (caption) skin::text(dl, ImVec2(p0.x + 5, p0.y + 3), k.graphText, caption, 9.0f);
}

// Waveform of a loaded sample inside a graph well (iskin::wave in the skin's colours).
inline void sampleWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& k, const Sample* smp) {
  skin::graphWell(dl, p0, p1, k);
  iskin::wave(dl, smp, ImVec2(p0.x + 2, p0.y + 2), ImVec2(p1.x - 2, p1.y - 2), k.graphCurve, 0, 1, k.graphGrid);
}

// Audition keyboard on the form grid (the J-2 instrument keyboard): `whites`
// white keys from lowNote (a C). With rootParam, Alt+click sets the root note.
inline void auditionKeyboard(App& a, const PluginTarget& t, const skin::Form& F, float x, float y, float w, float h, int lowNote, int whites,
                             const skin::Skin& k, int mark = -1, int rootParam = -1) {
  iskin::Skin ik;
  ik.o = F.o, ik.s = F.s;
  iskin::KeyColors kc;
  kc.mark = k.accent2C();
  kc.pressed = k.fill;
  const bool pick = rootParam >= 0 && ImGui::GetIO().KeyAlt;
  int n = iskin::keyboard(a, t, ik, x, y, w, h, lowNote, whites, kc, mark, nullptr, pick);
  if (ImGui::IsItemHovered()) a.setHint(rootParam >= 0 ? "Click a key to audition · Alt+click sets the root note" : "Click a key to audition");
  if (pick && n >= 0) a.doc.setParam(t.pid(rootParam), n, false);
}

// File dialog for audio files.
inline void openSampleDialog(App& a, const std::string& title, std::function<void(const std::string&)> ok) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = title;
  const char* h = std::getenv("HOME");
  a.fileDialog.dir = h ? h : ".";
  a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3"};
  a.fileDialog.onOk = std::move(ok);
}

// Accepts an audio file dropped on the last item; returns the path or "".
inline std::string acceptAudioDrop() {
  std::string out;
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
      std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (isAudioFile(path)) out = path;
    }
    ImGui::EndDragDropTarget();
  }
  return out;
}

}  // namespace gl::flsyn
