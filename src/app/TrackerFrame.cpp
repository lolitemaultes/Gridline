// Gridline - the Tracker layout's header band (K-106): the Order list part. See TrackerView.h.
//
// In the Tracker layout the part fills the header region with the classic tracker's boxes, drawn in
// Gridline's own style (bevelled boxes from the theme's surface roles, numbers in the monospaced face):
// the order list (derived from the playlist), the song box (steppers), the scope box (a level cell per
// channel), two button columns, the instrument and sample lists, and the page strip that picks what the
// centre shows. Every button runs a Gridline command; nothing here is stored in the project.
// In the other layouts the part is the order list alone.
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/MenuOps.h"
#include "../core/MenuParity.h"
#include "../core/Params.h"
#include "../core/TrackerGrid.h"
#include "App.h"
#include "ContextMenus.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "MenuParityUi.h"
#include "MixerMeters.h"
#include "PixelGrid.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "TrackerPages.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::trackerview {

using theme::pal;

ImFont* monoFont(App& a) { return a.fontMono ? a.fontMono : ImGui::GetFont(); }

void bevelBox(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool sunken) {
  const auto& P = pal();
  const float dpp = pixelgrid::frameScale();
  p0 = ImVec2(pixelgrid::edge(p0.x, dpp), pixelgrid::edge(p0.y, dpp));
  p1 = ImVec2(pixelgrid::edge(p1.x, dpp), pixelgrid::edge(p1.y, dpp));
  const float w = pixelgrid::whole(1.0f, dpp);
  // Two steps of edge: a raised face is lit at the top and left and shaded at the bottom and right
  // (an outer edge, and a softer inner one); a sunken well the other way round. Theme roles only.
  const ImU32 face = sunken ? P.surface950 : P.surface700;
  dl->AddRectFilled(p0, p1, face);
  const ImU32 lit = theme::mix(P.surface700, P.text, 0.42f), lit2 = theme::mix(P.surface700, P.text, 0.16f);
  const ImU32 shade = theme::mix(P.surface950, IM_COL32(0, 0, 0, 255), 0.35f), shade2 = theme::mix(P.surface700, P.surface950, 0.55f);
  auto edges = [&](ImVec2 q0, ImVec2 q1, ImU32 tl, ImU32 br) {
    dl->AddRectFilled(q0, ImVec2(q1.x, q0.y + w), tl);
    dl->AddRectFilled(q0, ImVec2(q0.x + w, q1.y), tl);
    dl->AddRectFilled(ImVec2(q0.x, q1.y - w), q1, br);
    dl->AddRectFilled(ImVec2(q1.x - w, q0.y + w), q1, br);
  };
  if (sunken) {
    edges(p0, p1, shade, theme::mix(P.surface700, P.text, 0.22f));
  } else {
    edges(p0, p1, lit, shade);
    if (p1.x - p0.x > 6 * w && p1.y - p0.y > 6 * w) edges(ImVec2(p0.x + w, p0.y + w), ImVec2(p1.x - w, p1.y - w), lit2, shade2);
  }
}

bool bevelButton(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  const bool click = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)));
  const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  const bool off = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;  // still to come, or nothing to act on
  bevelBox(dl, p0, p1, held || on);
  if (on) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), theme::withAlpha(P.hot, 0.85f));
  else if (hov && !held) dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 2), ImVec2(p1.x - 2, p1.y - 2), theme::withAlpha(P.surface600, 0.6f));
  const float fs = ImGui::GetFontSize();
  // Pressed, the label sinks a pixel with the face.
  const ImVec2 d = held ? ImVec2(1, 1) : ImVec2(0, 0);
  theme::centreText(dl, a.fontRegular, fs, ImVec2(p0.x + d.x, p0.y + d.y), ImVec2(p1.x + d.x, p1.y + d.y), on ? theme::contrastText(P.hot) : off ? P.textFaint : P.text, i18n::tr(label));
  if (hint) ui::Hint(hint);
  if (click) trackerview::keysBackToPattern();
  return click;
}

void boxTitle(App& a, ImVec2 p, const char* text) {
  ImGui::GetWindowDrawList()->AddText(a.fontBold ? a.fontBold : a.fontRegular, ImGui::GetFontSize(), p, pal().text, i18n::tr(text));
}

namespace {

// A small arrow button (a filled triangle on a bevel face); repeats while held.
bool arrowButton(App& a, const char* id, ImVec2 p0, ImVec2 p1, bool up, const char* hint) {
  const auto& P = pal();
  ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
  const bool click = bevelButton(a, id, "", p0, p1, false, hint);
  ImGui::PopItemFlag();
  const bool held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cx = std::round((p0.x + p1.x) * 0.5f) + (held ? 1 : 0), cy = std::round((p0.y + p1.y) * 0.5f) + (held ? 1 : 0);
  const float r = std::max(2.0f, std::floor(std::min(p1.x - p0.x, p1.y - p0.y) * 0.22f));
  if (up) dl->AddTriangleFilled(ImVec2(cx - r - 1, cy + r * 0.6f), ImVec2(cx + r + 1, cy + r * 0.6f), ImVec2(cx, cy - r * 0.8f), P.text);
  else dl->AddTriangleFilled(ImVec2(cx - r - 1, cy - r * 0.6f), ImVec2(cx + r + 1, cy - r * 0.6f), ImVec2(cx, cy + r * 0.8f), P.text);
  (void)a;
  return click;
}

// One stepper row: the label, the value in a sunken field right after it, the arrows at the box's
// right edge (down, up). `w` is the row's width, `labelW` the label column's.
int stepper(App& a, const char* id, const char* label, const std::string& value, ImVec2 p0, float w, float h, const char* hint, float labelW = -1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  dl->AddText(a.fontRegular, fs, ImVec2(p0.x, std::round(p0.y + (h - fs) * 0.5f)), P.textDim, i18n::tr(label));
  const float bw = std::round(h * 1.35f);
  const float x2 = p0.x + w - bw, x1 = x2 - bw;
  float vw = std::round(mono->CalcTextSizeA(fs, FLT_MAX, 0, "000.0").x + 8);
  const float lx = labelW >= 0 ? labelW : std::round(w * 0.42f);
  float v0 = std::min(p0.x + lx, x1 - 2 - vw);
  // A narrow box: the field gives way to the label (down to its value's own width), never over it.
  const float labelEnd = std::ceil(p0.x + a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(label)).x + 4);
  if (v0 < labelEnd) {
    v0 = labelEnd;
    vw = std::max(std::round(mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x + 6), x1 - 2 - v0);
  }
  int r = 0;
  ImGui::PushID(id);
  bevelBox(dl, ImVec2(v0, p0.y), ImVec2(v0 + vw, p0.y + h), true);
  const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x;
  dl->AddText(mono, fs, ImVec2(std::round(v0 + vw - 4 - tw), std::round(p0.y + (h - fs) * 0.5f)), P.hot, value.c_str());
  ImGui::SetCursorScreenPos(ImVec2(v0, p0.y));
  ImGui::InvisibleButton("##val", ImVec2(vw, h));
  if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) r = ImGui::GetIO().MouseWheel > 0 ? 1 : -1;
  if (hint) ui::Hint(hint);
  if (arrowButton(a, "##up", ImVec2(x1, p0.y), ImVec2(x1 + bw, p0.y + h), true, hint)) r = 1;
  if (arrowButton(a, "##dn", ImVec2(x2, p0.y), ImVec2(x2 + bw, p0.y + h), false, hint)) r = -1;
  ImGui::PopID();
  return r;
}

// A read-only value row (label, value in a sunken field at the right).
void readout(App& a, const char* label, const std::string& value, ImVec2 p0, float w, float h, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const float ty = std::round(p0.y + (h - fs) * 0.5f);
  dl->AddText(a.fontRegular, fs, ImVec2(p0.x, ty), P.textDim, i18n::tr(label));
  const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x;
  dl->AddText(mono, fs, ImVec2(std::round(p0.x + w - tw), ty), P.hot, value.c_str());
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(label, ImVec2(std::max(1.0f, w), h));
  if (hint) ui::Hint(hint);
}

// The entry at the song position (stopped outside the song: the first entry of the shown pattern).
int currentEntry(App& a, const std::vector<trk::OrderEntry>& ol) {
  // The entry under the song position when it plays the pattern shown (or while the song plays);
  // otherwise the pattern shown's first entry, so the list and the editor always agree.
  const Tick pos = static_cast<Tick>(a.engine.position());
  int at = -1;
  for (size_t i = 0; i < ol.size() && at < 0; ++i)
    if (pos >= ol[i].start && pos < ol[i].end) at = static_cast<int>(i);
  if (at >= 0 && (ol[static_cast<size_t>(at)].pattern == a.curPattern || (a.engine.playing() && a.engine.mode() == PlayMode::Song))) return at;
  for (size_t i = 0; i < ol.size(); ++i)
    if (ol[i].pattern == a.curPattern) return static_cast<int>(i);
  return at;
}

