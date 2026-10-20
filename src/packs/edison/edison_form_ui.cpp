// Gridline — "edison" pack: the editor form. Layout follows the reference
// editor's proportions (744 x 487 form units): transport row and record
// options top left, menu block under it, one row of tool buttons, an info
// line, the title and level meter on the right, and the waveform editor
// filling the rest. Every glyph and label is Gridline's own.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../../core/I18n.h"
#include "../../app/Theme.h"
#include "../../../third_party/imgui/imgui_internal.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace fs = std::filesystem;

namespace {

constexpr float kFormW = 744, kHeadH = 122;

enum Glyph {
  GUndo, GRedo, GCopy, GLoopSet, GMarker, GTrim, GSlice, GSave, GFadeIn, GFadeOut, GNormalize, GReverb, GBlur, GEq, GEnvelope,
  GStretch, GDenoise, GSend, GPlay, GPause, GStop, GRec, GScrub, GLoop, GFollow, GSpeaker, GSpeakerOff, GScroll
};

void line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float t) { dl->AddLine(a, b, c, t); }

// Vector glyph inside [a, b].
void glyph(ImDrawList* dl, int g, ImVec2 a, ImVec2 b, ImU32 c) {
  const float w = b.x - a.x, h = b.y - a.y, s = std::min(w, h);
  const ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  const float t = std::max(1.2f, s * 0.09f);
  auto P = [&](float x, float y) { return ImVec2(m.x + x * s * 0.5f, m.y + y * s * 0.5f); };
  switch (g) {
    case GUndo:
    case GRedo: {
      const float d = g == GUndo ? 1.f : -1.f;
      dl->PathArcTo(P(0, 0.15f), s * 0.3f, g == GUndo ? 3.4f : -0.26f, g == GUndo ? 6.6f : 2.94f, 12);
      dl->PathStroke(c, 0, t);
      ImVec2 tip = P(-0.62f * d, 0.05f);
      dl->AddTriangleFilled(tip, P(-0.62f * d + 0.35f * d, -0.28f), P(-0.62f * d + 0.38f * d, 0.3f), c);
      break;
    }
    case GCopy:
      dl->AddRect(P(-0.55f, -0.6f), P(0.25f, 0.3f), c, 1.5f, 0, t);
      dl->AddRect(P(-0.2f, -0.25f), P(0.6f, 0.65f), c, 1.5f, 0, t);
      break;
    case GLoopSet:
    case GLoop: {
      dl->PathArcTo(m, s * 0.38f, 0.5f, 5.6f, 16);
      dl->PathStroke(c, 0, t);
      dl->AddTriangleFilled(P(0.55f, 0.05f), P(0.95f, 0.05f), P(0.75f, 0.45f), c);
      if (g == GLoopSet) line(dl, P(-0.15f, -0.15f), P(0.15f, 0.15f), c, t), line(dl, P(-0.15f, 0.15f), P(0.15f, -0.15f), c, t);
      break;
    }
    case GMarker:
      line(dl, P(-0.3f, -0.7f), P(-0.3f, 0.7f), c, t);
      dl->AddTriangleFilled(P(-0.3f, -0.7f), P(0.5f, -0.45f), P(-0.3f, -0.2f), c);
      break;
    case GTrim:
      line(dl, P(-0.7f, 0), P(0.7f, 0), skin::lighten(c, 0) & 0x80FFFFFF, t);
      line(dl, P(-0.45f, -0.7f), P(-0.45f, 0.7f), c, t);
      line(dl, P(0.45f, -0.7f), P(0.45f, 0.7f), c, t);
      for (float x = -0.3f; x <= 0.31f; x += 0.15f) line(dl, P(x, -0.4f * std::cos(x * 5)), P(x, 0.4f * std::cos(x * 5)), c, t * 0.8f);
      break;
    case GSlice:
      for (int k = -1; k <= 1; ++k) line(dl, P(k * 0.45f, -0.7f), P(k * 0.45f, 0.7f), c, t * 0.8f);
      for (float x = -0.65f; x < 0.66f; x += 0.1f) {
        float amp = 0.55f * std::exp(-3.f * std::fmod(x + 0.65f, 0.45f));
        line(dl, P(x, -amp), P(x, amp), c, t * 0.6f);
      }
      break;
    case GSave:
      dl->AddRect(P(-0.6f, -0.6f), P(0.6f, 0.6f), c, 1.5f, 0, t);
      dl->AddRectFilled(P(-0.35f, -0.6f), P(0.3f, -0.15f), c);
      dl->AddRect(P(-0.35f, 0.15f), P(0.35f, 0.6f), c, 0, 0, t);
      break;
    case GFadeIn:
    case GFadeOut: {
      const float d = g == GFadeIn ? 1.f : -1.f;
      dl->AddTriangleFilled(P(-0.7f * d, 0.6f), P(0.7f * d, -0.6f), P(0.7f * d, 0.6f), skin::lighten(c, 0) & 0x60FFFFFF);
      line(dl, P(-0.7f * d, 0.6f), P(0.7f * d, -0.6f), c, t);
      line(dl, P(-0.7f, 0.6f), P(0.7f, 0.6f), c, t);
      break;
    }
    case GNormalize:
      line(dl, P(-0.7f, -0.6f), P(0.7f, -0.6f), c, t);
      line(dl, P(-0.7f, 0.6f), P(0.7f, 0.6f), c, t);
      for (float x = -0.55f; x <= 0.56f; x += 0.12f) {
        float amp = 0.55f * std::fabs(std::sin(x * 7.f));
        line(dl, P(x, -amp), P(x, amp), c, t * 0.7f);
      }
      break;
    case GReverb:
      for (int k = 0; k < 6; ++k) {
        float x = -0.6f + k * 0.24f, amp = 0.65f * std::exp(-0.45f * k);
        line(dl, P(x, 0.6f), P(x, 0.6f - 1.2f * amp), c, t);
      }
      break;
    case GBlur:
      for (int k = 0; k < 3; ++k) dl->AddCircle(m, s * (0.15f + 0.17f * k), skin::lighten(c, 0) & (k == 0 ? 0xFFFFFFFF : k == 1 ? 0xA0FFFFFF : 0x60FFFFFF), 16, t);
      break;
    case GEq: {
      ImVec2 pts[24];
      for (int k = 0; k < 24; ++k) {
        float x = -0.7f + 1.4f * k / 23.f;
        pts[k] = P(x, -0.45f * std::sin(x * 3.5f) * std::exp(-x * x));
      }
      dl->AddPolyline(pts, 24, c, 0, t);
      line(dl, P(-0.7f, 0.6f), P(0.7f, 0.6f), skin::lighten(c, 0) & 0x70FFFFFF, t * 0.7f);
      break;
    }
    case GEnvelope: {
      ImVec2 e[5] = {P(-0.7f, 0.6f), P(-0.4f, -0.5f), P(0.0f, -0.1f), P(0.35f, -0.25f), P(0.7f, 0.6f)};
      dl->AddPolyline(e, 5, c, 0, t);
      for (auto& q : e) dl->AddCircleFilled(q, t * 1.4f, c);
      break;
    }
    case GStretch:
      line(dl, P(-0.75f, 0), P(0.75f, 0), c, t);
      dl->AddTriangleFilled(P(-0.75f, 0), P(-0.4f, -0.3f), P(-0.4f, 0.3f), c);
      dl->AddTriangleFilled(P(0.75f, 0), P(0.4f, -0.3f), P(0.4f, 0.3f), c);
      line(dl, P(-0.15f, -0.65f), P(-0.15f, 0.65f), c, t * 0.7f);
      line(dl, P(0.15f, -0.65f), P(0.15f, 0.65f), c, t * 0.7f);
      break;
    case GDenoise:
      for (int k = 0; k < 9; ++k) {
        float x = -0.6f + k * 0.15f;
        float amp = (k == 4) ? 0.65f : 0.12f + 0.06f * static_cast<float>((k * 7) % 3);
        line(dl, P(x, -amp), P(x, amp), c, t * 0.8f);
      }
      break;
    case GSend:
      dl->AddRect(P(-0.7f, -0.3f), P(0.2f, 0.3f), c, 1.5f, 0, t);
      dl->AddTriangleFilled(P(0.75f, 0), P(0.3f, -0.4f), P(0.3f, 0.4f), c);
      break;
    case GPlay:
      dl->AddTriangleFilled(P(-0.4f, -0.55f), P(0.6f, 0), P(-0.4f, 0.55f), c);
      break;
    case GPause:
      dl->AddRectFilled(P(-0.45f, -0.5f), P(-0.12f, 0.5f), c);
      dl->AddRectFilled(P(0.12f, -0.5f), P(0.45f, 0.5f), c);
      break;
    case GStop:
      dl->AddRectFilled(P(-0.45f, -0.45f), P(0.45f, 0.45f), c, 1.5f);
      break;
    case GRec:
      dl->AddCircleFilled(m, s * 0.3f, c, 20);
      break;
    case GScrub:
      for (int k = 0; k < 7; ++k) {
        float x = -0.6f + k * 0.2f, amp = 0.15f + 0.4f * std::fabs(std::sin(k * 1.3f));
        line(dl, P(x, -amp), P(x, amp), c, t * 0.8f);
      }
      dl->AddTriangleFilled(P(-0.1f, -0.8f), P(0.1f, -0.8f), P(0, -0.6f), c);
      break;
    case GFollow:
      dl->AddCircle(P(-0.25f, 0), s * 0.22f, c, 12, t);
      dl->AddCircle(P(0.25f, 0), s * 0.22f, c, 12, t);
      break;
    case GSpeaker:
    case GSpeakerOff:
      dl->AddRectFilled(P(-0.6f, -0.2f), P(-0.3f, 0.2f), c);
      dl->AddTriangleFilled(P(-0.3f, -0.2f), P(0.1f, -0.55f), P(0.1f, 0.55f), c);
      dl->AddTriangleFilled(P(-0.3f, -0.2f), P(0.1f, 0.55f), P(-0.3f, 0.2f), c);
      if (g == GSpeakerOff)
        line(dl, P(0.25f, -0.3f), P(0.65f, 0.3f), c, t), line(dl, P(0.25f, 0.3f), P(0.65f, -0.3f), c, t);
      else
        dl->PathArcTo(P(0.1f, 0), s * 0.32f, -0.8f, 0.8f, 8), dl->PathStroke(c, 0, t);
      break;
    case GScroll:
      dl->AddTriangleFilled(P(-0.35f, -0.4f), P(0.35f, 0), P(-0.35f, 0.4f), c);
      line(dl, P(0.45f, -0.45f), P(0.45f, 0.45f), c, t);
      break;
    default: break;
  }
}

