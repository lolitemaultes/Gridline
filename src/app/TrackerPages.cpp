// Gridline - the Tracker layout's own pages (K-106): the Song, Steps and Mix pages in the tracker
// look (bevelled boxes, two-digit hex numbering in the monospaced face, the header's row height and
// colour roles). They draw inside the Playlist, Channel rack and Mixer windows while the Tracker
// layout is built from its shell, so those windows keep their identity (menus, keys, the page strip)
// and every other layout keeps its own views. Same data, same Document commands: a step toggled
// here, a clip placed or a volume dragged is one undo step and syncs to collaborators as anywhere.
#include "TrackerPages.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Arrangements.h"
#include "../core/ClipGain.h"
#include "../engine/ClipSource.h"
#include "../engine/PlaylistTools.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/MenuOps.h"
#include "../core/Params.h"
#include "../core/TrackerGrid.h"
#include "App.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "MenuParityUi.h"
#include "MixerMeters.h"
#include "PixelGrid.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::trackerpages {

using theme::pal;
using trackerview::bevelBox;
using trackerview::bevelButton;
using trackerview::boxTitle;
using trackerview::monoFont;

bool active(const App& a) {
  return layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker);
}

bool enteringFrame() {
  static int seen = -10, entered = -10;
  if (layoutui::active() != layout::Preset::Tracker) return false;
  const int f = ImGui::GetFrameCount();
  if (seen != f && seen != f - 1) entered = f;  // not the Tracker on the frame before
  seen = f;
  if (f != entered) return false;
  const ImGuiWindow* w = ImGui::GetCurrentWindow();
  return w && !w->DockIsActive;
}


namespace {

constexpr float kGap = 2;

float rowHeight() { return std::round(ImGui::GetFontSize() + 4); }

// The page's title bar: a raised box with the title left, `info` right (monospaced), and at the
// right end Exit, back to the pattern (S-03: the pages are screens swapped into the centre, as the
// classic tracker's; each has its Exit, and the page strip switches between them. They do not
// close or float: the centre always shows one page and nothing gets lost behind another window).
float titleBar(App& a, ImVec2 o, float w, const char* title, const std::string& info) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), h = std::round(fs + 8);
  bevelBox(dl, o, ImVec2(o.x + w, o.y + h));
  boxTitle(a, ImVec2(o.x + 6, std::round(o.y + (h - fs) * 0.5f)), title);
  const float ew = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Exit")).x) + 24;
  if (bevelButton(a, "##pageexit", "Exit", ImVec2(o.x + w - 2 - ew, o.y + 2), ImVec2(o.x + w - 2, o.y + h - 2), false, "Back to the pattern"))
    trackerview::showPage(a, "Pattern editor");
  if (!info.empty()) {
    ImFont* mono = monoFont(a);
    const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, info.c_str()).x;
    dl->AddText(mono, fs, ImVec2(std::round(o.x + w - ew - 10 - tw), std::round(o.y + (h - fs) * 0.5f)), pal().textDim, info.c_str());
  }
  return h;
}

// A sunken name field (the instrument list's look): number, colour mark, name.
void nameField(App& a, ImVec2 p0, ImVec2 p1, int number, unsigned colour, const std::string& name, bool selected, bool dim) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  bevelBox(dl, p0, p1, true);
  if (selected) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), theme::withAlpha(P.surface600, 0.9f));
  dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 3), ImVec2(p0.x + 5, p1.y - 3), theme::rgb(colour));
  const float ty = std::round(p0.y + (p1.y - p0.y - fs) * 0.5f);
  const std::string num = trk::hex2(number);
  dl->AddText(mono, fs, ImVec2(p0.x + 9, ty), selected ? P.hot : P.textDim, num.c_str());
  const float nx = p0.x + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), p1.x - nx - 4, dim ? P.textFaint : selected ? P.text : P.textDim, name.c_str());
}

// A small square lamp (mute: lit while the sound plays).
bool lamp(App& a, const char* id, ImVec2 p0, float size, bool lit, ImU32 colour, const char* hint) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  const bool click = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hov = ImGui::IsItemHovered();
  bevelBox(dl, p0, ImVec2(p0.x + size, p0.y + size), true);
  const ImVec2 i0(p0.x + 3, p0.y + 3), i1(p0.x + size - 3, p0.y + size - 3);
  dl->AddRectFilled(i0, i1, lit ? colour : theme::withAlpha(colour, hov ? 0.35f : 0.18f));
  if (hint) ui::Hint(hint);
  (void)a;
  return click;
}

// A horizontal value bar (volume, pan) in a sunken box; dragging sets it. Returns true while it changed.
bool valueBar(const char* id, ImVec2 p0, ImVec2 p1, double v, double lo, double hi, bool centred, double* out, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bevelBox(dl, p0, p1, true);
  const float x0 = p0.x + 2, x1 = p1.x - 2;
  const float k = static_cast<float>((std::clamp(v, lo, hi) - lo) / (hi - lo));
  const ImU32 fill = act ? P.hot : hov ? theme::mix(P.led, P.text, 0.25f) : P.led;
  if (centred) {
    const float mid = std::round((x0 + x1) * 0.5f), at = std::round(x0 + (x1 - x0) * k);
    dl->AddRectFilled(ImVec2(std::min(mid, at), p0.y + 3), ImVec2(std::max(mid, at) + 1, p1.y - 3), fill);
    dl->AddRectFilled(ImVec2(mid, p0.y + 2), ImVec2(mid + 1, p1.y - 2), P.textFaint);
  } else {
    dl->AddRectFilled(ImVec2(x0, p0.y + 3), ImVec2(std::round(x0 + (x1 - x0) * k), p1.y - 3), fill);
  }
  if (hint) ui::Hint(hint);
  if (act && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) || ImGui::IsItemActivated())) {
    const float mk = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
    *out = lo + (hi - lo) * mk;
    return true;
  }
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {  // double-click: the default
    *out = centred ? (lo + hi) * 0.5 : 0.8;
    return true;
  }
  return false;
}