// The order list rows (both the header box and the part on its own).
void orderRows(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const float rowH = std::round(fs + 1);  // tight rows: the header box shows three or more
  const std::vector<trk::OrderEntry> ol = trk::orderList(s);
  if (ol.empty()) {
    const bool other = std::any_of(s.playlist.begin(), s.playlist.end(), [](const Clip& c) { return c.kind != ClipKind::Pattern; });
    // Wrapped: the box is narrow.
    dl->AddText(a.fontRegular, fs, ImVec2(p0.x + 4, p0.y + 2), P.textFaint,
                i18n::tr(other ? "No pattern clips. Automation clips are on the Song page." : "No pattern clips in the song yet"), nullptr,
                p1.x - p0.x - 8);
    return;
  }
  const int current = currentEntry(a, ol);
  const int vis = std::max(1, static_cast<int>((p1.y - p0.y) / rowH));
  const int first = std::clamp(std::max(0, current) - vis / 2, 0, std::max(0, static_cast<int>(ol.size()) - vis));
  dl->PushClipRect(p0, p1, true);
  for (int k = 0; k < vis && first + k < static_cast<int>(ol.size()); ++k) {
    const int i = first + k;
    const trk::OrderEntry& e = ol[static_cast<size_t>(i)];
    const float y = p0.y + static_cast<float>(k) * rowH;
    if (i == current) dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p1.x, y + rowH), theme::withAlpha(P.hot, 0.85f));
    const ImU32 col = i == current ? theme::contrastText(P.hot) : P.text;
    const Pattern* pat = s.pattern(e.pattern);
    // T-13: an audio part (no pattern plays there) shows "AU" and its top clip's channel; a pattern
    // entry with audio under it is marked with a "~" after its number.
    const bool audioPart = e.pattern == kNoId;
    std::string name = pat ? pat->name : std::string();
    if (audioPart && !e.audio.empty())
      if (const Clip* c = s.clip(e.audio.front()))
        if (const Channel* ch = s.channel(c->source)) name = ch->name;
    char line[96];
    std::snprintf(line, sizeof line, "%s  %s%s", trk::hex2(i).c_str(), audioPart ? "AU" : trk::hex2(trk::patternNumber(s, e.pattern)).c_str(),
                  e.patterns > 1 ? "+" : !audioPart && !e.audio.empty() ? "~" : " ");
    dl->AddText(mono, fs, ImVec2(p0.x + 4, std::round(y + (rowH - fs) * 0.5f)), col, line);
    const float lw = mono->CalcTextSizeA(fs, FLT_MAX, 0, line).x;
    if (!name.empty())
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(p0.x + 4 + lw + 6, std::round(y + (rowH - fs) * 0.5f)), p1.x - (p0.x + 4 + lw + 10),
                          i == current ? col : P.textDim, name.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##entry", ImVec2(std::max(1.0f, p1.x - p0.x), rowH))) {
      if (!audioPart) a.selectPattern(e.pattern);
      a.engine.setPosition(e.start, true);
    }
    ImGui::PopID();
    ui::Hint(audioPart ? "Only audio clips play here (no pattern) \xC2\xB7 click: go there \xC2\xB7 the Song page edits them"
             : e.patterns > 1 ? "Several patterns play here (the top track's is shown) \xC2\xB7 click: go there"
             : !e.audio.empty() ? "A part of the song with audio clips under its pattern (~) \xC2\xB7 click: go there and show its pattern"
                                : "A part of the song (cut where clips start and end) \xC2\xB7 click: go there and show its pattern");
  }
  // The free entries after the song's end: their numbers, faint (the list keeps its rows filled).
  for (int i = static_cast<int>(ol.size()); i - first < vis; ++i) {
    const float y = p0.y + static_cast<float>(i - first) * rowH;
    dl->AddText(mono, fs, ImVec2(p0.x + 4, std::round(y + (rowH - fs) * 0.5f)), theme::withAlpha(P.textFaint, 0.6f), trk::hex2(i).c_str());
  }
  dl->PopClipRect();
}

// The page strip: the Gridline pages the classic tracker has no screen for. Instrument, Sample,
// Files, Settings and Help are switched from the transport column (one control each).
struct Page {
  const char* label;
  const char* window;
  const char* hint;
};
const Page kPages[] = {
    {i18n::N_("Pattern"), "Pattern editor", i18n::N_("The pattern in rows (Esc: edit mode, F1 to F8: octave)")},
    {i18n::N_("Song"), "Playlist", i18n::N_("The arrangement, a row per bar")},
    {i18n::N_("Roll"), "Piano roll", i18n::N_("The same notes in the piano roll")},
    {i18n::N_("Steps"), "Channel rack", i18n::N_("The channels and their steps")},
    {i18n::N_("Mix"), "Mixer", i18n::N_("The mixer")},
    {i18n::N_("Pads"), "Live pads", i18n::N_("Clips launched by hand")},
};

bool windowFront(App& a, const char* window) {
  const bool* f = layoutui::windowOpenFlag(a, window);
  if (!f || !*f) return false;
  // A page docked with others is lit only while it is the tab in front.
  const ImGuiWindow* w = ImGui::FindWindowByName((std::string("###") + window).c_str());  // "Label###English"
  return !w || !w->DockIsActive || !w->DockNode || w->DockNode->VisibleWindow == w;
}

bool pageOn(App& a, const Page& pg) { return windowFront(a, pg.window); }

// The header's wordmark, a band filling the slot [p0, p1]: the whole slot in the fixed Gridline
// brand orange (255, 181, 71; never the chosen accent or theme), lit by a one pixel line along the
// top and shaded by one along the bottom (both from that orange), so it reads as a raised bar. At
// the left, the Tile mark inverted (the fixed brand dark (38, 44, 48), its cut-outs in the orange),
// square, as tall as the slot less a 3 px margin (the same margin at the left).
// Then "GRIDLINE TRACKER" in capitals, JetBrains Mono Bold, in the same dark colour and no
// shadow: the largest size whose cap ink fits the height within the margins and whose line fits
// the width, with the letters spread evenly (the space counts as a letter, at most a third of a
// letter apart) so the line runs from 5 px after the mark to the right margin, or sits centred
// between them in a wide band. Everything sits on device pixels, so crisp at any
// scale; a slot squeezed narrow shrinks the text, which stays inside the bar.
void logoLine(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const float dpp = pixelgrid::frameScale();
  p0 = ImVec2(pixelgrid::edge(p0.x, dpp), pixelgrid::edge(p0.y, dpp));
  p1 = ImVec2(pixelgrid::edge(p1.x, dpp), pixelgrid::edge(p1.y, dpp));
  const float px = pixelgrid::whole(1.0f, dpp);
  const ImU32 brand = IM_COL32(255, 181, 71, 255);  // the Gridline brand orange and dark: fixed, whatever accent or theme is chosen
  const ImU32 dark = IM_COL32(38, 44, 48, 255);
  dl->AddRectFilled(p0, p1, brand);
  dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + px), theme::mix(brand, IM_COL32(255, 255, 255, 255), 0.45f));
  dl->AddRectFilled(ImVec2(p0.x, p1.y - px), p1, theme::mix(brand, IM_COL32(0, 0, 0, 255), 0.30f));
  const float m = pixelgrid::whole(3.0f, dpp);
  const float mk = pixelgrid::edge(p1.y - p0.y - m * 2, dpp);
  if (mk < 6 || p1.x - p0.x < mk + m * 3) return;  // a band squeezed too small for any wordmark
  theme::drawMark(dl, ImVec2(p0.x + m, p0.y + m), mk, dark, brand);
  // The text, in capitals (the second word translated, then upper-cased).
  std::string text = std::string("GRIDLINE ") + i18n::tr("tracker");
  for (char& c : text)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  ImFont* font = a.fontMonoBold ? a.fontMonoBold : monoFont(a);
  const float x0 = p0.x + m + mk + pixelgrid::whole(5.0f, dpp), x1 = p1.x - m;
  const float availW = x1 - x0, availH = p1.y - p0.y - m * 2;
  if (availW < 10) return;
  const int glyphs = static_cast<int>(text.size());
  auto adv = [](ImFontBaked* fb, char c) { return fb->GetCharAdvance(static_cast<ImWchar>(static_cast<unsigned char>(c))); };
  auto find = [](ImFontBaked* fb, char c) { return fb->FindGlyph(static_cast<ImWchar>(static_cast<unsigned char>(c))); };
  // At a size: the cap ink's top and bottom below the pen's top ("H"), and the ink's width (the
  // first glyph's left bearing and the last one's right side left out).
  auto capInk = [&](float fs, float* top, float* bottom) {
    const ImFontGlyph* g = font->GetFontBaked(fs)->FindGlyph('H');
    *top = g ? g->Y0 : fs * 0.2f;
    *bottom = g ? g->Y1 : fs * 0.9f;
  };
  auto inkW = [&](float fs) {
    ImFontBaked* fb = font->GetFontBaked(fs);
    float w = 0;
    for (char c : text) w += adv(fb, c);
    const ImFontGlyph* f = find(fb, text.front());
    const ImFontGlyph* l = find(fb, text.back());
    return w - (f ? f->X0 : 0.0f) - (l ? adv(fb, text.back()) - l->X1 : 0.0f);
  };
  float fs = std::floor(availH * 1.6f), top = 0, bottom = 0;
  capInk(fs, &top, &bottom);
  while (fs > 6 && (bottom - top > availH || inkW(fs) > availW)) capInk(fs -= 1, &top, &bottom);
  // The spare width spread evenly between the glyphs: the last glyph's ink ends at the margin.
  ImFontBaked* fb = font->GetFontBaked(fs);
  // T-22: the spread stops at a third of a letter; a wider band centres the line in it instead of
  // letter-spacing it across.
  const float track = std::min(std::max(0.0f, (availW - inkW(fs)) / static_cast<float>(std::max(1, glyphs - 1))), adv(fb, 'G') * 0.34f);
  const float lineW = inkW(fs) + track * static_cast<float>(std::max(0, glyphs - 1));
  const float y = pixelgrid::edge(p0.y + m + (availH - (bottom - top)) * 0.5f - top, dpp);
  float x = std::round(x0 + std::max(0.0f, (availW - lineW) * 0.5f));
  if (const ImFontGlyph* f = find(fb, text.front())) x -= f->X0;  // the first glyph's ink starts at x0
  for (char c : text) {
    const char one[2] = {c, 0};
    if (c != ' ') dl->AddText(font, fs, ImVec2(pixelgrid::edge(x, dpp), y), dark, one);
    x += adv(fb, c) + track;
  }
}