// Raised button (form units); true on left click, *right on right click.
bool button(const skin::Form& F, ImDrawList* dl, const char* id, float x, float y, float w, float h, int g, const char* hint, bool lit,
            bool* right = nullptr, bool enabled = true, ImU32 litCol = 0) {
  ImVec2 p0 = F.p(x, y), p1 = F.p(x + w, y + h);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(std::max(1.f, p1.x - p0.x), std::max(1.f, p1.y - p0.y)));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool clicked = enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left);
  if (right) *right = enabled && ImGui::IsItemClicked(ImGuiMouseButton_Right);
  ImU32 face = act ? C(col::BtnLo) : hov && enabled ? C(col::BtnHi) : C(col::Btn);
  dl->AddRectFilledMultiColor(p0, p1, skin::lighten(face, 0.06f), skin::lighten(face, 0.06f), skin::darken(face, 0.08f), skin::darken(face, 0.08f));
  dl->AddRect(p0, p1, C(col::BtnLo), F.d(2), 0, 1.f);
  if (lit) dl->AddRectFilled(ImVec2(p0.x + F.d(3), p1.y - F.d(3.5f)), ImVec2(p1.x - F.d(3), p1.y - F.d(1.5f)), litCol ? litCol : C(col::Lit));
  const float pad = std::min(w, h) * 0.2f;
  glyph(dl, g, F.p(x + pad, y + pad), F.p(x + w - pad, y + h - pad - (lit ? 1.5f : 0)),
        !enabled ? C(col::GlyphDim, 0.5f) : lit && g == GRec ? C(col::Rec) : lit ? (litCol ? litCol : C(col::Lit)) : C(col::Glyph));
  if (hov && hint && *hint) {
    ImGui::SetTooltip("%s", hint);
    app().setHint(hint);
  }
  return clicked;
}

void text(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 c, const char* s) { dl->AddText(f, size, p, c, s); }