// The scrolled body under a title bar: a sunken box and a child window clipped to it.
bool beginBody(const char* id, ImVec2 p0, ImVec2 p1, float contentH) {
  bevelBox(ImGui::GetWindowDrawList(), p0, p1, true);
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 1, p0.y + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  const bool open = ImGui::BeginChild(id, ImVec2(std::max(1.0f, p1.x - p0.x - 2), std::max(1.0f, p1.y - p0.y - 2)), ImGuiChildFlags_None,
                                      contentH > p1.y - p0.y ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  return open;
}
void endBody(float contentH) {
  ImGui::SetCursorPos(ImVec2(0, contentH));
  ImGui::Dummy(ImVec2(1, 0));
  ImGui::EndChild();
}

// K-306: clang-cl 20.1.8 (Windows CI) crashed in its x86 instruction selection on steps(), and
// again after the step cells moved into a function of their own, which it inlined back. The Steps
// page's parts are kept out of line, and clang-cl builds them without optimisation (a page drawn
// once a frame; every other compiler optimises them as usual).
#if defined(__clang__) && defined(_MSC_VER)
#define GL_STEPS_FN __attribute__((noinline, optnone))
#elif defined(_MSC_VER)
#define GL_STEPS_FN __declspec(noinline)
#else
#define GL_STEPS_FN __attribute__((noinline))
#endif

// One channel's row of step cells.
GL_STEPS_FN void stepCells(App& a, ImDrawList* dl, const Pattern* pat, ObjectId channel, const std::vector<uint8_t>* row, int nSteps, int playStep,
                           float gx, float y, float rh, float cw, float group) {
  const auto& P = pal();
  for (int k = 0; k < nSteps; ++k) {
    const float x = std::round(gx + static_cast<float>(k) * (cw + 1) + group * static_cast<float>(k / 4));
    const ImVec2 s0(x, y + 1), s1(x + cw, y + rh - 2);
    const uint8_t v = row && static_cast<size_t>(k) < row->size() ? (*row)[static_cast<size_t>(k)] : 0;
    ImGui::SetCursorScreenPos(s0);
    ImGui::PushID(k);
    const bool click = ImGui::InvisibleButton("##st", ImVec2(cw, s1.y - s0.y));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    const bool beat = (k / 4) % 2 == 0;
    bevelBox(dl, s0, s1, v == 0);
    if (v > 0) {
      dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(s1.x - 1, s1.y - 1), theme::mix(P.surface700, P.hot, 0.35f + 0.65f * static_cast<float>(v) / 127.0f));
    } else {
      dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(s1.x - 1, s1.y - 1), beat ? P.surface900 : P.surface950);
    }
    if (hov) dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(s1.x - 1, s1.y - 1), theme::withAlpha(P.text, 0.12f));
    if (k == playStep) dl->AddRectFilled(ImVec2(s0.x + 1, s1.y - 3), ImVec2(s1.x - 1, s1.y - 1), P.playhead);
    if (click && pat) a.doc.setStep(pat->id, channel, k, v > 0 ? 0 : 100);
  }
}

// One channel's row on the Steps page: its name, mute lamp, and step cells (or its notes' starts).
GL_STEPS_FN void stepRow(App& a, const Pattern* pat, size_t ci, ImVec2 c0, float W, float nameW, float lampW, float gx, float avail, float rh,
                         float cw, float group, int nSteps, int playStep) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Channel& c = s.channels[ci];
  const float y = std::round(c0.y + 2 + static_cast<float>(ci) * rh);
  ImGui::PushID(static_cast<int>(ci));
  const bool sel = a.selChannel == c.id;
  nameField(a, ImVec2(c0.x + 2, y), ImVec2(c0.x + 2 + nameW, y + rh - 1), static_cast<int>(ci) + 1, c.color, c.name, sel, c.muted);
  ImGui::SetCursorScreenPos(ImVec2(c0.x + 2, y));
  if (ImGui::InvisibleButton("##name", ImVec2(nameW, rh - 1))) a.selectChannel(c.id);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.settingsChannel = c.id;
  ui::Hint("Click: select \xC2\xB7 double-click: its instrument");
  if (lamp(a, "##mute", ImVec2(c0.x + 2 + nameW + kGap, y), lampW, !c.muted, P.led, "Lit: the channel plays \xC2\xB7 click: mute"))
    a.doc.setParam(channelParam(c.id, ChannelParam::Mute), c.muted ? 0 : 1, false);
  const std::vector<uint8_t>* row = pat && pat->steps.count(c.id) ? &pat->steps.at(c.id) : nullptr;
  const bool hasNotes = pat && pat->notes.count(c.id) && !pat->notes.at(c.id).empty();
  if (hasNotes && (!row || std::none_of(row->begin(), row->end(), [](uint8_t v) { return v > 0; }))) {
    // A channel playing notes: the piano roll edits them; here a quiet line of their starts.
    const std::vector<Note>& ns = pat->notes.at(c.id);
    const Tick len = std::max<Tick>(1, s.patternLength(*pat));
    dl->AddRectFilled(ImVec2(gx, y + 1), ImVec2(c0.x + W - 4, y + rh - 2), theme::withAlpha(P.surface800, 0.6f));
    for (const Note& n : ns) {
      const float nx0 = std::round(gx + avail * static_cast<float>(n.start % len) / static_cast<float>(len));
      const float nx1 = std::max(nx0 + 2, std::round(gx + avail * static_cast<float>(std::min<Tick>(len, n.start % len + n.length)) / static_cast<float>(len)));
      const float ny = std::round(y + 3 + (rh - 8) * (1.0f - static_cast<float>(n.pitch % 24) / 24.0f));
      dl->AddRectFilled(ImVec2(nx0, ny), ImVec2(nx1, ny + 2), theme::rgb(c.color));
    }
    ImGui::SetCursorScreenPos(ImVec2(gx, y));
    if (ImGui::InvisibleButton("##notes", ImVec2(std::max(1.0f, avail), rh - 1))) a.selectChannel(c.id);
    ui::Hint("This channel plays notes: the pattern editor and the piano roll edit them");
  } else {
    stepCells(a, dl, pat, c.id, row, nSteps, playStep, gx, y, rh, cw, group);
  }
  ImGui::PopID();
}

// The Steps page's title text: the pattern's number and its length in steps.
std::string stepsInfo(const Song& s, const Pattern* pat, int nSteps) {
  const std::string num = pat ? trk::hex2(trk::patternNumber(s, pat->id)) : std::string("--");
  char info[96];
  std::snprintf(info, sizeof info, "%s %s  %d %s", i18n::tr("Pattern"), num.c_str(), nSteps, i18n::tr("steps"));
  return info;
}

}  // namespace

void pageTitle(App& a, const char* title, const std::string& info) {
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  const float h = titleBar(a, o, w, title, info);
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h + kGap));
}