// What the scope box shows: the channel scopes, or a tool swapped in (one at a time).
enum class ScopeTool { Scopes, Transpose, CleanUp };
ScopeTool gTool = ScopeTool::Scopes;

void scopes(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const int n = static_cast<int>(s.channels.size());
  if (n == 0) {
    dl->AddText(a.fontRegular, fs, ImVec2(p0.x + 6, p0.y + 4), P.textFaint, i18n::tr("No channels"));
    return;
  }
  const float W = p1.x - p0.x, H = p1.y - p0.y;
  // As many scopes as stay readable (a name's few letters each); more channels: the page of them
  // holding the selected channel.
  const int all = n;
  const int rows = all > 4 ? 2 : 1;
  const int maxCols = std::max(1, static_cast<int>(W / std::max(24.0f, fs * 3.2f)));
  const int perPage = rows * maxCols;
  const int sel = std::max(0, s.channelIndex(a.selChannel));
  const int first = all > perPage ? (sel / perPage) * perPage : 0;
  const int shown = std::min(all - first, perPage);
  const int cols = std::max(1, std::min(maxCols, (shown + rows - 1) / rows));
  const float cw = std::floor((W - static_cast<float>(cols - 1)) / static_cast<float>(cols));
  const float ch = std::floor((H - static_cast<float>(rows - 1)) / static_cast<float>(rows));
  static ops::ChannelSolo solo;
  // Each channel's level over the last moments (its mixer track's peak, read once a frame): the
  // scope draws it as a trace scrolling right to left, mirrored about its centre line.
  constexpr int kHist = 96;
  struct Trace {
    std::array<float, kHist> v{};
    int head = 0;
  };
  static std::unordered_map<ObjectId, Trace> traces;
  static int tracedFrame = -1;
  const bool step = tracedFrame != ImGui::GetFrameCount();
  tracedFrame = ImGui::GetFrameCount();
  for (int i = 0; i < shown; ++i) {
    const Channel& c = s.channels[static_cast<size_t>(first + i)];
    const float cx = p0.x + static_cast<float>(i % cols) * (cw + 1), cy = p0.y + static_cast<float>(i / cols) * (ch + 1);
    const ImVec2 c0(cx, cy), c1(i % cols == cols - 1 ? p1.x : cx + cw, i / cols == rows - 1 ? p1.y : cy + ch);
    bevelBox(dl, c0, c1, true);
    const float lvl = a.engine.playing() ? std::clamp(mixmeters::get(a, c.mixerTrack).peak(), 0.0f, 1.0f) : 0.0f;
    Trace& tr = traces[c.id];
    if (step) tr.v[static_cast<size_t>(tr.head = (tr.head + 1) % kHist)] = lvl;
    const float mid = std::round((c0.y + fs + 2 + c1.y) * 0.5f);
    const float amp = std::max(1.0f, (c1.y - c0.y - fs - 8) * 0.5f);
    if (c.muted) {  // a muted channel is crossed out
      dl->AddLine(ImVec2(c0.x + 3, c0.y + fs + 4), ImVec2(c1.x - 3, c1.y - 3), theme::withAlpha(P.textDim, 0.6f));
      dl->AddLine(ImVec2(c0.x + 3, c1.y - 3), ImVec2(c1.x - 3, c0.y + fs + 4), theme::withAlpha(P.textDim, 0.6f));
    } else {
      dl->AddRectFilled(ImVec2(c0.x + 3, mid), ImVec2(c1.x - 3, mid + 1), theme::withAlpha(P.led, 0.45f));
      // The trace: newest at the right edge, one column per two pixels.
      const float x0 = c0.x + 3, x1 = c1.x - 3;
      const int cols2 = std::max(1, static_cast<int>((x1 - x0) / 2));
      for (int k = 0; k < cols2; ++k) {
        const int age = (cols2 - 1 - k) * kHist / cols2;
        const float v = tr.v[static_cast<size_t>((tr.head - age + kHist) % kHist)];
        if (v <= 0.001f) continue;
        const float hh = std::max(1.0f, std::round(amp * std::sqrt(v)));
        const float x = x0 + static_cast<float>(k) * 2;
        dl->AddRectFilled(ImVec2(x, mid - hh), ImVec2(x + 1, mid + hh + 1), theme::withAlpha(P.led, 0.35f + 0.55f * v));
      }
    }
    char label[16];
    std::snprintf(label, sizeof label, "%d", first + i + 1);
    dl->AddText(mono, fs, ImVec2(c0.x + 4, c0.y + 2), c.muted ? P.textFaint : P.text, label);
    const float lw = mono->CalcTextSizeA(fs, FLT_MAX, 0, label).x;
    float nx = c0.x + 10 + lw;
    if (!c.muted && ops::channelSoloed(s, c.id, solo)) {
      dl->AddText(a.fontRegular, fs * 0.85f, ImVec2(nx, c0.y + 3), P.solo, i18n::tr("solo"));
      nx += a.fontRegular->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, i18n::tr("solo")).x + 6;
    }
    // The channel's name, so a scope says whose it is.
    if (c1.x - 4 - nx > 12) theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, c0.y + 2), c1.x - 4 - nx, c.muted ? P.textFaint : P.textDim, c.name.c_str());
    ImGui::SetCursorScreenPos(c0);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##scope", ImVec2(std::max(1.0f, c1.x - c0.x), std::max(1.0f, c1.y - c0.y))))
      a.doc.setParam(channelParam(c.id, ChannelParam::Mute), c.muted ? 0 : 1, false);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ops::toggleChannelSolo(a.doc, c.id, solo);
    ImGui::PopID();
    std::string tip = c.name + " \xC2\xB7 " + i18n::tr("click: mute or unmute \xC2\xB7 right-click: solo");
    ui::Hint(tip.c_str());
  }
}

void transposeTool(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float rows = 4, gap = 2;
  const float rh = std::floor((p1.y - p0.y - 4 - gap * (rows - 1)) / rows);
  dl->AddText(a.fontBold ? a.fontBold : a.fontRegular, fs, ImVec2(p0.x + 6, std::round(p0.y + 2 + (rh - fs) * 0.5f)), P.text, i18n::tr("Transpose"));
  struct Scope {
    const char* label;
    TransposeScope scope;
    const char* hint;
  };
  const Scope sc[] = {{i18n::N_("Track"), TransposeScope::Track, i18n::N_("The cursor's track")},
                      {i18n::N_("Pattern"), TransposeScope::Pattern, i18n::N_("Every track of the pattern")},
                      {i18n::N_("Block"), TransposeScope::Block, i18n::N_("The marked block (or the cursor's row)")}};
  const float labelW = std::round((p1.x - p0.x) * 0.26f);
  const float bw = std::floor((p1.x - p0.x - 8 - labelW - gap * 3) / 4);
  const int steps[] = {-12, -1, 1, 12};
  const char* names[] = {"-12", "-1", "+1", "+12"};
  for (int r = 0; r < 3; ++r) {
    const float y = std::round(p0.y + 2 + static_cast<float>(r + 1) * (rh + gap));
    dl->AddText(a.fontRegular, fs, ImVec2(p0.x + 6, std::round(y + (rh - fs) * 0.5f)), P.textDim, i18n::tr(sc[r].label));
    for (int k = 0; k < 4; ++k) {
      const float x = std::round(p0.x + 4 + labelW + static_cast<float>(k) * (bw + gap));
      ImGui::PushID(r * 8 + k);
      if (bevelButton(a, "##tr", names[k], ImVec2(x, y), ImVec2(k == 3 ? p1.x - 4 : x + bw, y + rh), false, sc[r].hint)) transposeScope(a, sc[r].scope, steps[k]);
      ImGui::PopID();
    }
  }
}

void cleanUpTool(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float gap = 2, rh = std::floor((p1.y - p0.y - 4 - gap * 3) / 4);
  dl->AddText(a.fontBold ? a.fontBold : a.fontRegular, fs, ImVec2(p0.x + 6, std::round(p0.y + 2 + (rh - fs) * 0.5f)), P.text, i18n::tr("Clean up"));
  const float x0 = p0.x + 4, x1 = p1.x - 4;
  float y = std::round(p0.y + 2 + rh + gap);
  if (bevelButton(a, "##cupat", "Delete unused patterns", ImVec2(x0, y), ImVec2(x1, y + rh), false, "Patterns no clip plays")) {
    const int n = ops::deleteUnusedPatterns(a.doc);
    a.notify(n ? std::to_string(n) + " " + i18n::tr("unused patterns deleted") : std::string(i18n::tr("No unused patterns")));
  }
  y += rh + gap;
  if (bevelButton(a, "##cumix", "Trim unused mixer tracks", ImVec2(x0, y), ImVec2(x1, y + rh), false, "Empty mixer tracks at the end")) {
    const int n = ops::trimUnusedMixerTracks(a.doc);
    a.notify(n ? std::to_string(n) + " " + i18n::tr("unused mixer tracks removed") : std::string(i18n::tr("No unused mixer tracks at the end")));
  }
}

