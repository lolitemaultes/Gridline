// Gridline — "edison" pack: the editor form. Layout follows the reference
// editor's proportions (744 x 487 form units): transport row and record
// options top left, menu block under it, one row of tool buttons, an info
// line, the title and level meter on the right, and the waveform editor
// filling the rest. Every glyph and label is Gridline's own.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../../core/PluginBrand.h"
#include "../../core/I18n.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../../third_party/imgui/imgui_internal.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace fs = std::filesystem;

namespace {

constexpr float kFormW = 744, kHeadH = 122;
// J-205: the head on one grid (form units). Every row shares the left edge kL
// and the right edge kR (the meters sit beyond it); controls of a row share
// its centre line.
constexpr float kL = 6, kR = 722;
constexpr float kRow1 = 8, kRow1H = 30;   // transport, record options | title, mute
constexpr float kRow2 = 48, kRow2H = 32;  // menus (two rows of five) | tool buttons
constexpr float kRow3 = 90, kRow3H = 20;  // info line
constexpr float kBtn = 30, kBtnStep = 31;  // transport buttons
constexpr float kMenuW = 46, kMenuStep = 48, kMenuH = 15, kMenuGap = 2;
constexpr float kToolW = 24, kToolStep = 25, kToolGroupGap = 4;
constexpr float kRow1Mid = kRow1 + kRow1H * 0.5f;

enum Glyph {
  GUndo, GRedo, GCopy, GLoopSet, GMarker, GTrim, GSlice, GSave, GFadeIn, GFadeOut, GNormalize, GReverb, GBlur, GEq, GEnvelope,
  GStretch, GDenoise, GSend, GPlay, GPause, GStop, GRec, GScrub, GLoop, GFollow, GSpeaker, GSpeakerOff, GScroll
};

// Vector glyph inside [a, b].
void glyph(ImDrawList* dl, int g, ImVec2 a, ImVec2 b, ImU32 c) {
  // SVG icons (assets/icons/ui) in the old vector glyphs' places: the glyph box
  // spans -1..1 (half its shorter side per unit), lines max(1.2, 9 % of it).
  const float w = b.x - a.x, h = b.y - a.y, s = std::min(w, h);
  const ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  const float t = std::max(1.2f, s * 0.09f), u = s * 0.5f;
  // Shared icons whose geometry differs: fitted to the old glyph's ink box (units).
  auto fit = [&](ui::Icon icon, float x0, float y0, float x1, float y1, bool stroked) {
    const float e = stroked ? t * 0.5f : 0.0f;
    ui::drawIconFit(dl, icon, ImVec2(m.x + x0 * u - e, m.y + y0 * u - e), ImVec2(m.x + x1 * u + e, m.y + y1 * u + e), c);
  };
  // Drawn on the old grid, fitted to the geometry alone: the old strokes are flat-ended
  // lines, so their ends add nothing (and at 2x a share of the width is a whole pixel).
  auto own = [&](ui::Icon icon) { ui::drawIconGlyph(dl, icon, m, u, 0.0f, c); };
  switch (g) {
    case GUndo: fit(ui::Icon::Undo, -0.62f, -0.45f, 0.6f, 0.34f, true); break;
    case GRedo: fit(ui::Icon::Redo, -0.6f, -0.45f, 0.62f, 0.34f, true); break;
    case GCopy: fit(ui::Icon::Clone, -0.55f, -0.6f, 0.6f, 0.65f, true); break;
    case GLoopSet: own(ui::Icon::EdisonLoopSet); break;
    case GLoop: own(ui::Icon::EdisonLoop); break;
    case GMarker: own(ui::Icon::Flag); break;
    case GTrim: own(ui::Icon::Trim); break;
    case GSlice: own(ui::Icon::SliceWave); break;
    case GSave: fit(ui::Icon::Save, -0.6f, -0.6f, 0.6f, 0.6f, true); break;
    case GFadeIn: own(ui::Icon::FadeIn); break;
    case GFadeOut: own(ui::Icon::FadeOut); break;
    case GNormalize: own(ui::Icon::Normalize); break;
    case GReverb: ui::drawIconGlyph(dl, ui::Icon::ReverbTail, m, u, 0.0f, c); break;  // flat bar ends: no stroke past them
    case GBlur: own(ui::Icon::Blur); break;
    case GEq: own(ui::Icon::EqCurve); break;
    case GEnvelope: own(ui::Icon::EnvelopePoints); break;
    case GStretch: own(ui::Icon::Stretch); break;
    case GDenoise: own(ui::Icon::Denoise); break;
    case GSend: own(ui::Icon::SendOut); break;
    case GPlay: fit(ui::Icon::Play, -0.4f, -0.55f, 0.6f, 0.55f, false); break;
    case GPause: fit(ui::Icon::Pause, -0.45f, -0.5f, 0.45f, 0.5f, false); break;
    case GStop: fit(ui::Icon::Stop, -0.45f, -0.45f, 0.45f, 0.45f, false); break;
    case GRec: fit(ui::Icon::Record, -0.6f, -0.6f, 0.6f, 0.6f, false); break;
    case GScrub: own(ui::Icon::Scrub); break;
    case GFollow: own(ui::Icon::Follow); break;
    case GSpeaker: own(ui::Icon::SpeakerSmall); break;
    case GSpeakerOff: own(ui::Icon::SpeakerSmallOff); break;
    case GScroll: ui::drawIconGlyph(dl, ui::Icon::ScrollTo, m, u, 0.0f, c); break;  // flat bar ends
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
  float tx = align == 0 ? p0.x + F.d(4) : align == 1 ? (p0.x + p1.x - ts.x) * 0.5f : p1.x - F.d(4) - ts.x;
  dl->PushClipRect(p0, p1, true);
  text(dl, f, fs, ImVec2(std::round(tx), std::round((p0.y + p1.y - ts.y) * 0.5f)), c, s);
  dl->PopClipRect();
}

// LCD box with a dim caption on the left and the value on the right (one size, one baseline).
void lcdField(const skin::Form& F, ImDrawList* dl, ImFont* f, float x, float y, float w, float h, const char* caption, const char* value) {
  ImVec2 p0 = F.p(x, y), p1 = F.p(x + w, y + h);
  dl->AddRectFilled(p0, p1, C(col::Lcd), F.d(2));
  const float fs = std::clamp(h * F.s * 0.72f, 9.f, 15.f);
  const ImVec2 vs = f->CalcTextSizeA(fs, FLT_MAX, 0, value);
  const float ty = std::round((p0.y + p1.y - vs.y) * 0.5f);
  dl->PushClipRect(p0, p1, true);
  text(dl, f, fs, ImVec2(std::round(p0.x + F.d(4)), ty), C(col::TextDim), caption);
  text(dl, f, fs, ImVec2(std::round(p1.x - F.d(4) - vs.x), ty), C(col::LcdText), value);
  dl->PopClipRect();
}

// Text vertically centred on form y `cy`, starting at form x.
void textMid(const skin::Form& F, ImDrawList* dl, ImFont* f, float fs, float x, float cy, ImU32 c, const char* s) {
  const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, s);
  text(dl, f, fs, ImVec2(std::round(F.p(x, 0).x), std::round(F.p(0, cy).y - ts.y * 0.5f)), c, s);
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
  lcd(F, dl, f, x, y, w, h, cur.c_str(), en ? C(col::LcdText) : C(col::TextDim));
  ui::drawIconFit(dl, ui::Icon::Down, F.p(x + w - 10.5f, y + h * 0.5f - 2), F.p(x + w - 4.5f, y + h * 0.5f + 2), C(col::Glyph));
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

  // ---- row 1: transport (equal buttons, one step), record options on the same centre line
  const bool playing = isPlaying(a, s);
  const bool loopOn = param(a, s, EdLoop, 0) >= 0.5;
  const float by = kRow1;
  if (button(F, dl, "follow", kL, by, kBtn, kBtn, GFollow, "Follow transport: playback starts and stops with the song", param(a, s, EdFollowHost, 0) >= 0.5) &&
      s.slot)
    setParam(a, s, EdFollowHost, param(a, s, EdFollowHost, 0) >= 0.5 ? 0 : 1);
  if (button(F, dl, "loop", kL + kBtnStep, by, kBtn, kBtn, GLoop, "Loop playback (L)", loopOn) && s.slot) setParam(a, s, EdLoop, loopOn ? 0 : 1);
  if (button(F, dl, "play", kL + 2 * kBtnStep, by, kBtn, kBtn, playing ? GPause : GPlay, "Play / pause the selection or from the cursor (Space)", playing))
    playing ? stopPlay(a, s) : play(a, s);
  if (button(F, dl, "scrub", kL + 3 * kBtnStep, by, kBtn, kBtn, GScrub, "Scrub: drag in the waveform to hear it (or hold Alt)", s.scrubMode))
    s.scrubMode = !s.scrubMode;
  if (button(F, dl, "stop", kL + 4 * kBtnStep, by, kBtn, kBtn, GStop, "Stop (cursor returns)", false)) stopPlay(a, s);
  const bool armed = s.rec.armed() || s.rec.recording();
  const ImU32 recCol = s.rec.recording() ? C(col::Rec) : C(col::Rec, 0.5f + 0.5f * static_cast<float>(std::fabs(std::sin(ImGui::GetTime() * 4))));
  if (button(F, dl, "rec", kL + 5 * kBtnStep, by, kBtn, kBtn, GRec, s.slot ? "Record from the slot input (R)" : "Recording needs the editor in a mixer slot", armed,
             nullptr, s.slot, recCol))
    toggleRecord(a, s);
  constexpr float kComboH = 22, kComboY = kRow1Mid - kComboH * 0.5f;
  const float capFs = F.font(7.5f);
  float rx = kL + 5 * kBtnStep + kBtn + 8;  // record options start after the transport group
  paramCombo(a, s, F, dl, fm, "##recmode", EdRecMode, rx, kComboY, 84, kComboH, "When to start recording");
  rx += 84 + 8;
  textMid(F, dl, fm, capFs, rx, kRow1Mid, C(col::TextDim), "MAX");
  rx += 22;
  paramCombo(a, s, F, dl, fm, "##recmax", EdRecMax, rx, kComboY, 58, kComboH, "Maximum recording length");
  rx += 58 + 10;
  {  // append check box (LED square + label on the row's centre line)
    const bool on = param(a, s, EdAppend, 0) >= 0.5;
    constexpr float kBox = 12;
    ImGui::SetCursorScreenPos(F.p(rx, kComboY));
    ImGui::InvisibleButton("append", F.sz(66, kComboH));
    if (ImGui::IsItemClicked() && s.slot) setParam(a, s, EdAppend, on ? 0 : 1);
    if (ImGui::IsItemHovered()) a.setHint("Append recordings after the existing audio (else a take replaces it)");
    const float y0 = kRow1Mid - kBox * 0.5f;
    dl->AddRectFilled(F.p(rx, y0), F.p(rx + kBox, y0 + kBox), C(col::Lcd), F.d(2));
    dl->AddRect(F.p(rx, y0), F.p(rx + kBox, y0 + kBox), C(col::BtnLo), F.d(2), 0, 1.f);
    if (on) dl->AddRectFilled(F.p(rx + 3, y0 + 3), F.p(rx + kBox - 3, y0 + kBox - 3), C(col::Led), F.d(1));
    textMid(F, dl, fm, capFs, rx + kBox + 5, kRow1Mid, on ? C(col::Text) : C(col::TextDim), "APPEND");
  }
  rx += 66 + 10;
  // Recording status (same centre line).
  if (s.slot) {
    char b[64];
    if (s.rec.recording())
      std::snprintf(b, sizeof b, "REC %s", timeText(a, s, s.rec.recordedFrames()).c_str());
    else if (s.rec.armed())
      std::snprintf(b, sizeof b, "ARMED");
    else
      b[0] = 0;
    if (b[0]) textMid(F, dl, fb, F.font(9), rx, kRow1Mid, C(col::Rec), b);
  }

  // ---- title, mute (row 1, right edge), meters beyond the right edge
  constexpr float kMuteX = kR - kBtn;
  {
    std::string word = pluginDisplayName("gl.edison");  // J-94: the catalogue name, lower case
    for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const char* title = word.c_str();
    const float fs = F.d(17);
    ImVec2 ts = fr->CalcTextSizeA(fs, FLT_MAX, 0, title);
    const float tx1 = kMuteX - 8;
    // J-136: the wordmark, right-aligned before the mute button
    if (!pui::titleWordmarkIn(dl, "gl.edison", F.p(tx1 - 138, kRow1 - 4), F.p(tx1, kRow1 + 22), fs * 1.2f, 2, C(col::Text, 0.85f)))
      text(dl, fr, fs, ImVec2(F.p(tx1, 0).x - ts.x, F.p(0, kRow1 - 2).y), C(col::Text, 0.85f), title);
    const std::string nm = s.slot ? "slot editor" : "sample editor";
    ImVec2 ns = fm->CalcTextSizeA(F.font(7), FLT_MAX, 0, nm.c_str());
    text(dl, fm, F.font(7), ImVec2(std::round(F.p(tx1, 0).x - ns.x), std::round(F.p(0, kRow1 + 23).y)), C(col::TextDim), nm.c_str());
  }
  const bool muted = param(a, s, EdMuteInput, 0) >= 0.5;
  if (button(F, dl, "mute", kMuteX, kRow1, kBtn, kBtn, muted ? GSpeakerOff : GSpeaker, "Mute input (the slot outputs only the editor's playback)", muted,
             nullptr, s.slot, C(col::Rec)))
    setParam(a, s, EdMuteInput, muted ? 0 : 1);
  meterBar(F, dl, 731, kRow1, 5, kRow3 + kRow3H - kRow1, s.inMeter);
  meterBar(F, dl, 737, kRow1, 5, kRow3 + kRow3H - kRow1, s.outMeter);

  // ---- row 2: menus + tool buttons (grouped: edit, markers, file, levels, processing, send)
  drawMenus(a, s, F, dl);
  const bool busy = s.job.active();
  const bool hasAudio = s.frames() > 0;
  bool r = false;
  float x = kR - (18 * kToolStep - 1 + 5 * kToolGroupGap);  // the row ends on the right edge
  auto tb = [&](const char* id, int g, const char* hint, bool lit = false, bool en = true) {
    bool c = button(F, dl, id, x, kRow2, kToolW, kRow2H, g, hint, lit, &r, en && !busy);
    x += kToolStep;
    return c;
  };
  auto gap = [&] { x += kToolGroupGap; };
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
  gap();
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
  gap();
  if (tb("t_save", GSave, "Save as WAV... / right-click: save as FLAC...", false, hasAudio)) saveAs(a, s, FileFormat::Wav);
  if (r) saveAs(a, s, FileFormat::Flac);
  gap();
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
  gap();
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
  gap();
  if (tb("t_send", GSend, "Send to the playlist as an audio clip / right-click: send to a new sampler channel", false, hasAudio)) sendToPlaylist(a, s);
  if (r) sendToSampler(a, s, false);

  // ---- row 3: info line (equal height, captions inside the boxes)
  {
    char b[96];
    float ix = kL;
    auto next = [&](float w) {
      const float x0 = ix;
      ix += w + 4;
      return x0;
    };
    std::snprintf(b, sizeof b, "%.0f Hz", s.rate());
    lcd(F, dl, fm, next(62), kRow3, 62, kRow3H, b, C(col::LcdText), 1);
    const float bitsX = next(26);
    lcd(F, dl, fm, bitsX, kRow3, 26, kRow3H, "32", C(col::LcdText), 1);
    if (ImGui::IsMouseHoveringRect(F.p(bitsX, kRow3), F.p(bitsX + 26, kRow3 + kRow3H))) a.setHint("Edits are kept as 32-bit float; exports are 24-bit");
    const float chX = next(26);
    ImGui::SetCursorScreenPos(F.p(chX, kRow3));
    ImGui::InvisibleButton("chans", F.sz(26, kRow3H));
    const bool mono = s.cur().a().channels == 1;
    lcd(F, dl, fm, chX, kRow3, 26, kRow3H, mono ? "M" : "S", C(col::LcdText), 1);
    if (ImGui::IsItemHovered()) a.setHint(mono ? "Mono: click to make stereo" : "Stereo: click to mix to mono");
    if (ImGui::IsItemClicked() && hasAudio && !busy) runEdit(a, s, mono ? "To stereo" : "To mono", [mono](const Content& c) { return mono ? toStereo(c) : toMono(c); });
    std::snprintf(b, sizeof b, "%.0f BPM", a.doc.song().tempo);
    lcd(F, dl, fm, next(66), kRow3, 66, kRow3H, b, C(col::LcdText), 1);
    std::string name;
    if (s.slot) {
      name = s.committed.empty() || s.committed[0] == '\x01' ? std::string(i18n::tr("(empty)")) : fs::path(s.committed).stem().string();
      const size_t dash = name.rfind('-');
      if (dash != std::string::npos && name.size() - dash == 13) name.resize(dash);  // drop the version hash
    } else {
      name = a.doc.song().channel(s.channel) ? a.doc.song().channel(s.channel)->name : std::string("?");
    }
    // Position / length on the right, flush with the right edge; the name takes the rest.
    constexpr float kLenW = 128, kPosW = 196;
    const float posX = kR - kLenW - 4 - kPosW;
    lcd(F, dl, fm, ix, kRow3, posX - 4 - ix, kRow3H, name.c_str(), s.frames() > 0 ? C(col::Text) : C(col::TextDim));
    const Range sr = s.sel;
    const std::string selTxt = sr.empty() ? timeText(a, s, s.cursor) : timeText(a, s, sr.a) + " - " + timeText(a, s, sr.b);
    const std::string lenTxt = timeText(a, s, sr.empty() ? s.frames() : sr.len());
    lcdField(F, dl, fm, posX, kRow3, kPosW, kRow3H, sr.empty() ? "CURSOR" : "SEL", selTxt.c_str());
    lcdField(F, dl, fm, kR - kLenW, kRow3, kLenW, kRow3H, sr.empty() ? "LENGTH" : "SEL LEN", lenTxt.c_str());
  }

  // ---- waveform editor (the rest of the form)
  drawWaveArea(a, s, F.p(0, kHeadH), F.p(kR, kHeadH + editUnits), F.s);
  // Auto-scroll strip on the right edge (under the meters).
  if (button(F, dl, "autoscroll", kR + 1, kHeadH, kFormW - kR - 1, editUnits, GScroll, "Auto-scroll: follow the playback position", s.autoScroll))
    s.autoScroll = !s.autoScroll;

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
    const int row = i < 5 ? 0 : 1, colI = row == 0 ? i : i - 5;  // J-205: equal cells on one grid
    const float x = kL + colI * kMenuStep, y = kRow2 + row * (kMenuH + kMenuGap);
    ImVec2 p0 = F.p(x, y), p1 = F.p(x + kMenuW, y + kMenuH);
    ImGui::SetCursorScreenPos(p0);
    char id[16];
    std::snprintf(id, sizeof id, "##menu%d", i);
    ImGui::InvisibleButton(id, ImVec2(p1.x - p0.x, p1.y - p0.y));
    const bool hov = ImGui::IsItemHovered();
    const bool open = ImGui::IsPopupOpen(("edmenu" + std::to_string(i)).c_str());
    dl->AddRectFilled(p0, p1, open ? C(col::BtnHi) : hov ? C(col::BtnHi) : C(col::Btn, 0.7f), F.d(2));
    const float fs = F.font(7.5f);
    ImVec2 ts = fm->CalcTextSizeA(fs, FLT_MAX, 0, kMenus[i]);
    dl->AddText(fm, fs, ImVec2(std::round((p0.x + p1.x - ts.x) * 0.5f), std::round((p0.y + p1.y - ts.y) * 0.5f)), open ? C(col::Lit) : C(col::Text), kMenus[i]);
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
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Split stems..."), nullptr, false, has && !busy)) splitStems(a, s);  // J-191
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