// LCD-like label box.
void lcd(const skin::Form& F, ImDrawList* dl, ImFont* f, float x, float y, float w, float h, const char* s, ImU32 c, int align = 0) {
  ImVec2 p0 = F.p(x, y), p1 = F.p(x + w, y + h);
  dl->AddRectFilled(p0, p1, C(col::Lcd), F.d(2));
  const float fs = std::clamp(h * F.s * 0.72f, 9.f, 15.f);
  ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, s);
  float tx = align == 0 ? p0.x + F.d(3) : align == 1 ? (p0.x + p1.x - ts.x) * 0.5f : p1.x - F.d(3) - ts.x;
  dl->PushClipRect(p0, p1, true);
  text(dl, f, fs, ImVec2(tx, (p0.y + p1.y - ts.y) * 0.5f), c, s);
  dl->PopClipRect();
}

// Combo over a choice parameter of the slot, drawn as an LCD box.
void paramCombo(App& a, Session& s, const skin::Form& F, ImDrawList* dl, ImFont* f, const char* id, int k, float x, float y, float w, float h,
                const char* hint) {
  const PluginInfo* info = findPlugin(kType);
  if (!info) return;
  const ParamDesc& d = info->params[static_cast<size_t>(k)];
  const int v = static_cast<int>(param(a, s, k, d.def) + 0.5);
  const std::string cur = v >= 0 && v < static_cast<int>(d.choices.size()) ? d.choices[static_cast<size_t>(v)] : "?";
  ImVec2 p0 = F.p(x, y), p1 = F.p(x + w, y + h);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(p1.x - p0.x, p1.y - p0.y));
  const bool en = s.slot;
  lcd(F, dl, f, x, y, w - 9, h, cur.c_str(), en ? C(col::LcdText) : C(col::TextDim));
  dl->AddTriangleFilled(F.p(x + w - 7.5f, y + h * 0.4f), F.p(x + w - 1.5f, y + h * 0.4f), F.p(x + w - 4.5f, y + h * 0.65f), C(col::Glyph));
  if (ImGui::IsItemHovered()) app().setHint(hint), ImGui::SetTooltip("%s", hint);
  if (en && ImGui::IsItemClicked()) ImGui::OpenPopup(id);
  if (ImGui::BeginPopup(id)) {
    for (int i = 0; i < static_cast<int>(d.choices.size()); ++i)
      if (ImGui::MenuItem(d.choices[static_cast<size_t>(i)].c_str(), nullptr, i == v)) setParam(a, s, k, i);
    ImGui::EndPopup();
  }
}

void meterBar(const skin::Form& F, ImDrawList* dl, float x, float y, float w, float h, float lv) {
  ImVec2 p0 = F.p(x, y), p1 = F.p(x + w, y + h);
  dl->AddRectFilled(p0, p1, C(col::Lcd));
  const float db = lv > 1e-6f ? 20.f * std::log10(lv) : -90.f;
  const float n = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
  const float top = p1.y - (p1.y - p0.y) * n;
  for (float yy = p1.y - 2; yy > top; yy -= 3) {
    float frac = (p1.y - yy) / (p1.y - p0.y);
    ImU32 c = frac > 0.92f ? C(col::Rec) : frac > 0.75f ? C(0xD7C94A) : C(col::Wave);
    dl->AddRectFilled(ImVec2(p0.x + 1, yy - 2), ImVec2(p1.x - 1, yy), c);
  }
}

}  // namespace