// What the whole header shows: the boxes, or a screen swapped in (Help).
enum class Swap { None, Help, Disk, Config };
Swap gSwap = Swap::None;
int gHelpSubject = 0;

struct HelpRow {
  const char* keys;
  const char* what;
};
// The pattern editor's keys in the Tracker key set (Gridline's own wording).
const HelpRow kPatternKeys[] = {
    {"Z S X D C ... /", i18n::N_("Notes in the octave shown (the row above: the black keys)")},
    {"Q 2 W 3 E ... P", i18n::N_("Notes an octave up")},
    {"F1 ... F8", i18n::N_("The octave")},
    {"Caps Lock, < >", i18n::N_("A note off (the note above ends here)")},
    {"Esc", i18n::N_("Edit mode on or off (off: the keys only play)")},
    {"`  Shift+`", i18n::N_("Edit step one more, one fewer")},
    {"Ctrl+0 ... 9", i18n::N_("Edit step")},
    {"Up Down PgUp PgDn", i18n::N_("Rows (16 at a time with the page keys)")},
    {"Home End", i18n::N_("The first, the last row")},
    {"Left Right", i18n::N_("The fields of a cell")},
    {"Tab  Shift+Tab", i18n::N_("The next, the previous track")},
    {"Alt+Q ... I, Alt+A ... K", i18n::N_("Tracks 1 to 8, 9 to 16")},
    {"Ctrl+Shift+Up Down", i18n::N_("The previous, the next instrument")},
    {"F9 ... F12", i18n::N_("Jump rows (Shift: keep the cursor's row there, Ctrl: play the pattern from it)")},
    {"Shift or Alt+arrows", i18n::N_("Mark a block")},
    {"Alt+C", i18n::N_("Mark the track")},
    {"Ctrl+C X V, Alt+F4 F3 F5", i18n::N_("Copy, cut, paste the block")},
    {"Shift+F3 F4 F5", i18n::N_("Cut, copy, paste the track")},
    {"Ctrl+F3 F4 F5", i18n::N_("Cut, copy, paste the pattern")},
    {"Delete", i18n::N_("Clears the field (the block when one is marked)")},
    {"Shift+Delete", i18n::N_("Clears the cell")},
    {"Ctrl+Delete  Alt+Delete", i18n::N_("Clears volume and effect, the effect")},
    {"Insert, Ctrl+Enter", i18n::N_("A row in (Shift: in every track)")},
    {"Backspace", i18n::N_("The row above out (Shift: in every track)")},
    {"Ctrl+Up Down", i18n::N_("The block a semitone up, down")},
    {"Shift+F1 F2", i18n::N_("The track a semitone down, up (F7 F8 the same)")},
    {"Ctrl+F1 F2", i18n::N_("The pattern a semitone down, up")},
    {"Alt+F1 F2", i18n::N_("The block a semitone down, up")},
    {"Ctrl+Left Right", i18n::N_("The previous, the next pattern")},
    {"Space", i18n::N_("Play, stop")},
};