// ---- Steps: the channels and their steps ----
GL_STEPS_FN void steps(App& a) {
  trackerview::pumpPage(a);  // (the header band may be hidden: T-05)
  const Song& s = a.doc.song();
  const Pattern* pat = s.pattern(a.curPattern);
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const int nSteps = pat ? std::max(1, pat->stepsPerBar * pat->bars) : 16;
  const float th = titleBar(a, o, av.x, i18n::tr("Steps"), stepsInfo(s, pat, nSteps));
  const float rh = rowHeight(), y0 = o.y + th + kGap;
  const ImVec2 b0(o.x, y0), b1(o.x + av.x, o.y + std::max(av.y, th + rh * 3));
  const float contentH = static_cast<float>(s.channels.size()) * rh + 4;
  // The step being played (pattern mode on this pattern, or this pattern under the song position).
  int playStep = -1;
  if (pat && a.engine.playing()) {
    Tick t = -1;
    if (a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == pat->id) t = static_cast<Tick>(a.engine.position());
    else if (a.engine.mode() == PlayMode::Song) t = trk::patternTickAt(s, pat->id, static_cast<Tick>(a.engine.position()));
    const Tick st = s.ticksPerStep(*pat);
    if (t >= 0 && st > 0) playStep = static_cast<int>((t / st) % nSteps);
  }
  if (beginBody("##steps", b0, b1, contentH)) {
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    const float nameW = std::min(200.0f, std::round(W * 0.22f)), lampW = rh - 2;
    const float gx = c0.x + 2 + nameW + kGap + lampW + 6;
    const float avail = c0.x + W - 4 - gx;
    const float group = 3;  // extra gap every 4 steps
    const float cw = std::max(8.0f, std::floor((avail - group * static_cast<float>(nSteps / 4)) / static_cast<float>(nSteps)) - 1);
    for (size_t ci = 0; ci < s.channels.size(); ++ci)
      stepRow(a, pat, ci, c0, W, nameW, lampW, gx, avail, rh, cw, group, nSteps, playStep);
    endBody(contentH);
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y));
  ImGui::Dummy(ImVec2(1, 0));
}

// ---- Song: the arrangement as rows of bars, a column per playlist track ----
// The clip picked on the Song page (a click), its properties in the clip box under the rows.
ObjectId gSongClip = kNoId;

namespace {
std::string clipTitle(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Pattern) {
    const Pattern* p = s.pattern(c.pattern);
    return p ? trk::hex2(trk::patternNumber(s, p->id)) + " " + p->name : std::string(i18n::tr("Pattern"));
  }
  if (c.kind == ClipKind::Audio) {
    const Channel* ch = s.channel(c.source);
    return ch ? ch->name : std::string(i18n::tr("Audio clip"));
  }
  const AutomationClip* au = s.automation(c.source);
  return au ? au->name : std::string(i18n::tr("Automation"));
}

// The clip box: the picked clip's properties, edited in place, each change one Document edit (the
// same edits the Studio playlist's clip menu and edge drags make, so every layout shows the same
// clip; nothing is stored that the other layouts lack). T-13: where it starts, how long it is and
// where its content starts inside it (the offset), its mute; an audio clip's gain, fades, pitch and
// direction; a pattern clip's playback transpose. Each value is a stepper (the arrows, the wheel, or
// a drag on the value): positions in rows of the song grid, Shift a whole bar. "More" opens the
// clip's whole menu (warp, audio tools, curves, export, as in the playlist).
// A note the clip box shows in its title row for a moment (instead of a toast over its fields).
std::string gBoxNote;
double gBoxNoteUntil = 0;

float clipBoxHeight() { return std::round(ImGui::GetFontSize() + 4) * 4 + 4 * 5 + 2; }

// bars:beats:rows of a length or an offset, from 0 ("+": and some ticks more).
std::string barsText(Tick t, Tick bar, Tick beat, Tick row) {
  char b[96];  // (room for three 64-bit numbers: GCC -Wformat-truncation)
  const Tick bars = t / bar, beats = (t % bar) / beat, rows = (t % beat) / row, rest = t % row;
  std::snprintf(b, sizeof b, "%lld:%lld:%02lld%s", static_cast<long long>(bars), static_cast<long long>(beats), static_cast<long long>(rows), rest ? "+" : "");
  return b;
}