// ------------------------------------------------------------------ the form
void drawForm(App& a, Session& s, float width, float maxHeight) {
  static const skin::Skin kSkin = [] {
    skin::Skin k = skin::Skin::fl(col::Panel, col::WaveBg, col::BtnLo, col::Wave, col::Btn, col::Text);
    k.lcdBg = C(col::Lcd);
    k.lcdText = C(col::LcdText);
    k.select = C(col::SelEdge);
    k.textDim = C(col::TextDim);
    return k;
  }();
  skin::Scope scope(kSkin);
  const float sc = std::max(0.6f, width / kFormW);
  const float editUnits = std::clamp(maxHeight / sc - kHeadH, 210.f, 365.f);
  skin::Form F = skin::formAt(kFormW, kHeadH + editUnits, width, 4.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* fm = a.fontMono ? a.fontMono : ImGui::GetFont();
  ImFont* fb = a.fontBold ? a.fontBold : ImGui::GetFont();
  ImFont* fr = a.fontRegular ? a.fontRegular : ImGui::GetFont();
  ImGui::PushID(static_cast<int>(s.key & 0x7FFFFFFF));

  // Panel.
  dl->AddRectFilledMultiColor(F.p(0, 0), F.p(kFormW, kHeadH), C(col::PanelHi), C(col::PanelHi), C(col::Panel), C(col::Panel));
  dl->AddRectFilled(F.p(0, kHeadH), F.p(kFormW, kHeadH + editUnits), C(col::PanelLo));
  dl->AddLine(F.p(0, kHeadH - 0.5f), F.p(kFormW, kHeadH - 0.5f), C(col::BtnLo), 1.f);

  // ---- transport row
  const bool playing = isPlaying(a, s);
  const bool loopOn = param(a, s, EdLoop, 0) >= 0.5;
  if (button(F, dl, "follow", 6, 9, 22, 25, GFollow, "Follow transport: playback starts and stops with the song", param(a, s, EdFollowHost, 0) >= 0.5) && s.slot)
    setParam(a, s, EdFollowHost, param(a, s, EdFollowHost, 0) >= 0.5 ? 0 : 1);
  if (button(F, dl, "loop", 29, 5, 33, 33, GLoop, "Loop playback (L)", loopOn) && s.slot) setParam(a, s, EdLoop, loopOn ? 0 : 1);
  if (button(F, dl, "play", 62, 5, 31, 33, playing ? GPause : GPlay, "Play / pause the selection or from the cursor (Space)", playing))
    playing ? stopPlay(a, s) : play(a, s);
  if (button(F, dl, "scrub", 93, 5, 25, 33, GScrub, "Scrub: drag in the waveform to hear it (or hold Alt)", s.scrubMode)) s.scrubMode = !s.scrubMode;
  if (button(F, dl, "stop", 118, 5, 33, 33, GStop, "Stop (cursor returns)", false)) stopPlay(a, s);
  const bool armed = s.rec.armed() || s.rec.recording();
  const ImU32 recCol = s.rec.recording() ? C(col::Rec) : C(col::Rec, 0.5f + 0.5f * static_cast<float>(std::fabs(std::sin(ImGui::GetTime() * 4))));
  if (button(F, dl, "rec", 154, 7, 29, 29, GRec, s.slot ? "Record from the slot input (R)" : "Recording needs the editor in a mixer slot", armed, nullptr, s.slot,
             recCol))
    toggleRecord(a, s);
  paramCombo(a, s, F, dl, fm, "##recmode", EdRecMode, 188, 11, 67, 21, "When to start recording");
  text(dl, fm, F.font(7.5f), F.p(257, 15), C(col::TextDim), "MAX");
  paramCombo(a, s, F, dl, fm, "##recmax", EdRecMax, 275, 11, 48, 21, "Maximum recording length");
  {  // append check box
    const bool on = param(a, s, EdAppend, 0) >= 0.5;
    ImVec2 p0 = F.p(156, 41);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("append", F.sz(62, 13));
    if (ImGui::IsItemClicked() && s.slot) setParam(a, s, EdAppend, on ? 0 : 1);
    if (ImGui::IsItemHovered()) a.setHint("Append recordings after the existing audio (else a take replaces it)");
    dl->AddRect(F.p(156, 42), F.p(166, 52), C(col::BtnLo), 1.5f, 0, 1.f);
    if (on) dl->AddRectFilled(F.p(158, 44), F.p(164, 50), C(col::Led));
    text(dl, fm, F.font(7.5f), F.p(170, 42), C(col::TextDim), "APPEND");
  }
  // Recording status / input level.
  if (s.slot) {
    char b[64];
    if (s.rec.recording())
      std::snprintf(b, sizeof b, "REC %s", timeText(a, s, s.rec.recordedFrames()).c_str());
    else if (s.rec.armed())
      std::snprintf(b, sizeof b, "ARMED");
    else
      b[0] = 0;
    if (b[0]) text(dl, fb, F.font(9), F.p(228, 41), C(col::Rec), b);
  }

  // ---- title, mute, meters
  {
    const char* title = "audio editor";
    const float fs = F.d(17);
    ImVec2 ts = fr->CalcTextSizeA(fs, FLT_MAX, 0, title);
    text(dl, fr, fs, ImVec2(F.p(698, 0).x - ts.x, F.p(0, 6).y), C(col::Text, 0.85f), title);
    const std::string nm = s.slot ? "slot editor" : "sample editor";
    ImVec2 ns = fm->CalcTextSizeA(F.font(7), FLT_MAX, 0, nm.c_str());
    text(dl, fm, F.font(7), ImVec2(F.p(698, 0).x - ns.x, F.p(0, 31).y), C(col::TextDim), nm.c_str());
  }
  const bool muted = param(a, s, EdMuteInput, 0) >= 0.5;
  if (button(F, dl, "mute", 702, 1, 25, 25, muted ? GSpeakerOff : GSpeaker, "Mute input (the slot outputs only the editor's playback)", muted, nullptr, s.slot,
             C(col::Rec)))
    setParam(a, s, EdMuteInput, muted ? 0 : 1);
  meterBar(F, dl, 731, 2, 5, 116, s.inMeter);
  meterBar(F, dl, 737, 2, 5, 116, s.outMeter);

  // ---- menus + toolbar
  drawMenus(a, s, F, dl);
  struct Tb {
    float x;
    int g;
    const char* hint;
  };
  const bool busy = s.job.active();
  const bool hasAudio = s.frames() > 0;
  bool r = false;
  float x = 251;
  auto tb = [&](const char* id, int g, const char* hint, bool lit = false, bool en = true) {
    bool c = button(F, dl, id, x, 57, 24, 31, g, hint, lit, &r, en && !busy);
    x += 24.9f;
    return c;
  };
  if (tb("t_undo", GUndo, "Undo / right-click: redo (Ctrl+Z / Ctrl+Y)", false, s.hist.canUndo() || s.hist.canRedo())) undo(a, s);
  if (r) redo(a, s);
  if (tb("t_redo", GRedo, "Redo", false, s.hist.canRedo())) redo(a, s);
  if (tb("t_copy", GCopy, "Copy selection / right-click: paste", false, hasAudio)) clipboard() = extract(s.cur().a(), target(s));
  if (r && clipboard().frames() > 0) {
    Audio clip = clipboard();
    Range sel = s.sel;
    int64_t at = s.cursor;
    runEdit(a, s, "Paste", [clip, sel, at](const Content& c) { return sel.empty() ? insertAudio(c, at, clip) : replaceRange(c, sel, clip); });
  }
  if (tb("t_loop", GLoopSet, "Set loop points from the selection / right-click: crossfade the loop seam", s.cur().hasLoop(), hasAudio))
    applyNow(a, s, s.sel.empty() ? "Clear loop" : "Set loop", setLoop(s.cur(), s.sel));
  if (r && s.cur().hasLoop()) {
    Range lp{s.cur().loopStart, s.cur().loopEnd};
    runEdit(a, s, "Crossfade loop", [lp](const Content& c) { return crossfadeLoop(c, lp, static_cast<int64_t>(c.rate() * 0.03)); });
  }
  if (tb("t_marker", GMarker, "Add marker at the cursor, or a region from the selection (M) / right-click: remove the nearest marker", false, hasAudio))
    addMarkerAtCursor(a, s);
  if (r && !s.cur().markers.empty()) {
    size_t best = 0;
    for (size_t i = 1; i < s.cur().markers.size(); ++i)
      if (std::llabs(s.cur().markers[i].pos - s.cursor) < std::llabs(s.cur().markers[best].pos - s.cursor)) best = i;
    applyNow(a, s, "Remove marker", removeMarker(s.cur(), best));
  }
  if (tb("t_trim", GTrim, "Trim side silence / right-click: noise gate...", false, hasAudio))
    runEdit(a, s, "Trim side silence", [](const Content& c) { return trimSilence(c, -60); });
  if (r) openTool(s, Tool::Gate);
  if (tb("t_slice", GSlice, "Detect beats and place slice markers / right-click: slicing options...", false, hasAudio)) detectSlices(a, s, 0.5);
  if (r) openTool(s, Tool::Slice);
  if (tb("t_save", GSave, "Save as WAV... / right-click: save as FLAC...", false, hasAudio)) saveAs(a, s, FileFormat::Wav);
  if (r) saveAs(a, s, FileFormat::Flac);
  const Range tr = target(s);
  const Curve fc = static_cast<Curve>(s.fadeCurve);
  if (tb("t_fadein", GFadeIn, "Fade in the selection / right-click: declick in (5 ms)", false, hasAudio))
    runEdit(a, s, "Fade in", [tr, fc](const Content& c) { return fadeIn(c, tr, fc); });
  if (r) runEdit(a, s, "Declick in", [tr](const Content& c) { return fadeIn(c, {tr.a, std::min(tr.b, tr.a + static_cast<int64_t>(c.rate() * 0.005))}, Curve::SCurve); });
  if (tb("t_fadeout", GFadeOut, "Fade out the selection / right-click: declick out (5 ms)", false, hasAudio))
    runEdit(a, s, "Fade out", [tr, fc](const Content& c) { return fadeOut(c, tr, fc); });
  if (r) runEdit(a, s, "Declick out", [tr](const Content& c) { return fadeOut(c, {std::max(tr.a, tr.b - static_cast<int64_t>(c.rate() * 0.005)), tr.b}, Curve::SCurve); });
  if (tb("t_norm", GNormalize, "Normalize to 0 dB / right-click: normalize each region", false, hasAudio))
    runEdit(a, s, "Normalize", [tr](const Content& c) { return normalize(c, tr, 0); });
  if (r)
    runEdit(a, s, "Normalize regions", [](const Content& c) {
      Content o = c;
      for (const auto& g : c.regions) o = normalize(o, {g.start, g.end}, 0);
      return o;
    });
  if (tb("t_reverb", GReverb, "Convolution reverb...", false, hasAudio)) openTool(s, Tool::Reverb);
  if (tb("t_blur", GBlur, "Blur...", false, hasAudio)) openTool(s, Tool::Blur);
  if (tb("t_eq", GEq, "Equalize (drawn curve)...", false, hasAudio)) openTool(s, Tool::Equalize);
  if (tb("t_env", GEnvelope, "Amplitude envelope (drawn)...", false, hasAudio)) openTool(s, Tool::Envelope);
  if (tb("t_stretch", GStretch, "Time stretch / pitch shift... / right-click: tune to note...", false, hasAudio)) openTool(s, Tool::Stretch);
  if (r) openTool(s, Tool::Tune);
  if (tb("t_denoise", GDenoise, "Clean up (denoise)... / right-click: take the noise profile from the selection", s.noise.valid(), hasAudio))
    openTool(s, Tool::Denoise);
  if (r) {
    s.noise = acquireNoise(s.cur().a(), target(s));
    say(s, s.noise.valid() ? "Noise profile taken from the selection" : "Select at least 50 ms of noise first");
  }
  if (tb("t_send", GSend, "Send to the playlist as an audio clip / right-click: send to a new sampler channel", false, hasAudio)) sendToPlaylist(a, s);
  if (r) sendToSampler(a, s, false);

  // ---- info line
  {
    char b[96];
    std::snprintf(b, sizeof b, "%.0fHz", s.rate());
    lcd(F, dl, fm, 7, 103, 58, 17, b, C(col::LcdText));
    lcd(F, dl, fm, 68, 103, 17, 17, "32", C(col::LcdText), 1);
    if (ImGui::IsMouseHoveringRect(F.p(68, 103), F.p(85, 120))) a.setHint("Edits are kept as 32-bit float; exports are 24-bit");
    ImGui::SetCursorScreenPos(F.p(87, 103));
    ImGui::InvisibleButton("chans", F.sz(19, 17));
    const bool mono = s.cur().a().channels == 1;
    lcd(F, dl, fm, 87, 103, 19, 17, mono ? "M" : "S", C(col::LcdText), 1);
    if (ImGui::IsItemHovered()) a.setHint(mono ? "Mono: click to make stereo" : "Stereo: click to mix to mono");
    if (ImGui::IsItemClicked() && hasAudio && !busy) runEdit(a, s, mono ? "To stereo" : "To mono", [mono](const Content& c) { return mono ? toStereo(c) : toMono(c); });
    std::snprintf(b, sizeof b, "%.0fBPM", a.doc.song().tempo);
    lcd(F, dl, fm, 110, 103, 52, 17, b, C(col::LcdText));
    std::string name;
    if (s.slot) {
      name = s.committed.empty() || s.committed[0] == '\x01' ? std::string(i18n::tr("(empty)")) : fs::path(s.committed).stem().string();
      const size_t dash = name.rfind('-');
      if (dash != std::string::npos && name.size() - dash == 13) name.resize(dash);  // drop the version hash
    } else {
      name = a.doc.song().channel(s.channel) ? a.doc.song().channel(s.channel)->name : std::string("?");
    }
    lcd(F, dl, fm, 166, 103, 230, 17, name.c_str(), C(col::Text));
    // Selection / length readout (right).
    const Range sr = s.sel;
    std::string selTxt = sr.empty() ? timeText(a, s, s.cursor) : timeText(a, s, sr.a) + " to " + timeText(a, s, sr.b);
    std::string lenTxt = timeText(a, s, sr.empty() ? s.frames() : sr.len());
    text(dl, fm, F.font(6.5f), F.p(400, 93), C(col::TextDim), sr.empty() ? "CURSOR" : "SELECTION");
    lcd(F, dl, fm, 400, 103, 210, 17, selTxt.c_str(), C(col::LcdText));
    text(dl, fm, F.font(6.5f), F.p(614, 93), C(col::TextDim), sr.empty() ? "LENGTH" : "SEL. LENGTH");
    lcd(F, dl, fm, 614, 103, 108, 17, lenTxt.c_str(), C(col::LcdText), 2);
  }

  // ---- waveform editor (the rest of the form)
  drawWaveArea(a, s, F.p(0, kHeadH), F.p(722, kHeadH + editUnits), F.s);
  // Auto-scroll strip on the right edge.
  if (button(F, dl, "autoscroll", 723, kHeadH, 19, editUnits, GScroll, "Auto-scroll: follow the playback position", s.autoScroll)) s.autoScroll = !s.autoScroll;

  // Busy overlay.
  if (busy) {
    char b[96];
    std::snprintf(b, sizeof b, "%s...  %.1f s", s.job.label.c_str(), ImGui::GetTime() - s.job.started);
    ImVec2 c = F.p(361, kHeadH + editUnits * 0.45f);
    ImVec2 ts = fb->CalcTextSizeA(F.font(10), FLT_MAX, 0, b);
    dl->AddRectFilled(ImVec2(c.x - ts.x * 0.5f - 12, c.y - ts.y * 0.5f - 8), ImVec2(c.x + ts.x * 0.5f + 12, c.y + ts.y * 0.5f + 8), C(col::Panel, 0.92f), 4);
    text(dl, fb, F.font(10), ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), C(col::Text), b);
  }

  drawToolDialogs(a, s);
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) handleKeys(a, s);
  skin::reserve(F);
  ImGui::PopID();
}