void helpScreen(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const float leftW = std::round(std::max(150.0f, fs * 11));
  bevelBox(dl, p0, ImVec2(p0.x + leftW, p1.y));
  const float bh = std::round(fs + 9);
  const char* subjects[] = {i18n::N_("Pattern keys"), i18n::N_("Effect letters"), i18n::N_("Screen keys")};
  for (int i = 0; i < 3; ++i) {
    const float y = p0.y + 4 + static_cast<float>(i) * (bh + 2);
    ImGui::PushID(i);
    if (bevelButton(a, "##subj", subjects[i], ImVec2(p0.x + 4, y), ImVec2(p0.x + leftW - 4, y + bh), gHelpSubject == i)) gHelpSubject = i;
    ImGui::PopID();
  }
  {
    const float y = p0.y + 4 + 3 * (bh + 2);
    if (bevelButton(a, "##allkeys", "Every shortcut", ImVec2(p0.x + 4, y), ImVec2(p0.x + leftW - 4, y + bh), a.showShortcuts,
                    "Every command and its keys, from the shortcut set in use"))
      a.showShortcuts = !a.showShortcuts;
  }
  if (bevelButton(a, "##helpexit", "Exit", ImVec2(p0.x + 4, p1.y - 4 - bh), ImVec2(p0.x + leftW - 4, p1.y - 4), false, "Back to the header")) gSwap = Swap::None;
  // The text pane.
  const ImVec2 t0(p0.x + leftW + 2, p0.y), t1 = p1;
  bevelBox(dl, t0, t1, true);
  ImGui::SetCursorScreenPos(ImVec2(t0.x + 1, t0.y + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  if (ImGui::BeginChild("##helptext", ImVec2(std::max(1.0f, t1.x - t0.x - 2), std::max(1.0f, t1.y - t0.y - 2)), ImGuiChildFlags_AlwaysUseWindowPadding)) {
    ImDrawList* cl = ImGui::GetWindowDrawList();
    const float rh = std::round(fs + 4);
    const ImVec2 c = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    float y = c.y;
    if (gHelpSubject == 0) {
      float kw = 0;
      for (const HelpRow& r : kPatternKeys) kw = std::max(kw, mono->CalcTextSizeA(fs, FLT_MAX, 0, r.keys).x);
      kw += 16;
      // The rows flow into columns side by side as the band is wide, so more of them show at once
      // (two or three columns at a usual size; one, scrolling, when narrow).
      const int n = static_cast<int>(sizeof kPatternKeys / sizeof kPatternKeys[0]);
      const float visH = ImGui::GetContentRegionAvail().y;
      const int perCol = std::max(1, static_cast<int>(visH / rh));
      const int want = (n + perCol - 1) / perCol;
      const int ncol = std::max(1, std::min(want, static_cast<int>(w / std::max(1.0f, kw + fs * 18))));
      const int rowsPer = (n + ncol - 1) / ncol;
      const float colW2 = std::floor(w / static_cast<float>(ncol));
      for (int i = 0; i < n; ++i) {
        const HelpRow& r = kPatternKeys[i];
        const float cx = c.x + static_cast<float>(i / rowsPer) * colW2, cy = y + static_cast<float>(i % rowsPer) * rh;
        cl->AddText(mono, fs, ImVec2(cx, cy + 2), P.hot, r.keys);
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(cx + kw, cy + 2), colW2 - kw - 8, P.text, i18n::tr(r.what));
      }
      y += static_cast<float>(rowsPer) * rh;
    } else if (gHelpSubject == 2) {
      // The screens and their keys, read from the shortcut set in use (a key changed in the
      // shortcut editor shows here as it is).
      static const char* const ids[] = {"view.patternEditor", "view.playlist",  "view.pianoRoll",       "view.channelRack", "view.mixer",
                                        "view.pads",          "view.channelSettings", "view.audioEditor", "view.files",       "view.settings",
                                        "help.trackerKeys",   "view.headerBand", "view.browser",        "view.pluginPicker", "help.shortcuts"};
      float kw = 0;
      for (const char* id : ids) kw = std::max(kw, mono->CalcTextSizeA(fs, FLT_MAX, 0, keys::label(id)).x);
      kw += 16;
      for (const char* id : ids) {
        const keys::Command* cmd = keys::findCommand(id);
        if (!cmd) continue;
        const char* k = keys::label(id);
        cl->AddText(mono, fs, ImVec2(c.x, y + 2), *k ? P.hot : P.textFaint, *k ? k : "-");
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(c.x + kw, y + 2), w - kw, P.text, i18n::tr(cmd->label));
        y += rh;
      }
    } else {
      cl->AddText(a.fontRegular, fs, ImVec2(c.x, y + 2), P.textDim,
                  i18n::tr("The effect field shows a note's own properties as a letter and two hex digits:"));
      y += rh + 4;
      const int typical[] = {0, 0, 0x0A, 0x80, 0x80, 0x40};
      for (int i = 0; trk::kEffectLetters[i]; ++i) {
        const std::string h = trk::effectHelp(trk::kEffectLetters[i], typical[i]);
        const std::string code = std::string(1, trk::kEffectLetters[i]) + "xx";
        cl->AddText(mono, fs, ImVec2(c.x, y + 2), P.hot, code.c_str());
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(c.x + fs * 4, y + 2), w - fs * 4, P.text, h.c_str());
        y += rh;
      }
      y += 4;
      cl->AddText(a.fontRegular, fs, ImVec2(c.x, y + 2), P.textDim, i18n::tr("Type the letter, then the digits; Delete puts the property back."));
      y += rh;
    }
    ImGui::SetCursorScreenPos(ImVec2(c.x, y));
    ImGui::Dummy(ImVec2(1, 1));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

// Files (the header's left part): the project commands at the left, the recent projects and the
// project's folder in a sunken list, drawn by the browser itself (browserProjectFiles), so the
// files, filters, open, preview, drag and menus are the ones every layout has.
void diskScreen(App& a, ImVec2 p0, ImVec2 p1) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float leftW = std::round(std::max(120.0f, fs * 9));
  bevelBox(dl, p0, ImVec2(p0.x + leftW, p1.y));
  struct Cmd {
    const char* id;
    const char* label;
    const char* hint;
  };
  const Cmd cmds[] = {{"##dsave", i18n::N_("Save"), i18n::N_("Saves the project (Ctrl+S)")},
                      {"##dsaveas", i18n::N_("Save as"), i18n::N_("Saves under a new name or format")},
                      {"##dopen", i18n::N_("Open"), i18n::N_("Opens a project or a song file (Ctrl+O)")},
                      {"##dnew", i18n::N_("New"), i18n::N_("A new project (asks to save first)")},
                      {"##dexport", i18n::N_("Export"), i18n::N_("Renders the song or the pattern to an audio file")},
                      {"##dlib", i18n::N_("Library"), i18n::N_("Samples, presets and plugins (the Library page)")}};
  const int n = static_cast<int>(sizeof cmds / sizeof cmds[0]);
  const float bh = std::floor((p1.y - p0.y - 4 - static_cast<float>(n - 1)) / static_cast<float>(n));
  for (int i = 0; i < n; ++i) {
    const float y = std::round(p0.y + 2 + static_cast<float>(i) * (bh + 1));
    const bool on = i == 5 && windowFront(a, "Browser");
    if (!bevelButton(a, cmds[i].id, cmds[i].label, ImVec2(p0.x + 2, y), ImVec2(p0.x + leftW - 2, i == n - 1 ? p1.y - 2 : y + bh), on, cmds[i].hint)) continue;
    switch (i) {
      case 0: a.save(); break;
      case 1: a.saveAs(); break;
      case 2: a.openDialog(); break;
      case 3: a.guardUnsaved([&a]() { a.newProject(); }); break;
      case 4: a.showExport = true; break;
      default:
        if (on) a.showBrowser = false;
        else showPage(a, "Browser");
        break;
    }
  }
  // The list: the browser's own recent files and project folder (same rows, filters, open, drag, menus).
  const ImVec2 l0(p0.x + leftW + 2, p0.y), l1 = p1;
  bevelBox(dl, l0, l1, true);
  ImGui::SetCursorScreenPos(ImVec2(l0.x + 1, l0.y + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, 2));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  if (ImGui::BeginChild("##disklist", ImVec2(std::max(1.0f, l1.x - l0.x - 2), std::max(1.0f, l1.y - l0.y - 2)), ImGuiChildFlags_AlwaysUseWindowPadding))
    browserProjectFiles(a);
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

// The settings screen: the pattern editor's own options in the header band (the band swaps to it,
// the pattern stays in view below, as the classic tracker's config screen), every other setting
// one click away in the Settings window.
void configScreen(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float leftW = std::round(std::max(150.0f, fs * 11));
  bevelBox(dl, p0, ImVec2(p0.x + leftW, p1.y));
  const float bh = std::round(fs + 9);
  if (bevelButton(a, "##cfgall", "All settings", ImVec2(p0.x + 4, p0.y + 4), ImVec2(p0.x + leftW - 4, p0.y + 4 + bh), a.showSettings,
                  "Every setting: audio, interface, keys, plugins (the Settings window)"))
    a.showSettings = !a.showSettings;
  if (bevelButton(a, "##cfgexit", "Exit", ImVec2(p0.x + 4, p1.y - 4 - bh), ImVec2(p0.x + leftW - 4, p1.y - 4), false, "Back to the header")) gSwap = Swap::None;
  const ImVec2 t0(p0.x + leftW + 2, p0.y), t1 = p1;
  bevelBox(dl, t0, t1);
  // Two columns: the pattern's columns and numbers (toggles), then the cursor's steppers.
  const float pad = 6, colW = std::floor((t1.x - t0.x - pad * 3) / 2);
  const float x0 = t0.x + pad, x1 = x0 + colW + pad;
  float y = t0.y + pad;
  dl->AddText(a.fontRegular, fs, ImVec2(x0, y), P.textDim, i18n::tr("Pattern editor"));
  dl->AddText(a.fontRegular, fs, ImVec2(x1, y), P.textDim, i18n::tr("Cursor"));
  y += fs + 6;
  struct Toggle {
    const char* id;
    const char* label;
    const char* key;
    const char* hint;
  };
  const Toggle toggles[] = {{"##cfgvol", i18n::N_("Volume column"), "tracker.volumeColumn", i18n::N_("Each note's volume beside it")},
                            {"##cfgpan", i18n::N_("Pan column"), "tracker.panColumn", i18n::N_("Each note's pan beside it")},
                            {"##cfgfx", i18n::N_("Effect column"), "tracker.effectColumn", i18n::N_("Each note's effect letter and value")}};
  float ty = y;
  for (const Toggle& t : toggles) {
    const bool on = a.settings.getBool(t.key);
    if (bevelButton(a, t.id, t.label, ImVec2(x0, ty), ImVec2(x0 + colW, ty + bh), on, t.hint)) a.settings.set(t.key, !on);
    ty += bh + 2;
  }
  const bool hex = a.settings.getString("tracker.rowBase") != "dec";
  if (bevelButton(a, "##cfghex", hex ? "Row numbers: hexadecimal" : "Row numbers: decimal", ImVec2(x0, ty), ImVec2(x0 + colW, ty + bh), false,
                  "Row numbers in hexadecimal (00 to 3F) or decimal (00 to 63)"))
    a.settings.set("tracker.rowBase", std::string(hex ? "dec" : "hex"));
  // The cursor's options.
  ty = y;
  if (bevelButton(a, "##cfgfollow", "Follow playback", ImVec2(x1, ty), ImVec2(x1 + colW, ty + bh), follow(), "The cursor follows the play row while playing"))
    setFollow(!follow());
  ty += bh + 2;
  if (const int d = stepper(a, "cfgstep", "Edit step", std::to_string(editStep()), ImVec2(x1, ty), colW, bh, "Rows the cursor moves after each note (Ctrl+0 to 9)"))
    setEditStep(editStep() + d);
  ty += bh + 2;
  if (const int d = stepper(a, "cfgoct", "Octave", std::to_string(a.typing.octave), ImVec2(x1, ty), colW, bh, "The note keys' octave (F1 to F8)"))
    a.typing.octave = std::clamp(a.typing.octave + d, 0, 8);
}

void pageStrip(App& a, ImVec2 o, float W, float py0, float stripH);

void header(App& a) {
  const auto& P = pal();
  if (gSwap == Swap::Help || gSwap == Swap::Config) {
    const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    const float stripH = std::round(ImGui::GetFontSize() + 9);
    const float H = std::max(120.0f, std::floor(av.y - stripH - 2));
    if (gSwap == Swap::Help) helpScreen(a, o, ImVec2(o.x + av.x, o.y + H));
    else configScreen(a, o, ImVec2(o.x + av.x, o.y + H));
    pageStrip(a, o, av.x, o.y + H + 2, stripH);
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + av.y));
    ImGui::Dummy(ImVec2(1, 0));
    return;
  }
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const float stripH = std::round(fs + 9);
  const float H = std::max(120.0f, std::floor(av.y - stripH - 2));
  const float W = av.x;
  const float k = H / 173.0f;  // the classic band's proportions at this height
  // Columns: the button columns and bank strip keep their scaled width; the left block and the
  // lists share what is left.
  // A button column is as wide as its longest label needs (translated), never cut.
  float labelW = 0;
  for (const char* l : {"About Gridline", "Metronome", "New", "Clean up", "Header band", "Transpose", "MIDI", "Sample tools", "Add track", "Remove track", "Play song",
                        "Play pattern", "Stop", "Record song", "Record pattern", "Files", "Channel settings", "Audio editor", "Settings", "Tracker help"})
    labelW = std::max(labelW, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(l)).x);
  const float colW = std::max(std::round(65 * k), std::ceil(labelW + 16)), bankW = std::round(45 * k);
  const float rest = std::max(200.0f, W - colW * 2 - bankW);
  const float extra = std::max(0.0f, rest - (291 + 166) * k);
  const float leftW = std::floor(std::min(rest * 0.64f, 291 * k + extra * 0.5f));
  const float listW = std::floor(W - leftW - colW * 2 - bankW);
  const float xL = o.x, xA = o.x + leftW, xB = xA + colW, xI = xB + colW, xK = xI + listW;
  const float y0 = o.y;
  const float topH = std::round(77 * k), stH = std::max(std::round(15 * k), std::round(fs + 4));  // the strip never shorter than its text
  const float yS = y0 + topH, yC = yS + stH, yE = y0 + H;
  const float pad = std::round(4 * k);
  const float rowH = std::max(14.0f, std::round(11 * k));

  // ---- button columns: tools (A) and transport and screens (B), ten rows each ----
  const float bh10 = std::floor((H - 4 - 9) / 10.0f);
  auto colBox = [&](float x) { bevelBox(dl, ImVec2(x, y0), ImVec2(x + colW, yE)); };
  auto slot = [&](int i, float* y0o, float* y1o) {
    *y0o = std::round(y0 + 2 + static_cast<float>(i) * (bh10 + 1));
    *y1o = i == 9 ? yE - 2 : *y0o + bh10;
  };
  // Files: the header's left part (the boxes and the tool column) shows the files instead.
  if (gSwap == Swap::Disk) {
    diskScreen(a, ImVec2(xL, y0), ImVec2(xB, yE));
  } else {
  // ---- order box ----
  const float orderW = std::round(leftW * 112.0f / 291.0f);
  {
    bevelBox(dl, ImVec2(xL, y0), ImVec2(xL + orderW, yS));
    const float bw = std::round(orderW * 0.30f);
    const float lx1 = xL + orderW - pad - bw - 2;
    const float listB = yS - pad - rowH * 2 - 2;
    bevelBox(dl, ImVec2(xL + pad, y0 + pad), ImVec2(lx1, listB), true);
    orderRows(a, ImVec2(xL + pad + 1, y0 + pad + 1), ImVec2(lx1 - 1, listB - 1));
    // Insert the shown pattern at the current entry, or remove the current entry (playlist edits).
    const float bx = lx1 + 2, bh = std::floor((listB - y0 - pad - 4) / 3);
    const std::vector<trk::OrderEntry> ol = trk::orderList(s);
    const int cur = currentEntry(a, ol);
    auto told = [&](trk::OrderEdit r) {
      if (r == trk::OrderEdit::Spans) a.notify(i18n::tr("Its clips run into the next entries: change them on the Song page"));
      else if (r == trk::OrderEdit::OtherClips) a.notify(i18n::tr("Audio or automation clips start there: change them on the Song page"));
    };
    const float r1 = y0 + pad + bh, r2 = r1 + 2 + bh;
    if (bevelButton(a, "##oins", "Ins", ImVec2(bx, y0 + pad), ImVec2(bx + bw, r1), false,
                    "Inserts the shown pattern at the current entry (the song after it moves right)"))
      told(trk::orderInsert(a.doc, cur < 0 ? static_cast<int>(ol.size()) : cur, a.curPattern));
    if (cur < 0) ImGui::BeginDisabled();
    // The current entry's pattern, one up or down (the entry plays the next or previous pattern).
    const float hw = std::floor((bw - 1) / 2);
    int dp = 0;
    if (arrowButton(a, "##opup", ImVec2(bx, r1 + 2), ImVec2(bx + hw, r2), true, "The current entry plays the next pattern")) dp = 1;
    if (arrowButton(a, "##opdn", ImVec2(bx + hw + 1, r1 + 2), ImVec2(bx + bw, r2), false, "The current entry plays the previous pattern")) dp = -1;
    if (dp && cur >= 0 && ol[static_cast<size_t>(cur)].pattern == kNoId) {
      told(trk::OrderEdit::OtherClips);  // an audio part
    } else if (dp && cur >= 0) {
      const int pi = s.patternIndex(ol[static_cast<size_t>(cur)].pattern) + dp;
      if (pi >= 0 && pi < static_cast<int>(s.patterns.size())) told(trk::orderSetPattern(a.doc, cur, s.patterns[static_cast<size_t>(pi)].id));
    }
    if (bevelButton(a, "##odel", "Del", ImVec2(bx, r2 + 2), ImVec2(bx + bw, listB), false,
                    "Removes the current entry (its clips; the song after it moves left)"))
      told(trk::orderRemove(a.doc, cur));
    if (cur < 0) ImGui::EndDisabled();
    readout(a, "Length", trk::hex2(static_cast<int>(ol.size())), ImVec2(xL + pad, listB + 2), orderW - pad * 2, rowH, "Entries in the song (from the playlist's pattern clips; AU: a part where only audio plays)");
    // Loop to: the entry the song goes back to at its end (the song loop marker, saved and shared).
    int loopAt = -1;
    Tick loopPos = 0;
    for (const Marker& m : s.markers)
      if (m.action == MarkerAction::SongLoop)
        for (size_t i = 0; i < ol.size() && loopAt < 0; ++i)
          if (m.pos < ol[i].end) loopAt = static_cast<int>(i), loopPos = m.pos;
    if (const int d = stepper(a, "loop", "Loop to", loopAt < 0 ? std::string("--") : trk::hex2(loopAt), ImVec2(xL + pad, listB + 2 + rowH),
                              orderW - pad * 2, rowH, "The entry the song goes back to when it ends (none: it stops)")) {
      const int to = loopAt + d;
      if (to < 0 && loopAt >= 0) ops::addSongLoopMarker(a.doc, loopPos);  // placed where it is: removed
      else if (to >= 0 && to < static_cast<int>(ol.size())) ops::addSongLoopMarker(a.doc, ol[static_cast<size_t>(to)].start);
    }
  }
  // ---- logo slot, song box, pattern box ----
  const float xs = xL + orderW, midW = leftW - orderW;
  // The wordmark gives up height first when the band is short: the song and pattern boxes keep
  // three rows of text (a 1.5x or 2x interface in a small window).
  const float boxNeed = 3 * std::round(fs + 4) + 4 + pad * 2;
  const float logoH = std::round(std::clamp(topH - boxNeed, std::min(fs + 8, 32 * k), 32 * k));
  {
    bevelBox(dl, ImVec2(xs, y0), ImVec2(xs + midW, y0 + logoH));
    logoLine(a, dl, ImVec2(xs, y0), ImVec2(xs + midW, y0 + logoH));
    ImGui::SetCursorScreenPos(ImVec2(xs, y0));
    if (ImGui::InvisibleButton("##logo", ImVec2(midW, logoH))) a.home.showHome(a.now);
    ui::Hint("Home: recent projects and templates");
  }
  const float songW = std::round(midW * 94.0f / 179.0f), patW = midW - songW;
  const float by0 = y0 + logoH, bh = yS - by0;
  const float srh = std::max(14.0f, std::floor((bh - pad * 2 - 4) / 3));
  {
    bevelBox(dl, ImVec2(xs, by0), ImVec2(xs + songW, yS));
    const float rw = songW - pad * 2, lw = std::round(rw * 0.30f);
    float y = by0 + pad;
    char bpm[16];
    const bool whole = std::fabs(s.tempo - std::round(s.tempo)) < 0.05;
    std::snprintf(bpm, sizeof bpm, whole ? "%.0f" : "%.1f", s.tempo);
    if (const int d = stepper(a, "bpm", "Tempo", bpm, ImVec2(xs + pad, y), rw, srh, "The song tempo in beats per minute (Shift: steps of 0.1)", lw)) {
      const double t = ImGui::GetIO().KeyShift ? std::round(s.tempo * 10 + d) / 10.0 : std::round(s.tempo) + d;
      a.doc.setParam(songParam(SongParam::Tempo), std::clamp(t, 10.0, 522.0), true);
    }
    y += srh + 2;
    const Pattern* p = s.pattern(a.curPattern);
    const trk::Grid g = p ? trk::gridOf(s, *p) : trk::Grid{};
    readout(a, "Rows/beat", std::to_string(g.rowsPerBeat), ImVec2(xs + pad, y), rw, srh, "Rows per beat: the pattern's steps per beat");
    y += srh + 2;
    if (const int d = stepper(a, "step", "Step", std::to_string(editStep()), ImVec2(xs + pad, y), rw, srh, "Rows the cursor moves after each note (Ctrl+0 to 9)", lw))
      setEditStep(editStep() + d);
  }
  {
    const float xp = xs + songW;
    bevelBox(dl, ImVec2(xp, by0), ImVec2(xp + patW, yS));
    const float rw = patW - pad * 2, lw = std::round(rw * 0.30f);
    float y = by0 + pad;
    const Pattern* p = s.pattern(a.curPattern);
    const trk::Grid g = p ? trk::gridOf(s, *p) : trk::Grid{};
    if (const int d = stepper(a, "pat", "Pattern", p ? trk::hex2(trk::patternNumber(s, p->id)) : std::string("--"), ImVec2(xp + pad, y), rw, srh,
                              "The pattern shown (the piano roll and steps show it too)", lw)) {
      const int i = std::clamp(s.patternIndex(a.curPattern) + d, 0, std::max(0, static_cast<int>(s.patterns.size()) - 1));
      if (!s.patterns.empty()) a.selectPattern(s.patterns[static_cast<size_t>(i)].id);
    }
    y += srh + 2;
    if (const int d = stepper(a, "len", "Rows", p ? trk::rowText(g.rows, true) : std::string("--"), ImVec2(xp + pad, y), rw, srh,
                              "The pattern's length in rows (a bar at a time)", lw))
      if (p) a.doc.setPatternLength(p->id, std::max(1, p->bars + d), p->stepsPerBar);
    y += srh + 2;
    const float hw = std::floor((rw - 2) / 2);
    if (bevelButton(a, "##exp", "Expand", ImVec2(xp + pad, y), ImVec2(xp + pad + hw, y + srh), false, "Notes twice as far apart (the block, or the cursor's track)"))
      scaleBlock(a, true);
    if (bevelButton(a, "##shr", "Shrink", ImVec2(xp + pad + hw + 2, y), ImVec2(xp + patW - pad, y + srh), false, "Notes twice as close"))
      scaleBlock(a, false);
  }
  // ---- status strip: master level, octave, time (displays) ----
  {
    bevelBox(dl, ImVec2(xL, yS), ImVec2(xA, yC));
    const float ty = std::round(yS + (stH - fs) * 0.5f);
    // A display: the label, the value; the wheel steps it, a click acts. Returns the wheel step.
    // `right` draws it ending at x instead of starting there.
    float x = xL + pad + 2;
    auto display = [&](const char* id, const char* label, const std::string& value, const char* hint, bool* clicked, bool right = false) {
      const char* l = i18n::tr(label);
      const float lw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, l).x + 5;
      const float w = lw + mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x;
      const float x0 = std::round(right ? x - w : x);
      ImGui::SetCursorScreenPos(ImVec2(x0 - 2, yS + 1));
      const bool c = ImGui::InvisibleButton(id, ImVec2(w + 4, std::max(1.0f, stH - 2)));
      const bool hov = ImGui::IsItemHovered();
      if (hov) dl->AddRectFilled(ImVec2(x0 - 2, yS + 2), ImVec2(x0 + w + 2, yC - 2), theme::withAlpha(P.surface600, 0.5f));
      dl->AddText(a.fontRegular, fs, ImVec2(x0, ty), P.textDim, l);
      dl->AddText(mono, fs, ImVec2(x0 + lw, ty), P.hot, value.c_str());
      ui::Hint(hint);
      if (clicked) *clicked = c;
      x = right ? x0 - 14 : x0 + w + 14;
      return hov && ImGui::GetIO().MouseWheel != 0 ? (ImGui::GetIO().MouseWheel > 0 ? 1 : -1) : 0;
    };
    bool click = false;
    if (!s.mixer.empty()) {
      const ParamId volId = mixerParam(s.masterId(), MixerParam::Volume);
      const double vol = getParam(s, volId).value_or(0.8);
      char mv[16];
      std::snprintf(mv, sizeof mv, "%d%%", static_cast<int>(std::lround(vol * 100)));
      // A display: the master fader lives on the Mix page (one control each).
      display("##master", "Master", mv, "The master volume (click: the Mix page, where its fader is)", &click);
      if (click) showPage(a, "Mixer");
    }
    {
      const ParamId pit = songParam(SongParam::MasterPitch);
      const double v = getParam(s, pit).value_or(0.0);
      char txt[16];
      std::snprintf(txt, sizeof txt, "%+d", static_cast<int>(std::lround(v)));
      if (const int d = display("##pitch", "Pitch", txt, "The master pitch in semitones (wheel: a semitone \xC2\xB7 click: back to 0)", &click))
        a.doc.setParam(pit, std::clamp(std::round(v) + d, -12.0, 12.0), true);
      if (click && std::lround(v) != 0) a.doc.setParam(pit, 0.0, false);
    }
    display("##oct", "Octave", std::to_string(a.typing.octave), "The octave the note keys type (F1 to F8 set it)", nullptr);
    // The song position at the right end (right-click: the time format, as the position display elsewhere).
    x = xA - pad - 2;
    const std::string pos = menuparity::positionText(a, a.engine.position());
    display("##pos", "Position", pos, "The song position (right-click: its format)", nullptr, true);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##posfmt");
    if (ImGui::BeginPopup("##posfmt")) {
      menuparity::timeFormatMenu(a);
      ImGui::EndPopup();
    }
  }
  // ---- scope box (or the tool swapped in) ----
  {
    bevelBox(dl, ImVec2(xL, yC), ImVec2(xA, yE));
    const ImVec2 i0(xL + 3, yC + 3), i1(xA - 3, yE - 3);
    if (gTool == ScopeTool::Transpose) transposeTool(a, i0, i1);
    else if (gTool == ScopeTool::CleanUp) cleanUpTool(a, i0, i1);
    else scopes(a, i0, i1);
  }
  colBox(xA);
  {
    const float bx0 = xA + 2, bx1 = xA + colW - 2;
    auto b = [&](int i, const char* id, const char* label, const char* hint, bool on = false) {
      float t0, t1;
      slot(i, &t0, &t1);
      return bevelButton(a, id, label, ImVec2(bx0, t0), ImVec2(bx1, t1), on, hint);
    };
    // Labels as the menus word the same command (Help > About Gridline, Options > Metronome, File > New).
    if (b(0, "##about", "About Gridline", "The version, the credits and the licences", a.showAbout)) a.showAbout = !a.showAbout;
    if (b(1, "##metro", "Metronome", "The click while playing and recording", a.metronome)) a.metronome = !a.metronome;
    if (b(2, "##new", "New", "A new, empty project (asks to save first)")) a.guardUnsaved([&a]() { a.newProject(); });
    if (b(3, "##clean", "Clean up", "Clean-up tools in the scope box", gTool == ScopeTool::CleanUp))
      gTool = gTool == ScopeTool::CleanUp ? ScopeTool::Scopes : ScopeTool::CleanUp;
    if (b(4, "##ext", "Header band", "Hides this band: the pattern gets the room (the same button in the pattern bar brings it back)")) setExtended(a, true);
    if (b(5, "##trans", "Transpose", "Transpose tools in the scope box", gTool == ScopeTool::Transpose))
      gTool = gTool == ScopeTool::Transpose ? ScopeTool::Scopes : ScopeTool::Transpose;
    ImGui::BeginDisabled();
    b(6, "##iext", "MIDI", "The instrument's MIDI settings in the scope box (to come)");
    b(7, "##sext", "Sample tools", "Sample range tools in the scope box (to come)");
    ImGui::EndDisabled();
    if (b(8, "##addtr", "Add track", "A new track at the end (a sampler channel)")) {
      // At the end, as the classic tracker adds channels (the numbers of the others stay; one undo step).
      a.doc.beginGroup("add track");
      const ObjectId id = menus::addInstrumentPick(a, "gl.tracker");
      if (id) a.doc.moveChannel(id, static_cast<int>(a.doc.song().channels.size()) - 1);
      a.doc.endGroup(false);
      if (id) a.selectChannel(id);
    }
    const Channel* last = s.channels.empty() ? nullptr : &s.channels.back();
    bool lastEmpty = last != nullptr;
    for (const Pattern& p : s.patterns) {
      if (!last) break;
      if (p.notes.count(last->id) && !p.notes.at(last->id).empty()) lastEmpty = false;
      if (p.steps.count(last->id))
        for (uint8_t v : p.steps.at(last->id)) lastEmpty = lastEmpty && v == 0;
    }
    if (!lastEmpty) ImGui::BeginDisabled();
    if (b(9, "##subtr", "Remove track", "Removes the last track while it has no notes or steps") && last) {
      const ObjectId gone = last->id;
      if (a.settingsChannel == gone) a.settingsChannel = kNoId;  // its settings close with it (no other channel's open instead)
      a.doc.removeChannel(gone);
    }
    if (!lastEmpty) ImGui::EndDisabled();
  }
  }  // not the files
  colBox(xB);
  {
    const float bx0 = xB + 2, bx1 = xB + colW - 2;
    auto b = [&](int i, const char* id, const char* label, const char* hint, bool on = false) {
      float t0, t1;
      slot(i, &t0, &t1);
      return bevelButton(a, id, label, ImVec2(bx0, t0), ImVec2(bx1, t1), on, hint);
    };
    const bool playing = a.engine.playing();
    auto start = [&](PlayMode m, bool rec) {
      a.engine.setMode(m);
      if (m == PlayMode::Pattern) a.engine.setPattern(a.curPattern);
      a.recordArmed = rec;
      if (!playing) menuparity::playPressed(a);
    };
    if (b(0, "##psong", "Play song", "Plays the song from the position", playing && !a.recordArmed && a.engine.mode() == PlayMode::Song)) start(PlayMode::Song, false);
    if (b(1, "##ppat", "Play pattern", "Plays the pattern shown, looped", playing && !a.recordArmed && a.engine.mode() == PlayMode::Pattern)) start(PlayMode::Pattern, false);
    if (b(2, "##stop", "Stop", "Stops (again: back to the start)")) menuparity::stopPressed(a);
    if (b(3, "##rsong", "Record song", "Plays the song and records what is played", playing && a.recordArmed && a.engine.mode() == PlayMode::Song)) start(PlayMode::Song, true);
    if (b(4, "##rpat", "Record pattern", "Loops the pattern and records what is played", playing && a.recordArmed && a.engine.mode() == PlayMode::Pattern))
      start(PlayMode::Pattern, true);
    if (b(5, "##files", "Files", "Files: save, open, recent files and the project folder (the header's left part shows them)", gSwap == Swap::Disk))
      gSwap = gSwap == Swap::Disk ? Swap::None : Swap::Disk;
    if (b(6, "##inst", "Channel settings", "The selected channel's instrument and settings (as View > Channel settings)", a.settingsChannel != kNoId)) {
      if (a.settingsChannel != kNoId) a.settingsChannel = kNoId;
      else showPage(a, "Instrument");
    }
    if (b(7, "##smp", "Audio editor", "The audio editor, in the layout", a.settings.getBool("ui.loupePinned"))) {
      if (a.settings.getBool("ui.loupePinned")) a.settings.set("ui.loupePinned", false);
      else showPage(a, "Loupe");
    }
    if (b(8, "##cfg", "Settings", "The pattern editor's settings (the header shows them) and the way to every other setting", gSwap == Swap::Config))
      gSwap = gSwap == Swap::Config ? Swap::None : Swap::Config;
    if (b(9, "##help", "Tracker help", "The pattern keys, the effect letters and the screen keys (the header shows them)", gSwap == Swap::Help)) gSwap = Swap::Help;
  }
  // ---- instrument list, sample list, song name; the bank strip ----
  {
    const float iH = std::round(H * 126.0f / 173.0f), nameH = std::round(H * 24.0f / 173.0f);
    const float yI1 = std::round(y0 + iH), yN = yE - nameH;
    // Instruments.
    bevelBox(dl, ImVec2(xI, y0), ImVec2(xK, yI1));
    const ImVec2 l0(xI + pad, y0 + pad), l1(xK - pad, yI1 - pad);
    bevelBox(dl, l0, l1, true);
    const float lrh = std::max(14.0f, std::round(fs + 2));
    const int ci = s.channelIndex(a.selChannel);
    const int vis = std::max(1, static_cast<int>((l1.y - l0.y - 2) / lrh));
    static int bank = 0;
    const int banks = std::max(1, (static_cast<int>(s.channels.size()) + vis - 1) / vis);
    // The page follows the selection when the selection changes (a page picked by hand stays).
    static int lastCi = -2;
    if (ci != lastCi && ci >= 0 && (ci < bank * vis || ci >= (bank + 1) * vis)) bank = ci / vis;
    lastCi = ci;
    bank = std::clamp(bank, 0, banks - 1);
    dl->PushClipRect(l0, l1, true);
    for (int r = 0; r < vis; ++r) {
      const int i = bank * vis + r;
      const float y = l0.y + 1 + static_cast<float>(r) * lrh;
      const std::string num = trk::hex2(i + 1);
      if (i >= static_cast<int>(s.channels.size())) {
        dl->AddText(mono, fs, ImVec2(l0.x + 9, y + 1), theme::withAlpha(P.textFaint, 0.6f), num.c_str());
        continue;
      }
      const Channel& c = s.channels[static_cast<size_t>(i)];
      ImGui::SetCursorScreenPos(ImVec2(l0.x, y));
      ImGui::PushID(1000 + i);
      const bool click = ImGui::InvisibleButton("##ins", ImVec2(std::max(1.0f, l1.x - l0.x), lrh));
      const bool hov = ImGui::IsItemHovered();
      if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.settingsChannel = c.id;
      ImGui::PopID();
      ui::Hint("Click: select (new notes use it) \xC2\xB7 double-click: its instrument");
      if (click) a.selectChannel(c.id);
      if (i == ci) dl->AddRectFilled(ImVec2(l0.x + 1, y), ImVec2(l1.x - 1, y + lrh), theme::withAlpha(P.surface600, 0.95f));
      else if (hov) dl->AddRectFilled(ImVec2(l0.x + 1, y), ImVec2(l1.x - 1, y + lrh), theme::withAlpha(P.surface700, 0.6f));
      dl->AddRectFilled(ImVec2(l0.x + 2, y + 2), ImVec2(l0.x + 5, y + lrh - 2), theme::rgb(c.color));
      const float ty = std::round(y + (lrh - fs) * 0.5f);
      dl->AddText(mono, fs, ImVec2(l0.x + 9, ty), i == ci ? P.hot : P.textDim, num.c_str());
      const float nx = l0.x + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), l1.x - nx - 4, i == ci ? P.text : c.muted ? P.textFaint : P.textDim, c.name.c_str());
    }
    dl->PopClipRect();
    // Samples.
    bevelBox(dl, ImVec2(xI, yI1), ImVec2(xK, yN));
    const ImVec2 s0(xI + pad, yI1 + pad), s1(xK - pad, yN - pad);
    bevelBox(dl, s0, s1, true);
    const Channel* sc = s.channel(a.selChannel);
    std::string sample;
    if (sc && !sc->plugin.data.empty() && sc->plugin.data.find('/') != std::string::npos)
      sample = std::filesystem::path(sc->plugin.data).filename().string();
    dl->PushClipRect(s0, s1, true);
    const float sy = std::round(s0.y + (s1.y - s0.y - fs) * 0.5f);  // centred in the box (a short band)
    if (!sample.empty()) {
      dl->AddText(mono, fs, ImVec2(s0.x + 9, sy), P.hot, "00");
      dl->AddText(a.fontRegular, fs, ImVec2(s0.x + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x, sy), P.text, sample.c_str());
    } else {
      dl->AddText(a.fontRegular, fs, ImVec2(s0.x + 9, sy), P.textFaint, sc ? i18n::tr("No sample: a synthesized sound") : i18n::tr("No channel selected"));
    }
    dl->PopClipRect();
    // Song name.
    bevelBox(dl, ImVec2(xI, yN), ImVec2(xK, yE));
    const ImVec2 n0(xI + pad, yN + 3), n1(xK - pad, yE - 3);
    bevelBox(dl, n0, n1, true);
    // Edited in place: a click opens the field, Enter or a click elsewhere keeps it (one undo step).
    static bool editing = false;
    static char buf[128] = {};
    if (editing) {
      ImGui::SetCursorScreenPos(ImVec2(n0.x + 2, std::round(n0.y + (n1.y - n0.y - fs) * 0.5f) - ImGui::GetStyle().FramePadding.y));
      ImGui::SetNextItemWidth(n1.x - n0.x - 4);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
      if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
      const bool enter = ImGui::InputText("##titleedit", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      const bool done = enter || ImGui::IsItemDeactivated();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
      if (done) {
        editing = false;
        const std::string t = buf;
        if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && t != s.info.title) a.doc.editSong("rename song", [t](Song& song) { song.info.title = t; });
      }
    } else {
      const std::string title = s.info.title.empty() ? std::string(i18n::tr("Untitled song")) : s.info.title;
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(n0.x + 6, std::round(n0.y + (n1.y - n0.y - fs) * 0.5f)), n1.x - n0.x - 12,
                          s.info.title.empty() ? P.textFaint : P.text, title.c_str());
      ImGui::SetCursorScreenPos(n0);
      if (ImGui::InvisibleButton("##title", ImVec2(std::max(1.0f, n1.x - n0.x), std::max(1.0f, n1.y - n0.y)))) {
        editing = true;
        std::snprintf(buf, sizeof buf, "%s", s.info.title.c_str());
      }
      ui::Hint("The song's title (click to edit)");
    }
    // Bank strip: eight pages of the instrument list, the column filled; pages past the last
    // channel are there but off (like the list's numbers past the last channel).
    // More than eight pages: the strip shows the eight around the page in front (the wheel over
    // it steps a page).
    bevelBox(dl, ImVec2(xK, y0), ImVec2(o.x + W, yE));
    if (ImGui::IsMouseHoveringRect(ImVec2(xK, y0), ImVec2(o.x + W, yE)) && ImGui::GetIO().MouseWheel != 0)
      bank = std::clamp(bank + (ImGui::GetIO().MouseWheel > 0 ? -1 : 1), 0, banks - 1);
    const int group = bank / 8;
    const float kb = std::floor((H - 4 - 7) / 8.0f);
    for (int gb = 0; gb < 8; ++gb) {
      const int b = group * 8 + gb;
      const float t0 = std::round(y0 + 2 + static_cast<float>(gb) * (kb + 1));
      const float t1 = gb == 7 ? yE - 2 : t0 + kb;
      char lbl[16];
      std::snprintf(lbl, sizeof lbl, "%02X-%02X", b * vis + 1, (b + 1) * vis);
      ImGui::PushID(2000 + b);
      if (b >= banks) ImGui::BeginDisabled();
      if (bevelButton(a, "##bank", lbl, ImVec2(xK + 2, t0), ImVec2(o.x + W - 2, t1), b == bank, "Instruments of this page")) bank = b;
      if (b >= banks) ImGui::EndDisabled();
      ImGui::PopID();
    }
  }
  pageStrip(a, o, W, yE + 2, stripH);
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + av.y));
  ImGui::Dummy(ImVec2(1, 0));
}