void clipBox(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Clip* c = s.clip(gSongClip);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bevelBox(dl, p0, p1);
  if (!c) return;
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const ObjectId id = c->id;
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const Tick beat = std::max<Tick>(1, s.ppq);
  const Tick row = std::max<Tick>(1, s.ppq / 4);  // one row of the song grid (four to the beat)
  static TempoMap tm;
  static uint64_t tmRev = ~0ull;
  if (tmRev != a.doc.revision()) tm.build(s), tmRev = a.doc.revision();
  const bool shift = ImGui::GetIO().KeyShift;
  const Tick unit = shift ? bar : row;
  static uint64_t key = 0x54524b00;
  const float rh = std::round(fs + 4), pad = 4;
  const float x0 = p0.x + 6, y0 = p0.y + pad;
  // Title: kind, name; the track at the right.
  const char* kind = c->kind == ClipKind::Audio ? i18n::tr("Audio clip") : c->kind == ClipKind::Pattern ? i18n::tr("Pattern clip") : i18n::tr("Automation clip");
  const float ty = std::round(y0 + (rh - fs) * 0.5f);
  boxTitle(a, ImVec2(x0, ty), kind);
  const float kw = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, kind).x) + 10;
  char where[48];
  std::snprintf(where, sizeof where, "%s %s", i18n::tr("Track"), trk::hex2(c->track + 1).c_str());
  const float ww = mono->CalcTextSizeA(fs, FLT_MAX, 0, where).x;
  const bool note = !gBoxNote.empty() && a.now < gBoxNoteUntil;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x0 + kw, ty), p1.x - 12 - ww - (x0 + kw) - 10, note ? P.hot : P.text, note ? gBoxNote.c_str() : clipTitle(s, *c).c_str());
  dl->AddText(mono, fs, ImVec2(std::round(p1.x - 6 - ww), ty), P.textDim, where);
  ImGui::PushID("##songclipbox");
  // The fields: a grid of steppers, four to a row (a narrow page: as many as fit).
  const float inner = p1.x - p0.x - 12;
  const int perRow = std::clamp(static_cast<int>(inner / std::max(fs * 13.0f, 160.0f)), 2, 4);
  const float cellW = std::floor((inner - static_cast<float>(perRow - 1) * 10) / static_cast<float>(perRow));
  int n = 0;
  auto cell = [&](ImVec2* at) {
    const int r = 1 + n / perRow, k = n % perRow;
    *at = ImVec2(std::round(x0 + static_cast<float>(k) * (cellW + 10)), std::round(y0 + static_cast<float>(r) * (rh + pad)));
    ++n;
    return at->y + rh <= p1.y - 2;  // (a short box shows the fields that fit)
  };
  const float lw = std::round(std::min(cellW * 0.42f, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Fade out")).x + 10));
  auto field = [&](const char* sid, const char* label, const std::string& value, const char* hint) {
    ImVec2 at;
    if (!cell(&at)) return 0;
    return trackerview::stepper(a, sid, label, value, at, cellW, rh, hint, lw, true);
  };
  auto toggle = [&](const char* sid, const char* label, bool on, const char* hint) {
    ImVec2 at;
    if (!cell(&at)) return false;
    return bevelButton(a, sid, label, at, ImVec2(at.x + std::min(cellW, std::round(fs * 6)), at.y + rh), on, hint);
  };
  // Start: the whole clip moves (its content with it).
  if (const int d = field("start", "Start", menuparity::positionText(a, static_cast<double>(c->start)),
                          "Where the clip starts in the song (a row a step, Shift: a bar; drag the value for more)")) {
    const Tick to = std::max<Tick>(0, c->start + unit * d);
    if (to != c->start) a.doc.editClip(id, "move clip", [to](Clip& x) { x.start = to; }, key + 10);
  }
  // Length: the right edge (audio stops at the end of its audio, as the playlist's edge drag).
  const Tick len = s.clipLength(*c);
  if (const int d = field("len", "Length", barsText(len, bar, beat, row), "How long the clip plays, in bars:beats:rows (a row a step, Shift: a bar)")) {
    const Tick by = std::max<Tick>(row, len + unit * d) - len;
    if (by != 0) {
      const std::vector<Clip> after = pltools::resizeEnds(s, s.playlist, {id}, by, false, &tm);
      // R3-08: an audio clip at the end of its audio grows no further: say so.
      if (by > 0 && c->kind == ClipKind::Audio)
        for (const Clip& x : after)
          if (x.id == id && s.clipLength(x) <= len) {
            // R4-08: said in the box's title row (a toast would cover its fields); the Offset hint
            // only when an earlier offset has audio to give.
            gBoxNote = i18n::tr(c->audioStart > 0.0005 ? "The audio ends there: the clip plays no longer (an earlier Offset gives it more)"
                                                       : "The audio ends there: the clip plays no longer");
            gBoxNoteUntil = a.now + 4.0;
          }
      a.doc.editClips("resize clips", [&after](std::vector<Clip>& cs) {
        if (cs.size() == after.size()) cs = after;
      }, key + 11);
    }
  }
  // Offset: where the content starts inside the clip (the playlist's slip edit).
  {
    std::string off;
    if (c->kind == ClipKind::Audio) {
      char b[32];
      std::snprintf(b, sizeof b, "%.3f s", c->audioStart);
      off = b;
    } else {
      off = barsText(c->offset, bar, beat, row);
    }
    if (const int d = field("off", "Offset", off,
                            c->kind == ClipKind::Audio ? "Where the clip's audio starts in its sample (a row a step, Shift: a bar)"
                                                       : "Where the clip's content starts inside it (a row a step, Shift: a bar)")) {
      const Clip after = pltools::slipClip(s, tm, *c, -unit * d);
      a.doc.editClip(id, "slip clip", [after](Clip& x) { x = after; }, key + 12);
    }
  }
  if (toggle("##mute", "Mute", c->muted, "The clip stays in place, silent")) {
    const bool m = !c->muted;
    a.doc.editClip(id, "mute clip", [m](Clip& x) { x.muted = m; });
  }
  if (c->kind == ClipKind::Audio) {
    n = std::max(n, perRow);  // the audio's own values from the next row (R3-07: n counts from the first field row)
    char g[32];
    const double db = clipgain::toDb(c->gain);
    if (db < clipgain::kMinDb) std::snprintf(g, sizeof g, "-inf dB");
    else std::snprintf(g, sizeof g, "%+.1f dB", db);
    if (const int d = field("gain", "Gain", g, "The clip's volume (0.5 dB a step, Shift: 6 dB)")) {
      const double base = db < clipgain::kMinDb ? clipgain::kMinDb - 0.1 : db;
      const double to = std::clamp(std::round((base + (shift ? 6.0 : 0.5) * d) * 10.0) / 10.0, clipgain::kMinDb - 0.1, clipgain::kMaxDb);
      a.doc.editClip(id, "clip gain", [to](Clip& x) { x.gain = clipgain::fromDb(to); }, key);
    }
    auto ms = [](double sec) {
      char b[32];
      std::snprintf(b, sizeof b, "%.0f ms", sec * 1000.0);
      return std::string(b);
    };
    const double fstep = shift ? 0.1 : 0.01;
    if (const int d = field("fi", "Fade in", ms(c->fadeIn), "The fade at the clip's start (10 ms a step, Shift: 100 ms)")) {
      const double to = std::max(0.0, std::round((c->fadeIn + fstep * d) * 1000.0) / 1000.0);
      a.doc.editClip(id, "clip fade in", [to](Clip& x) { x.fadeIn = to, x.fadeInManual = true; }, key + 2);
    }
    if (const int d = field("fo", "Fade out", ms(c->fadeOut), "The fade at the clip's end (10 ms a step, Shift: 100 ms)")) {
      const double to = std::max(0.0, std::round((c->fadeOut + fstep * d) * 1000.0) / 1000.0);
      a.doc.editClip(id, "clip fade out", [to](Clip& x) { x.fadeOut = to, x.fadeOutManual = true; }, key + 3);
    }
    char pc[32];
    std::snprintf(pc, sizeof pc, "%+.0f ct", static_cast<double>(c->pitch));
    if (const int d = field("pitch", "Pitch", pc, "The clip's pitch in cents (10 a step, Shift: a semitone)")) {
      const float to = std::clamp(std::round(c->pitch + static_cast<float>((shift ? 100 : 10) * d)), -2400.0f, 2400.0f);
      a.doc.editClip(id, "clip pitch", [to](Clip& x) { x.pitch = to; }, key + 1);
    }
    n = std::max(n, perRow * 2);
    if (toggle("##rev", "Reverse", c->reverse, "Plays the clip backwards (keeps the same audio)"))
      a.doc.editClip(id, "reverse clip", [&a](Clip& x) { toggleClipReverse(a.doc.song(), x); });
  } else if (c->kind == ClipKind::Pattern) {
    n = std::max(n, perRow);
    char t[16];
    std::snprintf(t, sizeof t, "%+d st", c->playTranspose);
    if (const int d = field("tr", "Transpose", t, "Plays the pattern's notes this many semitones higher or lower (the pattern stays as written; Shift: an octave)")) {
      const int to = std::clamp(c->playTranspose + d * (shift ? 12 : 1), -48, 48);
      a.doc.editClip(id, "clip transpose", [to](Clip& x) { x.playTranspose = to; }, key + 4);
    }
  } else {
    n = std::max(n, perRow);
  }
  {
    ImVec2 at;
    if (cell(&at) && bevelButton(a, "##more", "More...", at, ImVec2(at.x + std::min(cellW, std::round(fs * 6)), at.y + rh), false,
                                 "The clip's whole menu, as in the playlist: warp markers, audio tools, curves, export"))
      ImGui::OpenPopup("##songclipmore");
  }
  if (ImGui::BeginPopup("##songclipmore")) {
    devArrangementClipMenu(a, id);
    ImGui::EndPopup();
  }
  ImGui::PopID();
  // A held arrow or a dragged value is one undo step; it ends when the mouse lets go.
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) a.doc.endGesture();
}
}  // namespace