// ------------------------------------------------------------------ menus (reference: options menu groups)
void drawMenus(App& a, Session& s, const skin::Form& F, ImDrawList* dl) {
  static const char* kMenus[] = {"File", "Format", "Edit", "Tools", "Regions", "View", "Snap", "Select", "Zoom"};
  ImFont* fm = a.fontMono ? a.fontMono : ImGui::GetFont();
  const bool busy = s.job.active();
  const bool has = s.frames() > 0;
  for (int i = 0; i < 9; ++i) {
    const int row = i < 5 ? 0 : 1, colI = row == 0 ? i : i - 5;
    const float w = row == 0 ? 46.f : 57.5f;
    const float x = 19 + colI * w, y = 53 + row * 19.5f;
    ImVec2 p0 = F.p(x, y), p1 = F.p(x + w - 1.5f, y + 18);
    ImGui::SetCursorScreenPos(p0);
    char id[16];
    std::snprintf(id, sizeof id, "##menu%d", i);
    ImGui::InvisibleButton(id, ImVec2(p1.x - p0.x, p1.y - p0.y));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(p0, p1, hov ? C(col::BtnHi) : C(col::Btn, 0.55f), F.d(2));
    const float fs = F.font(7.5f);
    ImVec2 ts = fm->CalcTextSizeA(fs, FLT_MAX, 0, kMenus[i]);
    dl->AddText(fm, fs, ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), C(col::Text), kMenus[i]);
    char pid[24];
    std::snprintf(pid, sizeof pid, "edmenu%d", i);
    if (ImGui::IsItemClicked()) ImGui::OpenPopup(pid);
    ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + 2));
    if (!ImGui::BeginPopup(pid)) continue;
    const Range tr = target(s);
    switch (i) {
      case 0:  // File
        if (ImGui::MenuItem(i18n::tr("Load audio file..."), nullptr, false, !busy)) openFile(a, s);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Save as WAV..."), nullptr, false, has)) saveAs(a, s, FileFormat::Wav);
        if (ImGui::MenuItem(i18n::tr("Save as FLAC..."), nullptr, false, has)) saveAs(a, s, FileFormat::Flac);
        if (ImGui::MenuItem(i18n::tr("Export selection..."), nullptr, false, has)) exportSelection(a, s);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Send to playlist as audio clip"), nullptr, false, has && !busy)) sendToPlaylist(a, s);
        if (ImGui::MenuItem(i18n::tr("Send to new sampler channel"), nullptr, false, has && !busy)) sendToSampler(a, s, false);
        if (ImGui::MenuItem(i18n::tr("Send to new slicer channel (markers = slices)"), nullptr, false, has && !busy)) sendToSampler(a, s, true);
        break;
      case 1:  // Format
        if (ImGui::MenuItem(i18n::tr("Mix to mono"), nullptr, s.cur().a().channels == 1, has && !busy)) runEdit(a, s, "To mono", [](const Content& c) { return toMono(c); });
        if (ImGui::MenuItem(i18n::tr("Make stereo"), nullptr, false, has && !busy && s.cur().a().channels == 1))
          runEdit(a, s, "To stereo", [](const Content& c) { return toStereo(c); });
        if (ImGui::MenuItem(i18n::tr("Swap channels"), nullptr, false, has && !busy)) runEdit(a, s, "Swap channels", [](const Content& c) { return swapChannels(c); });
        if (ImGui::BeginMenu(i18n::tr("Sample rate"), has && !busy)) {
          for (int rate : {22050, 32000, 44100, 48000, 88200, 96000}) {
            char b[32];
            std::snprintf(b, sizeof b, "%d Hz", rate);
            if (ImGui::MenuItem(b, nullptr, std::lround(s.rate()) == rate)) {
              s.rsRate = rate;
              openTool(s, Tool::Resample);
            }
          }
          ImGui::EndMenu();
        }
        break;
      case 2:  // Edit
        if (ImGui::MenuItem(i18n::tr("Undo"), "Ctrl+Z", false, s.hist.canUndo() && !busy)) undo(a, s);
        if (ImGui::MenuItem(i18n::tr("Redo"), "Ctrl+Y", false, s.hist.canRedo() && !busy)) redo(a, s);
        if (ImGui::BeginMenu(i18n::tr("History"), s.hist.canUndo())) {
          auto l = s.hist.labels();
          for (size_t k = l.size(); k-- > 0;) ImGui::MenuItem(l[k].c_str(), nullptr, false, false);
          ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Cut"), "Ctrl+X", false, has && !s.sel.empty() && !busy)) {
          clipboard() = extract(s.cur().a(), s.sel);
          Range r = s.sel;
          runEdit(a, s, "Cut", [r](const Content& c) { return deleteRange(c, r); });
        }
        if (ImGui::MenuItem(i18n::tr("Copy"), "Ctrl+C", false, has)) clipboard() = extract(s.cur().a(), tr);
        if (ImGui::MenuItem(i18n::tr("Paste"), "Ctrl+V", false, clipboard().frames() > 0 && !busy)) {
          Audio clip = clipboard();
          Range sel = s.sel;
          int64_t at = s.cursor;
          runEdit(a, s, "Paste", [clip, sel, at](const Content& c) { return sel.empty() ? insertAudio(c, at, clip) : replaceRange(c, sel, clip); });
        }
        if (ImGui::MenuItem(i18n::tr("Delete"), "Del", false, has && !s.sel.empty() && !busy)) {
          Range r = s.sel;
          runEdit(a, s, "Delete", [r](const Content& c) { return deleteRange(c, r); });
        }
        if (ImGui::MenuItem(i18n::tr("Silence selection"), nullptr, false, has && !s.sel.empty() && !busy)) {
          Range r = s.sel;
          runEdit(a, s, "Silence", [r](const Content& c) { return silenceRange(c, r); });
        }
        if (ImGui::MenuItem(i18n::tr("Insert silence..."), nullptr, false, !busy)) openTool(s, Tool::Silence);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Trim to selection"), "Ctrl+T", false, has && !s.sel.empty() && !busy)) {
          Range r = s.sel;
          runEdit(a, s, "Trim to selection", [r](const Content& c) { return cropTo(c, r); });
        }
        if (ImGui::MenuItem(i18n::tr("Trim side silence"), nullptr, false, has && !busy)) runEdit(a, s, "Trim side silence", [](const Content& c) { return trimSilence(c, -60); });
        break;
      case 3:  // Tools
        if (ImGui::BeginMenu(i18n::tr("Scripts"), !busy)) {  // J-57: Lua audio scripts (SC-05)
          audioScriptMenuItems(a, s);
          ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(i18n::tr("Amplitude"), has && !busy)) {
          if (ImGui::MenuItem(i18n::tr("Normalize..."))) openTool(s, Tool::Normalize);
          if (ImGui::MenuItem(i18n::tr("Amplify..."))) openTool(s, Tool::Amplify);
          if (ImGui::MenuItem(i18n::tr("Fade in"))) runEdit(a, s, "Fade in", [tr, k = static_cast<Curve>(s.fadeCurve)](const Content& c) { return fadeIn(c, tr, k); });
          if (ImGui::MenuItem(i18n::tr("Fade out"))) runEdit(a, s, "Fade out", [tr, k = static_cast<Curve>(s.fadeCurve)](const Content& c) { return fadeOut(c, tr, k); });
          if (ImGui::BeginMenu(i18n::tr("Fade curve"))) {
            for (int k = 0; k < static_cast<int>(Curve::Count); ++k)
              if (ImGui::MenuItem(curveName(static_cast<Curve>(k)), nullptr, s.fadeCurve == k)) s.fadeCurve = k;
            ImGui::EndMenu();
          }
          if (ImGui::MenuItem(i18n::tr("Amplitude envelope..."))) openTool(s, Tool::Envelope);
          ImGui::Separator();
          if (ImGui::MenuItem(i18n::tr("Reverse"))) runEdit(a, s, "Reverse", [tr](const Content& c) { return reverse(c, tr); });
          if (ImGui::MenuItem(i18n::tr("Invert polarity"))) runEdit(a, s, "Invert", [tr](const Content& c) { return invert(c, tr); });
          if (ImGui::MenuItem(i18n::tr("Remove DC offset"))) runEdit(a, s, "Remove DC", [tr](const Content& c) { return removeDc(c, tr); });
          if (ImGui::MenuItem(i18n::tr("Noise gate..."))) openTool(s, Tool::Gate);
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(i18n::tr("Noise"), has && !busy)) {
          if (ImGui::MenuItem(i18n::tr("Take noise profile from selection"), nullptr, s.noise.valid())) {
            s.noise = acquireNoise(s.cur().a(), tr);
            say(s, s.noise.valid() ? "Noise profile taken" : "Select at least 50 ms of noise first");
          }
          if (ImGui::MenuItem(i18n::tr("Clean up (denoise)..."))) openTool(s, Tool::Denoise);
          if (ImGui::MenuItem(i18n::tr("Declick..."))) openTool(s, Tool::Declick);
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(i18n::tr("Time and pitch"), has && !busy)) {
          if (ImGui::MenuItem(i18n::tr("Time stretch / pitch shift..."))) openTool(s, Tool::Stretch);
          if (ImGui::MenuItem(i18n::tr("Tune to note..."))) openTool(s, Tool::Tune);
          if (ImGui::MenuItem(i18n::tr("Resample..."))) openTool(s, Tool::Resample);
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(i18n::tr("Effects"), has && !busy)) {
          if (ImGui::MenuItem(i18n::tr("Equalize..."))) openTool(s, Tool::Equalize);
          if (ImGui::MenuItem(i18n::tr("Convolution reverb..."))) openTool(s, Tool::Reverb);
          if (ImGui::MenuItem(i18n::tr("Blur..."))) openTool(s, Tool::Blur);
          if (ImGui::MenuItem(i18n::tr("Apply spectral brush..."), nullptr, false, !s.brush.empty())) openTool(s, Tool::Spectral);
          ImGui::EndMenu();
        }
        break;
      case 4:  // Regions
        if (ImGui::MenuItem(i18n::tr("Add marker / region"), "M", false, has)) addMarkerAtCursor(a, s);
        if (ImGui::MenuItem(i18n::tr("Set loop from selection"), nullptr, false, has && !s.sel.empty())) applyNow(a, s, "Set loop", setLoop(s.cur(), s.sel));
        if (ImGui::MenuItem(i18n::tr("Clear loop points"), nullptr, false, s.cur().hasLoop())) applyNow(a, s, "Clear loop", setLoop(s.cur(), {}));
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Detect beats (slice markers)..."), nullptr, false, has && !busy)) openTool(s, Tool::Slice);
        if (ImGui::MenuItem(i18n::tr("Snap markers to zero crossings"), nullptr, false, !s.cur().markers.empty() && !busy))
          applyNow(a, s, "Snap markers", snapMarkersToZero(s.cur(), static_cast<int64_t>(s.rate() * 0.01)));
        if (ImGui::MenuItem(i18n::tr("Remove all markers"), nullptr, false, !s.cur().markers.empty())) applyNow(a, s, "Remove markers", clearMarkers(s.cur()));
        if (!s.cur().markers.empty() || !s.cur().regions.empty()) {
          ImGui::Separator();
          for (size_t k = 0; k < s.cur().markers.size() && k < 40; ++k) {
            const auto& m = s.cur().markers[k];
            std::string l = m.name + "  " + timeText(a, s, m.pos) + "##m" + std::to_string(k);
            if (ImGui::MenuItem(l.c_str())) s.cursor = m.pos, s.sel = {};
          }
          for (size_t k = 0; k < s.cur().regions.size() && k < 40; ++k) {
            const auto& g = s.cur().regions[k];
            std::string l = "[" + g.name + "]  " + timeText(a, s, g.start) + "##r" + std::to_string(k);
            if (ImGui::MenuItem(l.c_str())) s.sel = {g.start, g.end};
          }
        }
        break;
      case 5:  // View
        if (ImGui::MenuItem(i18n::tr("Waveform"), "W", !s.spectral)) s.spectral = false;
        if (ImGui::MenuItem(i18n::tr("Spectrogram"), "W", s.spectral)) s.spectral = true;
        if (ImGui::MenuItem(i18n::tr("Spectral brush"), "B", s.brushMode, s.spectral)) s.brushMode = !s.brushMode;
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Time as min:sec"), nullptr, !s.barsTime)) s.barsTime = false;
        if (ImGui::MenuItem(i18n::tr("Time as bar:beat (song tempo)"), nullptr, s.barsTime)) s.barsTime = true;
        if (ImGui::MenuItem(i18n::tr("Auto-scroll"), nullptr, s.autoScroll)) s.autoScroll = !s.autoScroll;
        break;
      case 6:  // Snap
        if (ImGui::MenuItem(i18n::tr("No snap"), nullptr, s.snap == 0)) s.snap = 0;
        if (ImGui::MenuItem(i18n::tr("Zero crossings"), nullptr, s.snap == 1)) s.snap = 1;
        if (ImGui::MenuItem(i18n::tr("Markers"), nullptr, s.snap == 2)) s.snap = 2;
        if (ImGui::MenuItem(i18n::tr("Beats (song tempo)"), nullptr, s.snap == 3)) s.snap = 3;
        break;
      case 7:  // Select
        if (ImGui::MenuItem(i18n::tr("All"), "Ctrl+A", false, has)) s.sel = {0, s.frames()};
        if (ImGui::MenuItem(i18n::tr("None"), nullptr, false, !s.sel.empty())) s.sel = {};
        if (ImGui::MenuItem(i18n::tr("Between markers around the cursor"), nullptr, false, has)) {
          int64_t lo = 0, hi = s.frames();
          for (const auto& m : s.cur().markers) {
            if (m.pos <= s.cursor) lo = std::max(lo, m.pos);
            if (m.pos > s.cursor) hi = std::min(hi, m.pos);
          }
          s.sel = {lo, hi};
        }
        if (ImGui::MenuItem(i18n::tr("Loop"), nullptr, false, s.cur().hasLoop())) s.sel = {s.cur().loopStart, s.cur().loopEnd};
        break;
      case 8:  // Zoom
        if (ImGui::MenuItem(i18n::tr("Zoom in"), "+")) zoomBy(s, 0.5, s.viewStart + s.viewLen * 0.5);
        if (ImGui::MenuItem(i18n::tr("Zoom out"), "-")) zoomBy(s, 2.0, s.viewStart + s.viewLen * 0.5);
        if (ImGui::MenuItem(i18n::tr("Zoom to selection"), nullptr, false, !s.sel.empty())) zoomTo(s, s.sel);
        if (ImGui::MenuItem(i18n::tr("Show all"), "0")) zoomAll(s);
        break;
      default: break;
    }
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ keys
void handleKeys(App& a, Session& s) {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
  const bool ctrl = io.KeyCtrl || io.KeySuper;
  auto key = [](ImGuiKey k) {
    if (!ImGui::IsKeyPressed(k, false)) return false;
    ImGui::SetKeyOwner(k, ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);  // the editor consumes it
    return true;
  };
  const bool busy = s.job.active();
  if (key(ImGuiKey_Space)) isPlaying(a, s) ? stopPlay(a, s) : play(a, s);
  if (ctrl && !io.KeyShift && key(ImGuiKey_Z)) undo(a, s);
  if (ctrl && (key(ImGuiKey_Y) || (io.KeyShift && key(ImGuiKey_Z)))) redo(a, s);
  if (ctrl && key(ImGuiKey_A)) s.sel = {0, s.frames()};
  if (ctrl && key(ImGuiKey_C)) clipboard() = extract(s.cur().a(), target(s));
  if (!busy && ctrl && key(ImGuiKey_X) && !s.sel.empty()) {
    clipboard() = extract(s.cur().a(), s.sel);
    Range r = s.sel;
    runEdit(a, s, "Cut", [r](const Content& c) { return deleteRange(c, r); });
  }
  if (!busy && ctrl && key(ImGuiKey_V) && clipboard().frames() > 0) {
    Audio clip = clipboard();
    Range sel = s.sel;
    int64_t at = s.cursor;
    runEdit(a, s, "Paste", [clip, sel, at](const Content& c) { return sel.empty() ? insertAudio(c, at, clip) : replaceRange(c, sel, clip); });
  }
  if (!busy && ctrl && key(ImGuiKey_T) && !s.sel.empty()) {
    Range r = s.sel;
    runEdit(a, s, "Trim to selection", [r](const Content& c) { return cropTo(c, r); });
  }
  if (!busy && (key(ImGuiKey_Delete) || key(ImGuiKey_Backspace)) && !s.sel.empty()) {
    Range r = s.sel;
    runEdit(a, s, "Delete", [r](const Content& c) { return deleteRange(c, r); });
  }
  if (!ctrl && key(ImGuiKey_M) && s.frames() > 0) addMarkerAtCursor(a, s);
  if (!ctrl && key(ImGuiKey_L) && s.slot) setParam(a, s, EdLoop, param(a, s, EdLoop, 0) >= 0.5 ? 0 : 1);
  if (!ctrl && key(ImGuiKey_R)) toggleRecord(a, s);
  if (!ctrl && key(ImGuiKey_W)) s.spectral = !s.spectral;
  if (!ctrl && key(ImGuiKey_B) && s.spectral) s.brushMode = !s.brushMode;
  if (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) zoomBy(s, 0.5, s.viewStart + s.viewLen * 0.5);
  if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) zoomBy(s, 2.0, s.viewStart + s.viewLen * 0.5);
  if (key(ImGuiKey_0) || key(ImGuiKey_Keypad0)) zoomAll(s);
  if (key(ImGuiKey_Home)) s.cursor = 0, s.viewStart = 0;
  if (key(ImGuiKey_End)) s.cursor = s.frames();
  if (key(ImGuiKey_Escape)) s.sel = {}, s.brush.clear();
}

}  // namespace gl::packs::edison