void pageStrip(App& a, ImVec2 o, float W, float py0, float stripH) {
  const float fs = ImGui::GetFontSize(), py1 = py0 + stripH;
  float px = o.x;
  for (const Page& pg : kPages) {
    const char* label = i18n::tr(pg.label);
    const float w = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, label).x) + 24;
    if (px + w > o.x + W) break;
    ImGui::PushID(pg.window);
    if (bevelButton(a, "##page", pg.label, ImVec2(px, py0), ImVec2(px + w, py1), pageOn(a, pg), pg.hint)) showPage(a, pg.window);
    ImGui::PopID();
    px += w + 2;
  }
}
}  // namespace

void setHeaderSwap(int which) { gSwap = which == 1 ? Swap::Help : which == 2 ? Swap::Disk : which == 3 ? Swap::Config : Swap::None; }
int headerSwap() { return gSwap == Swap::Help ? 1 : gSwap == Swap::Disk ? 2 : gSwap == Swap::Config ? 3 : 0; }

void orderList(App& a) {
  if (layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker)) {
    trackerpages::enteringFrame();  // keeps the Tracker's frame count (see there)
    // The Tracker starts on the pattern page, with the keys, each time it comes on screen (at the
    // start, and back from another layout; a layout loaded from its saved state too).
    static int lastFrame = -10;
    const int frame = ImGui::GetFrameCount();
    if (frame - lastFrame > 1)
      if (const bool* f = layoutui::windowOpenFlag(a, "Pattern editor"); f && *f) showPage(a, "Pattern editor");
    lastFrame = frame;
    pumpPage(a);
    // T-21: a band too short for its ten button rows at the interface's text size (a small window)
    // draws its text smaller, down to three quarters, rather than letting it run over the edges.
    {
      const float fs0 = ImGui::GetFontSize(), h = ImGui::GetContentRegionAvail().y;
      const float need = fs0 * 13.0f + 22;  // ten button rows of text plus their bevels, and the page strip
      const float scale = h < need ? std::max(0.75f, h / need) : 1.0f;
      if (scale < 1.0f) ImGui::SetWindowFontScale(scale);
      header(a);
      if (scale < 1.0f) ImGui::SetWindowFontScale(1.0f);
    }
    return;
  }
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  orderRows(a, o, ImVec2(o.x + av.x, o.y + std::max(av.y, 60.0f)));
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + std::max(av.y, 60.0f)));
  ImGui::Dummy(ImVec2(1, 0));
}

}  // namespace gl::trackerview