void song(App& a) {
  trackerview::pumpPage(a);  // (the header band may be hidden: T-05)
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImFont* mono = monoFont(a);
  const float fs = ImGui::GetFontSize();
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const int usedBars = static_cast<int>((s.lengthTicks() + bar - 1) / bar);
  const int bars = std::max(16, usedBars + 8);
  int usedTracks = 0;
  for (const Clip& c : s.playlist) usedTracks = std::max(usedTracks, c.track + 1);
  const int tracks = std::clamp(usedTracks + 1, 4, static_cast<int>(s.tracks.size()));
  char info[128];
  // T-27: the columns past the used tracks are free ones to put clips on; the count says so.
  std::snprintf(info, sizeof info, "%s %s  %s %s  %s %s", i18n::tr("Bars"), trk::hex2(usedBars).c_str(), i18n::tr("Tracks"), trk::hex2(usedTracks).c_str(),
                i18n::tr("Free"), trk::hex2(std::max(0, tracks - usedTracks)).c_str());
  const float th = titleBar(a, o, av.x, i18n::tr("Song"), info);
  const float rh = rowHeight();
  // Track headers (bevelled), then the scrolled rows.
  const float numW = std::round(mono->CalcTextSizeA(fs, FLT_MAX, 0, "000").x + 10);
  const float colW = std::max(70.0f, std::floor((av.x - numW - 6) / static_cast<float>(tracks)) - kGap);
  const float hy = o.y + th + kGap;
  ImDrawList* wdl = ImGui::GetWindowDrawList();
  for (int t = 0; t < tracks; ++t) {
    const float x = std::round(o.x + numW + 2 + static_cast<float>(t) * (colW + kGap));
    const PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
    const std::string name = tr.name.empty() ? std::string(i18n::tr("Track")) + " " + std::to_string(t + 1) : tr.name;
    nameField(a, ImVec2(x, hy), ImVec2(x + colW, hy + rh), t + 1, tr.color, name, false, tr.muted);
  }
  if (!s.clip(gSongClip)) gSongClip = kNoId;
  const float boxH = gSongClip != kNoId ? clipBoxHeight() : 0.0f;  // the clip box under the rows
  const ImVec2 b0(o.x, hy + rh + kGap), b1(o.x + av.x, o.y + std::max(av.y - (boxH > 0 ? boxH + kGap : 0.0f), th + rh * 4));
  const float contentH = static_cast<float>(bars) * rh + 2;
  const Tick pos = a.engine.playing() && a.engine.mode() == PlayMode::Pattern ? trackerview::songPosition(a) : static_cast<Tick>(a.engine.position());
  const int playBar = static_cast<int>(pos / bar);
  (void)wdl;
  static ObjectId menuClip = kNoId;
  if (beginBody("##song", b0, b1, contentH)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    // Which clip covers (track, bar): the one starting there first, else one running through it.
    // The rows the page really shows: the body's clip, and the page window's own inner rectangle.
    const ImGuiWindow* page = ImGui::GetCurrentWindow()->ParentWindow;
    const float clipY0 = std::max(dl->GetClipRectMin().y, page ? page->InnerClipRect.Min.y : -FLT_MAX);
    const float clipY1 = std::min(dl->GetClipRectMax().y, page ? page->InnerClipRect.Max.y : FLT_MAX);
    // The shown tracks' clips with their ends, once (a big song has thousands; each cell looks only at its track's).
    struct Span {
      const Clip* clip;
      Tick end;
    };
    std::vector<std::vector<Span>> byTrack(static_cast<size_t>(tracks));
    for (const Clip& c : s.playlist)
      if (c.track >= 0 && c.track < tracks) byTrack[static_cast<size_t>(c.track)].push_back({&c, c.start + s.clipLength(c)});
    for (int r = 0; r < bars; ++r) {
      const float y = std::round(c0.y + static_cast<float>(r) * rh);
      if (y >= clipY1 || y + rh <= clipY0) continue;  // scrolled out: nothing drawn (nor laid under the next panel)
      // A row the body's edge cuts keeps its stripes but not its texts (no half letters at the edge).
      const bool textOk = y + (rh + fs) * 0.5f <= clipY1 && y + (rh - fs) * 0.5f >= clipY0;
      if (r % 4 == 0) dl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + W, y + rh), theme::withAlpha(P.surface800, 0.55f));
      if (r == playBar && a.engine.playing()) dl->AddRectFilled(ImVec2(c0.x, y), ImVec2(c0.x + W, y + rh), theme::withAlpha(P.playhead, 0.22f));
      const std::string num = trk::hex2(r);
      if (textOk) dl->AddText(mono, fs, ImVec2(c0.x + 4, std::round(y + (rh - fs) * 0.5f)), r % 4 == 0 ? P.text : P.textDim, num.c_str());
      for (int t = 0; t < tracks; ++t) {
        const float x = std::round(c0.x + numW + 1 + static_cast<float>(t) * (colW + kGap));
        if (x >= dl->GetClipRectMax().x) break;  // the tracks past the page's right edge
        const Tick b0t = static_cast<Tick>(r) * bar, b1t = b0t + bar;
        // R3-08, R4-05: the clip drawn in a bar is the longest of those starting in it, else the
        // longest of those running through it; every other clip covering the bar is counted ("+n").
        // A bar whose drawn clip is not the one drawn in the bar above names it (a clip that goes on
        // after the drawn one ended is named where it shows).
        struct Pick {
          const Clip* clip = nullptr;
          bool starts = false;
          int covering = 0;
        };
        auto pick = [&](Tick from, Tick to) {
          Pick k;
          Tick sEnd = 0, rEnd = 0;
          const Clip* runs = nullptr;
          for (const Span& sp : byTrack[static_cast<size_t>(t)]) {
            const Clip& c = *sp.clip;
            if (c.start >= from && c.start < to) {
              ++k.covering;
              if (!k.starts || sp.end > sEnd) k.clip = &c, k.starts = true, sEnd = sp.end;
            } else if (c.start < from && sp.end > from) {
              ++k.covering;
              if (sp.end > rEnd) runs = &c, rEnd = sp.end;
            }
          }
          if (!k.starts) k.clip = runs;
          return k;
        };
        const Pick here = pick(b0t, b1t);
        const Clip* starts = here.starts ? here.clip : nullptr;
        const int stackCount = here.covering;  // the drawn clip and the others under it
        const bool named = here.clip && (here.starts || r == 0 || pick(b0t - bar, b0t).clip != here.clip);
        const float ty = std::round(y + (rh - fs) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(r * 1024 + t);
        const bool click = ImGui::InvisibleButton("##cell", ImVec2(colW, rh));
        const bool hov = ImGui::IsItemHovered();
        const bool dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const bool rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::PopID();
        if (hov) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW, y + rh), theme::withAlpha(P.text, 0.08f));
        const Clip* c = here.clip;
        if (c) {
          const Pattern* cp = c->kind == ClipKind::Pattern ? s.pattern(c->pattern) : nullptr;
          // An audio or automation clip: its channel's or automation's name and colour (no pattern number).
          unsigned col = cp ? cp->color : 0x808080u;
          std::string other;
          if (c->kind == ClipKind::Audio) {
            if (const Channel* ch = s.channel(c->source)) col = ch->color, other = ch->name;
          } else if (!cp) {
            if (const AutomationClip* au = s.automation(c->source)) col = au->color, other = au->name;
          }
          // The clip's bars in a faint wash of its colour, the stripe at the left, the picked clip
          // outlined as one box over all its bars (top on the first, bottom on the last).
          const Tick cEnd = c->start + s.clipLength(*c);
          const bool first = starts != nullptr, last = cEnd <= b1t;
          // T-13: the clip's box starts where it starts inside its first bar and ends where it ends
          // in its last (a clip half a bar in is drawn from the middle of the bar).
          const float yT = first ? std::round(y + rh * static_cast<float>(c->start - b0t) / static_cast<float>(bar)) : y;
          const float yB = last ? std::max(yT + 2, std::round(y + rh * static_cast<float>(cEnd - b0t) / static_cast<float>(bar))) : y + rh;
          dl->AddRectFilled(ImVec2(x + 4, yT), ImVec2(x + colW, yB), theme::withAlpha(theme::rgb(col), 0.12f));
          dl->AddRectFilled(ImVec2(x + 1, yT), ImVec2(x + 4, yB), theme::rgb(col));
          if (c->muted) dl->AddRectFilled(ImVec2(x + 4, yT), ImVec2(x + colW, yB), theme::withAlpha(P.surface950, 0.45f));
          if (c->id == gSongClip) {
            dl->AddRectFilled(ImVec2(x, yT), ImVec2(x + 1, yB), P.hot);
            dl->AddRectFilled(ImVec2(x + colW - 1, yT), ImVec2(x + colW, yB), P.hot);
            if (first) dl->AddRectFilled(ImVec2(x, yT), ImVec2(x + colW, yT + 1), P.hot);
            if (last) dl->AddRectFilled(ImVec2(x, yB - 1), ImVec2(x + colW, yB), P.hot);
          }
          // An audio clip: its fades as ramps over the bars they cover, its gain in the first bar.
          if (c->kind == ClipKind::Audio) {
            const Tick cend = c->start + s.clipLength(*c);
            const double tps = std::max(1e-9, static_cast<double>(s.ppq) * s.tempo / 60.0);  // (the fades are seconds; the song tempo maps them for the eye)
            const Tick fin = c->start + static_cast<Tick>(c->fadeIn * tps), fout = cend - static_cast<Tick>(c->fadeOut * tps);
            const ImU32 ramp = theme::withAlpha(P.led, 0.35f);
            auto frac = [&](Tick t) { return std::clamp(static_cast<float>(t - b0t) / static_cast<float>(bar), 0.0f, 1.0f); };
            if (c->fadeIn > 0 && fin > b0t && c->start < b1t) {  // the rising part inside this bar
              const float f0 = frac(std::max(c->start, b0t)), f1 = frac(std::min(fin, b1t));
              dl->AddTriangleFilled(ImVec2(x + colW - 2, y + rh * f0), ImVec2(x + colW - 2, y + rh * f1), ImVec2(x + colW - 2 - (colW * 0.25f) * f1, y + rh * f1), ramp);
            }
            if (c->fadeOut > 0 && cend > b0t && fout < b1t) {
              const float f0 = frac(std::max(fout, b0t)), f1 = frac(std::min(cend, b1t));
              dl->AddTriangleFilled(ImVec2(x + colW - 2 - colW * 0.25f * (1 - f0), y + rh * f0), ImVec2(x + colW - 2, y + rh * f0), ImVec2(x + colW - 2, y + rh * f1), ramp);
            }
            if (starts && textOk && std::fabs(c->gain - 1.0f) > 1e-4f) {
              char g[24];
              const double db = clipgain::toDb(c->gain);
              if (db < clipgain::kMinDb) std::snprintf(g, sizeof g, "-inf");
              else std::snprintf(g, sizeof g, "%+.1f", db);
              const float gw = mono->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, g).x;
              const float mutedW = c->muted ? mono->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, "M").x + 10 : 0.0f;
              dl->AddText(mono, fs * 0.85f, ImVec2(std::round(x + colW - 6 - gw - colW * 0.12f - mutedW), std::round(y + (rh - fs * 0.85f) * 0.5f)), P.textDim, g);
            }
          }
          if (named && textOk) {
            // T-13: content that starts later than the clip (an offset) shows after the name: ">" and
            // where it starts (seconds of audio, or bars:beats:rows of the pattern).
            std::string off;
            if (!starts) {
              // (a clip named where it goes on: no offset, it is not its start)
            } else if (c->kind == ClipKind::Audio && c->audioStart > 0.0005) {
              char b[32];
              std::snprintf(b, sizeof b, "  >%.3fs", c->audioStart);
              off = b;
            } else if (c->kind != ClipKind::Audio && c->offset > 0) {
              off = "  >" + barsText(c->offset, bar, std::max<Tick>(1, s.ppq), std::max<Tick>(1, s.ppq / 4));
            }
            if (cp) {
              const std::string label = trk::hex2(trk::patternNumber(s, cp->id));
              dl->AddText(mono, fs, ImVec2(x + 8, ty), cp->id == a.curPattern ? P.hot : P.text, label.c_str());
              const float lx = x + 8 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
              theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(lx, ty), x + colW - lx - 4, P.textDim, (cp->name + off).c_str());
            } else {
              theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x + 8, ty), colW - 12, P.textDim, (other + off).c_str());
            }
          }
          // T-13: a muted clip says so in its first bar ("M" in a dark tag at the right).
          if (starts && textOk && c->muted) {
            const float mw = mono->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, "M").x;
            const ImVec2 m0(std::round(x + colW - mw - 9), y + 2), m1(x + colW - 3, y + rh - 2);
            dl->AddRectFilled(m0, m1, theme::withAlpha(P.surface950, 0.9f));
            dl->AddText(mono, fs * 0.85f, ImVec2(m0.x + 3, std::round(y + (rh - fs * 0.85f) * 0.5f)), P.textFaint, "M");
          }
          // A clip starting inside the bar: a tick where it starts; more clips in the bar: "+n".
          if (starts && starts->start > b0t) {
            const float sy = std::round(y + rh * static_cast<float>(starts->start - b0t) / static_cast<float>(bar));
            dl->AddRectFilled(ImVec2(x + 4, sy), ImVec2(x + 10, sy + 1), P.text);
          }
          if (stackCount > 1 && textOk) {
            const std::string more = "+" + std::to_string(stackCount - 1);
            const float mw = mono->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, more.c_str()).x;
            dl->AddRectFilled(ImVec2(x + colW - mw - 8, y + 1), ImVec2(x + colW - 1, y + rh - 1), theme::withAlpha(P.surface900, 0.9f));
            dl->AddText(mono, fs * 0.85f, ImVec2(x + colW - mw - 4, std::round(y + (rh - fs * 0.85f) * 0.5f)), P.hot, more.c_str());
          }
        } else {
          if (textOk) dl->AddText(mono, fs, ImVec2(x + 8, ty), theme::withAlpha(P.textFaint, 0.55f), "\xC2\xB7\xC2\xB7");
        }
        if (click) {
          trackerview::setSongPosition(a, b0t);  // (kept while a pattern plays)
          gSongClip = c ? c->id : kNoId;
          if (c && c->kind == ClipKind::Pattern) a.selectPattern(c->pattern);
        }
        if (dbl && !c && a.curPattern != kNoId) a.doc.addClip(a.curPattern, t, b0t);
        if (rclick && c) {
          gSongClip = c->id;
          menuClip = c->id;
          ImGui::OpenPopup("##songclipmenu");
        }
      }
    }
    if (ImGui::BeginPopup("##songclipmenu")) {
      if (s.clip(menuClip)) {
        // (N-08: the clip's menu below opens with its name; no second title here)
        if (ImGui::MenuItem(i18n::tr("Delete clip"))) {
          a.doc.removeClip(menuClip);
          if (gSongClip == menuClip) gSongClip = kNoId;
        } else {
          ImGui::Separator();
          devArrangementClipMenu(a, menuClip);
        }
      }
      ImGui::EndPopup();
    }
    endBody(contentH);
  }
  ui::Hint("Click: go there and pick the clip (its pattern shows; the box below edits it) \xC2\xB7 double-click an empty bar: place the pattern shown \xC2\xB7 right-click: the clip's menu");
  if (boxH > 0) clipBox(a, ImVec2(o.x, b1.y + kGap), ImVec2(o.x + av.x, b1.y + kGap + boxH));
  ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y + (boxH > 0 ? boxH + kGap : 0.0f)));
  ImGui::Dummy(ImVec2(1, 0));
}

