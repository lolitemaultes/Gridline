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
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/MenuOps.h"
#include "../core/Params.h"
#include "../core/TrackerGrid.h"
#include "App.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
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

// The page's title bar: a raised box with the title left and `info` right (monospaced).
float titleBar(App& a, ImVec2 o, float w, const char* title, const std::string& info) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize(), h = std::round(fs + 8);
  bevelBox(dl, o, ImVec2(o.x + w, o.y + h));
  boxTitle(a, ImVec2(o.x + 6, std::round(o.y + (h - fs) * 0.5f)), title);
  if (!info.empty()) {
    ImFont* mono = monoFont(a);
    const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, info.c_str()).x;
    dl->AddText(mono, fs, ImVec2(std::round(o.x + w - 6 - tw), std::round(o.y + (h - fs) * 0.5f)), pal().textDim, info.c_str());
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

}  // namespace

void pageTitle(App& a, const char* title, const std::string& info) {
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  const float h = titleBar(a, o, w, title, info);
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h + kGap));
}

// ---- Steps: the channels and their steps ----
void steps(App& a) {
  trackerview::pumpPage(a);  // (the header band may be hidden: T-05)
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Pattern* pat = s.pattern(a.curPattern);
  ImFont* mono = monoFont(a);
  const float fs = ImGui::GetFontSize();
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const int nSteps = pat ? std::max(1, pat->stepsPerBar * pat->bars) : 16;
  char info[96];
  std::snprintf(info, sizeof info, "%s %s  %d %s", i18n::tr("Pattern"), pat ? trk::hex2(trk::patternNumber(s, pat->id)).c_str() : "--", nSteps,
                i18n::tr("steps"));
  const float th = titleBar(a, o, av.x, i18n::tr("Steps"), info);
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
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    const float nameW = std::min(200.0f, std::round(W * 0.22f)), lampW = rh - 2;
    const float gx = c0.x + 2 + nameW + kGap + lampW + 6;
    const float avail = c0.x + W - 4 - gx;
    const float group = 3;  // extra gap every 4 steps
    const float cw = std::max(8.0f, std::floor((avail - group * static_cast<float>(nSteps / 4)) / static_cast<float>(nSteps)) - 1);
    for (size_t ci = 0; ci < s.channels.size(); ++ci) {
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
          if (click && pat) a.doc.setStep(pat->id, c.id, k, v > 0 ? 0 : 100);
        }
      }
      ImGui::PopID();
    }
    (void)mono;
    (void)fs;
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
// same edits the Studio playlist's clip menu makes, so every layout shows the same clip): where it
// is, mute; an audio clip's gain, pitch, fades and direction; a pattern clip's playback transpose.
// "More" opens the clip's whole menu (warp, audio tools, curves, export, as in the playlist).
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
  const float x0 = p0.x + 6, y0 = p0.y + 4;
  // Title: kind, name, place.
  char where[96];
  std::snprintf(where, sizeof where, "%s %s  %s %s  %s %.2f", i18n::tr("Track"), trk::hex2(c->track + 1).c_str(), i18n::tr("Bar"),
                trk::hex2(static_cast<int>(c->start / bar)).c_str(), i18n::tr("bars"), static_cast<double>(s.clipLength(*c)) / static_cast<double>(bar));
  const char* kind = c->kind == ClipKind::Audio ? i18n::tr("Audio clip") : c->kind == ClipKind::Pattern ? i18n::tr("Pattern clip") : i18n::tr("Automation clip");
  boxTitle(a, ImVec2(x0, y0), kind);
  const float kw = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, kind).x) + 10;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x0 + kw, y0), (p1.x - x0 - kw) * 0.5f, P.text, clipTitle(s, *c).c_str());
  const float ww = mono->CalcTextSizeA(fs, FLT_MAX, 0, where).x;
  dl->AddText(mono, fs, ImVec2(std::round(p1.x - 6 - ww), y0), P.textDim, where);
  // The fields, one row.
  ImGui::SetCursorScreenPos(ImVec2(x0, std::round(y0 + fs + 6)));
  ImGui::PushID("##songclipbox");
  static uint64_t key = 0x54524b00;
  const float fw = std::round(fs * 6.5f);
  auto label = [&](const char* t) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr(t));
    ImGui::SameLine();
  };
  bool muted = c->muted;
  if (ImGui::Checkbox(i18n::tr("Mute"), &muted)) a.doc.editClip(id, "mute clip", [muted](Clip& x) { x.muted = muted; });
  ui::Hint("The clip stays in place, silent");
  if (c->kind == ClipKind::Audio) {
    float gainDb = static_cast<float>(std::max(clipgain::kMinDb, clipgain::toDb(c->gain)));
    float cents = c->pitch;
    float fi = static_cast<float>(c->fadeIn * 1000.0), fo = static_cast<float>(c->fadeOut * 1000.0);
    ImGui::SameLine(0, 14);
    label("Gain");
    ImGui::SetNextItemWidth(fw);
    if (ImGui::DragFloat("##gain", &gainDb, 0.1f, static_cast<float>(clipgain::kMinDb), static_cast<float>(clipgain::kMaxDb), "%.1f dB"))
      a.doc.editClip(id, "clip gain", [&](Clip& x) { x.gain = clipgain::fromDb(gainDb <= clipgain::kMinDb ? clipgain::kMinDb - 0.1 : gainDb); }, key);
    ImGui::SameLine(0, 14);
    label("Pitch");
    ImGui::SetNextItemWidth(fw);
    if (ImGui::DragFloat("##pitch", &cents, 1.0f, -2400.0f, 2400.0f, "%.0f ct")) a.doc.editClip(id, "clip pitch", [&](Clip& x) { x.pitch = cents; }, key + 1);
    ImGui::SameLine(0, 14);
    label("Fade in");
    ImGui::SetNextItemWidth(fw);
    if (ImGui::DragFloat("##fi", &fi, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade in", [&](Clip& x) { x.fadeIn = std::max(0.0f, fi) / 1000.0, x.fadeInManual = true; }, key + 2);
    ImGui::SameLine(0, 14);
    label("Fade out");
    ImGui::SetNextItemWidth(fw);
    if (ImGui::DragFloat("##fo", &fo, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade out", [&](Clip& x) { x.fadeOut = std::max(0.0f, fo) / 1000.0, x.fadeOutManual = true; }, key + 3);
    ImGui::SameLine(0, 14);
    bool rev = c->reverse;
    if (ImGui::Checkbox(i18n::tr("Reverse"), &rev)) a.doc.editClip(id, "reverse clip", [&a](Clip& x) { toggleClipReverse(a.doc.song(), x); });
    ui::Hint("Plays the clip backwards (keeps the same audio)");
  } else if (c->kind == ClipKind::Pattern) {
    int tr = c->playTranspose;
    ImGui::SameLine(0, 14);
    label("Transpose");
    ImGui::SetNextItemWidth(fw);
    if (ImGui::DragInt("##tr", &tr, 0.2f, -48, 48, "%d st")) a.doc.editClip(id, "clip transpose", [tr](Clip& x) { x.playTranspose = std::clamp(tr, -48, 48); }, key + 4);
    ui::Hint("Plays the pattern's notes this many semitones higher or lower (the pattern stays as written)");
  }
  ImGui::SameLine(0, 14);
  if (ui::TextButton(i18n::tr("More..."), false, 0)) ImGui::OpenPopup("##songclipmore");
  ui::Hint("The clip's whole menu, as in the playlist: warp markers, audio tools, curves, export");
  if (ImGui::BeginPopup("##songclipmore")) {
    devArrangementClipMenu(a, id);
    ImGui::EndPopup();
  }
  ImGui::PopID();
  a.doc.endGesture();
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
  const float boxH = gSongClip != kNoId ? std::round(fs * 2 + 22) : 0.0f;  // the clip box under the rows
  const ImVec2 b0(o.x, hy + rh + kGap), b1(o.x + av.x, o.y + std::max(av.y - (boxH > 0 ? boxH + kGap : 0.0f), th + rh * 4));
  const float contentH = static_cast<float>(bars) * rh + 2;
  const Tick pos = static_cast<Tick>(a.engine.position());
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
        const Clip* starts = nullptr;
        const Clip* runs = nullptr;
        int startCount = 0;  // clips starting in this bar (the first is drawn, the rest counted)
        for (const Span& sp : byTrack[static_cast<size_t>(t)]) {
          const Clip& c = *sp.clip;
          if (c.start >= b0t && c.start < b1t) ++startCount;
          if (c.start >= b0t && c.start < b1t && !starts) starts = &c;
          else if (c.start < b0t && sp.end > b0t && !runs) runs = &c;
        }
        const float ty = std::round(y + (rh - fs) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(r * 1024 + t);
        const bool click = ImGui::InvisibleButton("##cell", ImVec2(colW, rh));
        const bool hov = ImGui::IsItemHovered();
        const bool dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const bool rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::PopID();
        if (hov) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW, y + rh), theme::withAlpha(P.text, 0.08f));
        const Clip* c = starts ? starts : runs;
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
          const bool first = starts != nullptr, last = c->start + s.clipLength(*c) <= b1t;
          dl->AddRectFilled(ImVec2(x + 4, y), ImVec2(x + colW, y + rh), theme::withAlpha(theme::rgb(col), 0.12f));
          dl->AddRectFilled(ImVec2(x + 1, y), ImVec2(x + 4, y + rh), theme::rgb(col));
          if (c->muted) dl->AddRectFilled(ImVec2(x + 4, y), ImVec2(x + colW, y + rh), theme::withAlpha(P.surface950, 0.45f));
          if (c->id == gSongClip) {
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 1, y + rh), P.hot);
            dl->AddRectFilled(ImVec2(x + colW - 1, y), ImVec2(x + colW, y + rh), P.hot);
            if (first) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW, y + 1), P.hot);
            if (last) dl->AddRectFilled(ImVec2(x, y + rh - 1), ImVec2(x + colW, y + rh), P.hot);
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
              dl->AddText(mono, fs * 0.85f, ImVec2(std::round(x + colW - 6 - gw - colW * 0.12f), std::round(y + (rh - fs * 0.85f) * 0.5f)), P.textDim, g);
            }
          }
          if (starts && textOk) {
            if (cp) {
              const std::string label = trk::hex2(trk::patternNumber(s, cp->id));
              dl->AddText(mono, fs, ImVec2(x + 8, ty), cp->id == a.curPattern ? P.hot : P.text, label.c_str());
              const float lx = x + 8 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
              theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(lx, ty), x + colW - lx - 4, P.textDim, cp->name.c_str());
            } else {
              theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x + 8, ty), colW - 12, P.textDim, other.c_str());
            }
          }
          // A clip starting inside the bar: a tick where it starts; more clips in the bar: "+n".
          if (starts && starts->start > b0t) {
            const float sy = std::round(y + rh * static_cast<float>(starts->start - b0t) / static_cast<float>(bar));
            dl->AddRectFilled(ImVec2(x + 4, sy), ImVec2(x + 10, sy + 1), P.text);
          }
          if (startCount > 1 && textOk) {
            const std::string more = "+" + std::to_string(startCount - 1);
            const float mw = mono->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, more.c_str()).x;
            dl->AddRectFilled(ImVec2(x + colW - mw - 8, y + 1), ImVec2(x + colW - 1, y + rh - 1), theme::withAlpha(P.surface900, 0.9f));
            dl->AddText(mono, fs * 0.85f, ImVec2(x + colW - mw - 4, std::round(y + (rh - fs * 0.85f) * 0.5f)), P.hot, more.c_str());
          }
        } else {
          if (textOk) dl->AddText(mono, fs, ImVec2(x + 8, ty), theme::withAlpha(P.textFaint, 0.55f), "\xC2\xB7\xC2\xB7");
        }
        if (click) {
          a.engine.setPosition(b0t, true);
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
      if (const Clip* mc = s.clip(menuClip)) {
        ImGui::TextDisabled("%s", clipTitle(s, *mc).c_str());
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