// ---- Mix: one row per mixer track ----
void mixer(App& a) {
  trackerview::pumpPage(a);  // (the header band may be hidden: T-05)
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImFont* mono = monoFont(a);
  const float fs = ImGui::GetFontSize();
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  char info[64];
  std::snprintf(info, sizeof info, "%s %s", i18n::tr("Tracks"), trk::hex2(static_cast<int>(s.mixer.size())).c_str());
  const float th = titleBar(a, o, av.x, i18n::tr("Mix"), info);
  const float rh0 = rowHeight();
  const ImVec2 b0(o.x, o.y + th + kGap), b1(o.x + av.x, o.y + std::max(av.y, th + rh0 * 3));
  // Few tracks: the rows grow (up to twice their height) so the page has no empty lower part.
  const float rh = std::max(rh0, std::min(rh0 * 2, std::floor((b1.y - b0.y - 4) / static_cast<float>(std::max<size_t>(1, s.mixer.size())))));
  const float contentH = static_cast<float>(s.mixer.size()) * rh + 4;
  if (beginBody("##mix", b0, b1, contentH)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    const float nameW = std::min(200.0f, std::round(W * 0.22f)), lampW = rh0 - 2;
    const float volW = std::round(W * 0.28f), panW = std::round(W * 0.14f);
    for (size_t i = 0; i < s.mixer.size(); ++i) {
      const MixerTrack& m = s.mixer[i];
      const float y = std::round(c0.y + 2 + static_cast<float>(i) * rh);
      ImGui::PushID(static_cast<int>(i));
      const std::string name = m.name.empty() ? (i == 0 ? std::string(i18n::tr("Master")) : std::string(i18n::tr("Insert")) + " " + std::to_string(i)) : m.name;
      float x = c0.x + 2;
      nameField(a, ImVec2(x, y), ImVec2(x + nameW, y + rh - 1), static_cast<int>(i), m.color, name, a.selMixer == m.id, m.muted);
      ImGui::SetCursorScreenPos(ImVec2(x, y));
      if (ImGui::InvisibleButton("##name", ImVec2(nameW, rh - 1))) a.selMixer = m.id;
      x += nameW + kGap;
      if (lamp(a, "##mute", ImVec2(x, std::round(y + (rh - rh0) * 0.5f)), lampW, !m.muted, P.led, "Lit: the track plays \xC2\xB7 click: mute"))
        a.doc.setParam(mixerParam(m.id, MixerParam::Mute), m.muted ? 0 : 1, false);
      x += lampW + 6;
      double nv = 0;
      char txt[16];
      std::snprintf(txt, sizeof txt, "%3d", static_cast<int>(std::lround(m.volume * 100)));
      dl->AddText(mono, fs, ImVec2(x, std::round(y + (rh - fs) * 0.5f)), P.textDim, txt);
      x += mono->CalcTextSizeA(fs, FLT_MAX, 0, "000 ").x;
      if (valueBar("##vol", ImVec2(x, y), ImVec2(x + volW, y + rh - 1), m.volume, 0.0, 1.25, false, &nv, "Volume (drag; double-click: the default)"))
        a.doc.setParam(mixerParam(m.id, MixerParam::Volume), nv, true);
      x += volW + 8;
      if (valueBar("##pan", ImVec2(x, y), ImVec2(x + panW, y + rh - 1), m.pan, -1.0, 1.0, true, &nv, "Pan (drag; double-click: centre)"))
        a.doc.setParam(mixerParam(m.id, MixerParam::Pan), nv, true);
      x += panW + 8;
      // A segmented level meter to the row's end.
      const float lvl = std::clamp(mixmeters::get(a, m.id).peak(), 0.0f, 1.0f);
      const float mx1 = c0.x + W - 4;
      if (mx1 - x > 20) {
        bevelBox(dl, ImVec2(x, y), ImVec2(mx1, y + rh - 1), true);
        const int segs = std::max(4, static_cast<int>((mx1 - x - 4) / 5));
        const int lit = static_cast<int>(std::round(lvl * static_cast<float>(segs)));
        for (int k = 0; k < segs; ++k) {
          const float sx = std::round(x + 2 + static_cast<float>(k) * 5);
          const ImU32 col = k >= segs * 9 / 10 ? P.danger : k >= segs * 7 / 10 ? P.hot : P.led;
          dl->AddRectFilled(ImVec2(sx, y + 3), ImVec2(sx + 3, y + rh - 4), k < lit ? col : theme::withAlpha(col, 0.12f));
        }
      }
      ImGui::PopID();
    }
    endBody(contentH);
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y));
  ImGui::Dummy(ImVec2(1, 0));
}

void pads(App& a) {
  trackerview::pumpPage(a);  // (the header band may be hidden: T-05)
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const bool live = a.engine.mode() == PlayMode::Live;
  // Title bar with the page's two controls at its right (Live mode is this page's own switch).
  const float th = titleBar(a, o, av.x, "Pads", "");
  {
    const float bw = std::ceil(std::max(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Live mode")).x,
                                        a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Stop all")).x)) + 24;
    const float x1 = o.x + av.x - 3, y0 = o.y + 2, y1 = o.y + th - 2;
    if (bevelButton(a, "##stopall", "Stop all", ImVec2(x1 - bw, y0), ImVec2(x1, y1), false, "Stops every track at its launch grid")) a.engine.stopAllLive();
    if (bevelButton(a, "##livemode", "Live mode", ImVec2(x1 - bw * 2 - 2, y0), ImVec2(x1 - bw - 2, y1), live,
                    "Live mode: the transport runs freely and plays only the clips launched here"))
      a.engine.setMode(live ? PlayMode::Song : PlayMode::Live);
  }
  const ImVec2 b0(o.x, o.y + th + kGap), b1(o.x + av.x, o.y + std::max(av.y, th + 60));
  if (s.live.areaEnd <= 0) {
    bevelBox(dl, b0, b1, true);
    ui::EmptyState(dl, b0, b1, "No live area", "Song page: clips placed before the live area's end become pads (Playlist menu, Live area)");
    ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y));
    ImGui::Dummy(ImVec2(1, 0));
    return;
  }
  const float rh = std::round(rowHeight() * 2 + 2), nameW = std::round(std::max(150.0f, fs * 12)), padW = std::round(fs * 8);
  std::vector<int> rows;
  for (int tr = 0; tr < static_cast<int>(s.tracks.size()); ++tr)
    if (!liveClips(s, tr).empty()) rows.push_back(tr);
  const float contentH = static_cast<float>(rows.size()) * (rh + kGap) + kGap;
  if (beginBody("##padsbody", b0, b1, contentH)) {
    static ObjectId held = kNoId;
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(a.now) * 8.0f);
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImDrawList* cl = ImGui::GetWindowDrawList();
    for (size_t r = 0; r < rows.size(); ++r) {
      const int tr = rows[r];
      const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
      const float y = c0.y + kGap + static_cast<float>(r) * (rh + kGap), x = c0.x + kGap;
      ImGui::PushID(tr);
      nameField(a, ImVec2(x, y), ImVec2(x + nameW, y + rowHeight()), tr + 1, t.color, playlistTrackName(s, static_cast<size_t>(tr)), false, false);
      const std::string mode = std::string(padModeName(t.pad)) + " \xC2\xB7 " + followModeName(t.follow);
      theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(x + 9, std::round(y + rowHeight() + 2 + (rh - rowHeight() - 2 - fs) * 0.5f)), nameW - 60, P.textFaint,
                          mode.c_str());
      if (bevelButton(a, "##stop", "Stop", ImVec2(x + nameW - 48, y + rowHeight() + 2), ImVec2(x + nameW, y + rh), false,
                      "Stops this track (right-click: its live settings)"))
        a.engine.stopLiveTrack(tr);
      if (ImGui::BeginPopupContextItem("##trlive")) {
        liveTrackMenu(a, tr);
        ImGui::EndPopup();
      }
      const std::vector<int> clips = liveClips(s, tr);
      const ObjectId act = a.engine.liveActive(tr), que = a.engine.liveQueued(tr);
      for (size_t k = 0; k < clips.size(); ++k) {
        const Clip& c = s.playlist[static_cast<size_t>(clips[k])];
        ImGui::PushID(static_cast<int>(c.id));
        const ImVec2 q(x + nameW + kGap + static_cast<float>(k) * (padW + kGap), y), q1(q.x + padW, y + rh);
        ImGui::SetCursorScreenPos(q);
        ImGui::InvisibleButton("##pad", ImVec2(padW, rh));
        if (ImGui::IsItemActivated()) {
          if (!live) a.engine.setMode(PlayMode::Live);
          a.engine.launchClip(c.id);
          held = c.id;
        }
        const bool hov = ImGui::IsItemHovered(), on = act == c.id, waiting = que == c.id;
        Rgb rgb = 0x50565E;
        std::string nm;
        int num = 0;
        if (c.kind == ClipKind::Pattern) {
          if (const Pattern* pt = s.pattern(c.pattern)) rgb = pt->color, nm = pt->name, num = trk::patternNumber(s, pt->id);
        } else if (c.kind == ClipKind::Audio) {
          if (const Channel* ch = s.channel(c.source)) rgb = ch->color, nm = ch->name;
        } else if (const AutomationClip* au = s.automation(c.source)) {
          rgb = au->color, nm = au->name;
        }
        bevelBox(cl, q, q1, on || ImGui::IsItemActive());
        if (on) cl->AddRectFilled(ImVec2(q.x + 1, q.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(P.hot, 0.85f));
        else if (hov) cl->AddRectFilled(ImVec2(q.x + 1, q.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(P.surface600, 0.5f));
        cl->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q1.x - 2, q.y + 5), theme::rgb(rgb));
        if (waiting) cl->AddRect(ImVec2(q.x + 1, q.y + 1), ImVec2(q1.x - 1, q1.y - 1), theme::withAlpha(P.hot, 0.35f + 0.65f * pulse), 0, 0, 2.0f);
        const ImU32 tc = on ? theme::contrastText(P.hot) : P.text;
        const std::string n = num ? trk::hex2(num) : std::string("--");
        cl->AddText(mono, fs, ImVec2(q.x + 5, q.y + 7), on ? tc : P.textDim, n.c_str());
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(q.x + 5, q.y + 7 + fs + 1), padW - 10, tc, nm.c_str());
        ui::Hint("Press to launch (on the track's launch grid)");
        ImGui::PopID();
      }
      ImGui::PopID();
    }
    if (held != kNoId && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      a.engine.releaseClip(held);
      held = kNoId;
    }
    endBody(contentH);
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, b1.y));
  ImGui::Dummy(ImVec2(1, 0));
}

}  // namespace gl::trackerpages
