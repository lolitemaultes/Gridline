// Gridline - the Clips layout's In/Out section (K-112c checkpoint 55). See LiveIo.h.
#include "LiveIo.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/TrackModes.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "LiveLook.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::liveio {

namespace {

enum class Kind { None, Audio, Midi, Group };

Kind kindOf(const Song& s, int t) {
  if (t < 0 || t >= static_cast<int>(s.tracks.size()) || trackmodes::isAutomationLane(s, t)) {
    return Kind::None;
  }
  if (trackmodes::isGroupTrack(s, t)) {
    return Kind::Group;
  }
  return trackmodes::modeOf(s, t) == TrackMode::Audio ? Kind::Audio : Kind::Midi;
}

// The mixer track the track plays through (its fader's), whose first send is its output.
const MixerTrack* playedThrough(const Song& s, int t) {
  if (kindOf(s, t) == Kind::Audio || kindOf(s, t) == Kind::Group) {
    return trackmodes::linkedMixer(s, t);
  }
  const MixerTrack* m = trackmodes::playedMixer(s, t);
  return m && m->id != s.masterId() ? m : nullptr;
}

// A MIDI track's instrument (kNoId: none).
ObjectId instrumentOf(const Song& s, int t) {
  if (kindOf(s, t) != Kind::Midi) {
    return kNoId;
  }
  const PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
  return tr.mode == TrackMode::Instrument ? tr.channel : kNoId;
}

char letterOf(const Song& s, ObjectId mixer) {
  const std::vector<size_t> r = trackmodes::returnTracks(s);
  for (size_t k = 0; k < r.size() && k < 26; ++k) {
    if (s.mixer[r[k]].id == mixer) {
      return static_cast<char>('A' + k);
    }
  }
  return 0;
}

std::string destName(const Song& s, ObjectId id) {
  if (id == s.masterId()) {
    return i18n::tr("Main");
  }
  const int mi = s.mixerIndex(id);
  if (mi < 0) {
    return std::string();
  }
  std::string n = mixerTrackName(s, static_cast<size_t>(mi));
  if (const char lt = letterOf(s, id)) {
    n = std::string(1, lt) + " " + n;
  }
  return n;
}

// The menus: one open at a time, for one track (or one return: gMenuMixer).
enum class Menu { From, To, Channel, ReturnTo };
const ImGuiID kMenuId = ImHashStr("##clips_io_menu");
int gMenuTrack = -1;
ObjectId gMenuMixer = kNoId;
Menu gMenu = Menu::From;
ImVec2 gMenuAt;

void openMenu(int track, Menu which, ImVec2 at, ObjectId mixer = kNoId) {
  gMenuTrack = track, gMenu = which, gMenuAt = at, gMenuMixer = mixer;
  ImGui::OpenPopupEx(kMenuId, ImGuiPopupFlags_None);
}

void setOutput(App& a, ObjectId from, ObjectId to) {
  const Song& s = a.doc.song();
  if (from == kNoId || from == to || !canRoute(s, from, to)) {
    a.notify(i18n::tr("That would play the track into itself"), true);
    return;
  }
  // Checkpoint 56 (N55-01): the output moves; the aux sends stay (one to the new output keeps its level,
  // greyed out and kept aside in Song::keptSends while the track plays into that return: checkpoint 57).
  a.doc.editSong("track output", [&](Song& x) { trackmodes::setOutput(x, from, to); });
}

// One field: a recessed box with its text and a small down arrow when it opens a menu.
// Checkpoint 57 (N56-07): `caption` ("From", "To"), drawn faint before the value, names the row on the
// compact layouts (no room for a label above), when caption and value both fit.
void field(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const std::string& text, bool menu, bool hot, bool dim,
           const char* caption = nullptr) {
  const auto& P = theme::pal();
  dl->AddRectFilled(p0, p1, P.dark ? P.field : P.surface950, theme::kRadiusSm);
  if (hot) {
    dl->AddRect(p0, p1, theme::withAlpha(P.text, 0.35f), theme::kRadiusSm);
  }
  const float fs = theme::kFontLabel;
  const float aw = menu ? 9.0f : 0.0f;
  const float ty = theme::textTop(a.fontRegular, fs, p0.y, p1.y - p0.y);
  float tx = p0.x + 4;
  if (caption && *caption) {
    const float cw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, caption).x;
    const float vw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, text.c_str()).x;
    if (cw + 4 + vw <= p1.x - p0.x - 8 - aw) {
      dl->AddText(a.fontRegular, fs, ImVec2(tx, ty), P.textFaint, caption);
      tx += cw + 4;
    }
  }
  dl->PushClipRect(p0, p1, true);
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(tx, ty), p1.x - tx - 4 - aw, dim ? P.textFaint : P.text, text.c_str());
  dl->PopClipRect();
  if (menu) {
    const float cx = p1.x - 7, cy = (p0.y + p1.y) * 0.5f;
    dl->AddTriangleFilled(ImVec2(cx - 3, cy - 1.5f), ImVec2(cx + 3, cy - 1.5f), ImVec2(cx, cy + 2), dim ? P.textFaint : P.textDim);
  }
}

// A row label ("Audio From", "Monitor", "Audio To"), small and faint, as the reference's.
void label(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text) {
  const auto& P = theme::pal();
  dl->PushClipRect(p0, p1, true);
  dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(p0.x + 1, theme::textTop(a.fontRegular, theme::kFontLabel, p0.y, p1.y - p0.y)), P.textFaint, text);
  dl->PopClipRect();
}

// Monitor In / Auto / Off as three segments.
bool monitorRow(App& a, ImDrawList* dl, ImVec2 m0, ImVec2 m1, int mon, bool midi, bool hovered, bool click, int* picked) {
  const auto& P = theme::pal();
  const ImVec2 m = ImGui::GetIO().MousePos;
  const InputMonitor order[3] = {InputMonitor::On, InputMonitor::Auto, InputMonitor::Off};
  const char* names[3] = {i18n::tr("In"), i18n::tr("Auto"), i18n::tr("Off")};
  // Checkpoint 57 (N56-07): each segment as wide as its word plus an even share of the rest, so "Auto"
  // reads in full wherever the three words fit (the 46 px Arrangement rows too); equal thirds below that.
  float tw[3], sum = 0;
  for (int i = 0; i < 3; ++i) {
    tw[i] = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, names[i]).x;
    sum += tw[i];
  }
  const float avail = m1.x - m0.x - 2;
  const float share = std::floor((avail - sum) / 3.0f);
  float xs = m0.x;
  bool over = false;
  for (int i = 0; i < 3; ++i) {
    const float sw = share >= 4.0f ? std::round(tw[i] + share) : std::floor(avail / 3.0f);
    const ImVec2 a0(xs, m0.y), a1(i == 2 ? m1.x : xs + sw, m1.y);
    xs = a1.x + 1;
    const bool on = mon == static_cast<int>(order[i]);
    const bool hot = hovered && m.x >= a0.x && m.x < a1.x && m.y >= a0.y && m.y < a1.y;
    over = over || hot;
    dl->AddRectFilled(a0, a1, on ? P.hot : hot ? P.surface700 : P.surface900, theme::kRadiusSm);
    // A narrow segment shows the name's first letter (I, A, O), so the lit one always says which.
    const char* full = names[i];
    const char* end = nullptr;
    ImVec2 ts = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, full);
    if (ts.x > a1.x - a0.x - 2) {
      end = full + 1;
      ts = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, full, end);
    }
    if (ts.x <= a1.x - a0.x - 2) {
      dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round((a0.x + a1.x - ts.x) * 0.5f), theme::textTop(a.fontRegular, theme::kFontLabel, a0.y, a1.y - a0.y)),
                  on ? theme::contrastText(P.hot) : P.textDim, full, end);
    }
    if (hot) {
      if (midi) {
        a.setHint(i == 0 ? i18n::tr("Monitor In: this track plays what the MIDI keyboard plays, selected or not (one track at a time: the selected one first)")
                  : i == 1 ? i18n::tr("Monitor Auto: this track plays the MIDI keyboard while it is selected")
                           : i18n::tr("Monitor Off: this track never plays the MIDI keyboard"));
      } else {
        a.setHint(i == 0 ? i18n::tr("Monitor In: the input is always heard through the track")
                  : i == 1 ? i18n::tr("Monitor Auto: the input is heard while the track is armed")
                           : i18n::tr("Monitor Off: the input is never heard (it still records)"));
      }
    }
    if (hot && click && !on) {
      *picked = static_cast<int>(order[i]);
    }
  }
  return over;
}

// ---- a MIDI track's monitoring (per person and Set, never in the song: clips.midiMonitor) ----
std::map<std::string, int> gMidiMon;  // "<set>\t<instrument id>" -> InputMonitor
bool gMidiMonLoaded = false;
void loadMidiMon(const App& a) {
  if (gMidiMonLoaded) {
    return;
  }
  gMidiMonLoaded = true;
  const std::string all = a.settings.getString("clips.midiMonitor");
  size_t at = 0;
  while (at < all.size()) {
    size_t nl = all.find('\n', at);
    if (nl == std::string::npos) {
      nl = all.size();
    }
    const std::string line = all.substr(at, nl - at);
    at = nl + 1;
    const size_t t = line.rfind('\t');
    if (t != std::string::npos && t > 0) {
      gMidiMon[line.substr(0, t)] = std::atoi(line.c_str() + t + 1);
    }
  }
}
std::string midiMonKey(const App& a, ObjectId inst) { return livelook::setKey(a) + "\t" + std::to_string(inst); }

// ---- a MIDI track's MIDI channel (the person's MIDI routing: Settings, MIDI) ----
int routedChannel(const App& a, ObjectId inst) {  // 0..15, or -1: all channels
  if (!a.midi || inst == kNoId) {
    return -1;
  }
  const int idx = a.doc.song().channelIndex(inst);
  const MidiRouting& r = a.midi->routing;
  if (!r.byChannel || idx < 0) {
    return -1;
  }
  for (int c = 0; c < 16; ++c) {
    if (r.route[c] == idx && c != r.omniChannel) {
      return c;
    }
  }
  return -1;
}
void setRoutedChannel(App& a, ObjectId inst, int ch) {
  if (!a.midi || inst == kNoId) {
    return;
  }
  const int idx = a.doc.song().channelIndex(inst);
  if (idx < 0) {
    return;
  }
  MidiRouting& r = a.midi->routing;
  for (int c = 0; c < 16; ++c) {
    if (r.route[c] == idx) {
      r.route[c] = -1;
    }
  }
  if (ch >= 0 && ch < 16) {
    r.route[ch] = idx;
    r.byChannel = true;
  } else {
    bool any = false;
    for (int c = 0; c < 16; ++c) {
      any = any || r.route[c] >= 0;
    }
    r.byChannel = any;
  }
  a.publishMidi();
  a.midi->save((std::filesystem::path(a.configDir()) / "midi.json").string());
}
std::string channelLabel(const App& a, ObjectId inst) {
  const int c = routedChannel(a, inst);
  return c < 0 ? std::string(i18n::tr("All Channels")) : i18n::tr("Ch") + std::string(" ") + std::to_string(c + 1);
}

}  // namespace

bool shown(const App& a) { return a.settings.getBool("clips.showIO"); }

void toggle(App& a) { a.settings.set("clips.showIO", !shown(a)); }

int midiMonitor(const App& a, ObjectId inst) {
  loadMidiMon(a);
  const auto it = gMidiMon.find(midiMonKey(a, inst));
  return it != gMidiMon.end() ? it->second : static_cast<int>(InputMonitor::Auto);
}

void setMidiMonitor(App& a, ObjectId inst, int mon) {
  loadMidiMon(a);
  const std::string k = midiMonKey(a, inst);
  if (mon == static_cast<int>(InputMonitor::Auto)) {
    gMidiMon.erase(k);
  } else {
    gMidiMon[k] = mon;
  }
  std::string all;
  int n = 0;
  for (auto it = gMidiMon.rbegin(); it != gMidiMon.rend() && n < 400; ++it, ++n) {
    all += (all.empty() ? "" : "\n") + it->first + "\t" + std::to_string(it->second);
  }
  a.settings.set("clips.midiMonitor", all);
}

ObjectId midiTarget(const App& a, ObjectId selected) {
  if (!livelook::active(a)) {
    return selected;
  }
  if (selected != kNoId && midiMonitor(a, selected) != static_cast<int>(InputMonitor::Off)) {
    return selected;
  }
  const Song& s = a.doc.song();
  for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t) {
    const ObjectId inst = instrumentOf(s, t);
    if (inst != kNoId && midiMonitor(a, inst) == static_cast<int>(InputMonitor::On)) {
      return inst;
    }
  }
  return selected != kNoId && midiMonitor(a, selected) == static_cast<int>(InputMonitor::Off) ? kNoId : selected;
}

std::string fromLabel(const App& a, int t) {
  const Song& s = a.doc.song();
  switch (kindOf(s, t)) {
    case Kind::Audio: {
      const MixerTrack* m = trackmodes::linkedMixer(s, t);
      return m && m->input >= 0 ? inputName(m->input, m->inputStereo) : std::string(i18n::tr("No input"));
    }
    case Kind::Midi: return i18n::tr("All Ins");
    default: return std::string();
  }
}

std::string toLabel(const App& a, int t) {
  const Song& s = a.doc.song();
  const MixerTrack* m = playedThrough(s, t);
  if (!m) {
    return kindOf(s, t) == Kind::None ? std::string() : std::string(i18n::tr("Main"));
  }
  const ObjectId out = trackmodes::outputOf(*m);
  return out == kNoId ? std::string(i18n::tr("No output")) : destName(s, out);
}

// The Monitor segments' natural width: the three words with 6 px either side.
float monitorWidth(const App& a) {
  float w = 2;
  for (const char* n : {i18n::tr("In"), i18n::tr("Auto"), i18n::tr("Off")}) {
    w += a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, n).x + 12;
  }
  return std::round(w);
}

// Checkpoint 57 (N56-06, N56-07): where the rows go in a block [p0, p1], the same for a track, a return
// and Main, so their rows line up. Labelled (from kLabelledH): the reference's labelled rows with the
// source row; Mid (from kMidH): the labels over From and To, no source row (the grid strips at their
// default height); then three rows, two (From; Monitor | To), or one, with the names in the fields.
struct Rows {
  enum Mode { Labelled, Mid, Three, Two, One } mode = One;
  ImVec2 lf0, lf1, lo0, lo1;  // the From and To labels (Labelled, Mid)
  ImVec2 src0, src1, f0, f1, m0, m1, o0, o1;
};
Rows rowsOf(const App& a, ImVec2 p0, ImVec2 p1) {
  (void)a;
  Rows r;
  const float W = p1.x - p0.x, H = p1.y - p0.y;
  const float lh = 12.0f, fh = 16.0f;
  if (H >= kLabelledH || H >= kMidH) {
    r.mode = H >= kLabelledH ? Rows::Labelled : Rows::Mid;
    float y = p0.y;
    r.lf0 = ImVec2(p0.x, y), r.lf1 = ImVec2(p1.x, y + lh);
    y += lh + 1;
    if (r.mode == Rows::Labelled) {
      r.src0 = ImVec2(p0.x, y), r.src1 = ImVec2(p1.x, y + fh);
      y += fh + 2;
    }
    r.f0 = ImVec2(p0.x, y), r.f1 = ImVec2(p1.x, y + fh);
    y += fh + 3;
    r.m0 = ImVec2(p0.x, y), r.m1 = ImVec2(p1.x, y + fh);
    y += fh + 3;
    r.lo0 = ImVec2(p0.x, y), r.lo1 = ImVec2(p1.x, y + lh);
    y += lh + 1;
    r.o0 = ImVec2(p0.x, y), r.o1 = ImVec2(p1.x, y + fh);
    return r;
  }
  const float rowH = std::min(fh, std::floor((H - 4) / 3.0f));
  const float rowH2 = std::min(fh, std::floor((H - 2) / 2.0f));
  if (rowH >= 12.0f) {
    r.mode = Rows::Three;
    r.f0 = p0, r.f1 = ImVec2(p1.x, p0.y + rowH);
    r.m0 = ImVec2(p0.x, r.f1.y + 2), r.m1 = ImVec2(p1.x, r.m0.y + rowH);
    r.o0 = ImVec2(p0.x, r.m1.y + 2), r.o1 = ImVec2(p1.x, r.o0.y + rowH);
  } else if (rowH2 >= 12.0f) {
    // Monitor sized to its three words ("In Auto Off" in full), To the rest of the row.
    r.mode = Rows::Two;
    r.f0 = p0, r.f1 = ImVec2(p1.x, p0.y + rowH2);
    const float y = r.f1.y + 2;
    const float mw = std::min(std::floor((W - 2) * 0.55f), monitorWidth(a));
    r.m0 = ImVec2(p0.x, y), r.m1 = ImVec2(p0.x + mw, y + rowH2);
    r.o0 = ImVec2(r.m1.x + 2, y), r.o1 = ImVec2(p1.x, y + rowH2);
  } else {
    r.mode = Rows::One;
    const float h = std::min(fh, H), y = p0.y;
    const float w3 = std::floor((W - 4) / 3.0f);
    r.f0 = ImVec2(p0.x, y), r.f1 = ImVec2(p0.x + w3, y + h);
    r.m0 = ImVec2(r.f1.x + 2, y), r.m1 = ImVec2(r.m0.x + w3, y + h);
    r.o0 = ImVec2(r.m1.x + 2, y), r.o1 = ImVec2(p1.x, y + h);
  }
  return r;
}

bool drawBlock(App& a, ImDrawList* dl, int t, ImVec2 p0, ImVec2 p1, bool hovered) {
  const Song& s = a.doc.song();
  const Kind k = kindOf(s, t);
  if (k == Kind::None || p1.x - p0.x < 40 || p1.y - p0.y < 14) {
    return false;
  }
  const ImVec2 m = ImGui::GetIO().MousePos;
  const bool click = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  auto in = [&](ImVec2 a0, ImVec2 a1) { return hovered && m.x >= a0.x && m.x < a1.x && m.y >= a0.y && m.y < a1.y; };
  const Rows L = rowsOf(a, p0, p1);
  const bool labelled = L.mode == Rows::Labelled, mid = L.mode == Rows::Mid;
  const ImVec2 src0 = L.src0, src1 = L.src1, f0 = L.f0, f1 = L.f1;
  const ImVec2 m0 = L.m0, m1 = L.m1, o1 = L.o1;
  ImVec2 o0 = L.o0;
  if (labelled || mid) {
    if (k != Kind::Group) {  // a group has no source
      label(a, dl, L.lf0, L.lf1, k == Kind::Midi ? i18n::tr("MIDI From") : i18n::tr("Audio From"));
    }
    label(a, dl, L.lo0, L.lo1, i18n::tr("Audio To"));
  }
  const ObjectId inst0 = instrumentOf(s, t);
  const bool monitored = k == Kind::Audio || (k == Kind::Midi && inst0 != kNoId);
  if (!monitored && L.mode == Rows::Two) {  // N56 P6-4: no monitor (a MIDI track with no instrument, a group): To takes the row
    o0.x = p0.x;
  }
  const char* fromCap = labelled || mid ? nullptr : i18n::tr("From");
  const char* toCap = labelled || mid ? nullptr : i18n::tr("To");
  bool over = false;
  const ObjectId inst = inst0;
  // The source (labelled only): the device inputs or every MIDI input.
  if (labelled && k != Kind::Group) {
    const bool hot = in(src0, src1);
    over = over || hot;
    field(a, dl, src0, src1, k == Kind::Audio ? i18n::tr("Ext. In") : i18n::tr("All Ins"), false, false, true);
    if (hot) {
      a.setHint(k == Kind::Audio ? i18n::tr("Audio From: the audio device's inputs (the channels are below)")
                                 : i18n::tr("MIDI From: every MIDI input (the channel is below)"));
    }
  }
  // From: an audio track's input; a MIDI track's MIDI channel.
  if (k == Kind::Audio) {
    const bool hot = in(f0, f1);
    over = over || hot;
    field(a, dl, f0, f1, fromLabel(a, t), true, hot, false, fromCap);
    if (hot) {
      a.setHint(i18n::tr("Audio From: the device input this track records and monitors (click to choose)"));
    }
    if (hot && click) {
      openMenu(t, Menu::From, ImVec2(f0.x, f1.y + 2));
    }
  } else if (k == Kind::Midi) {
    const bool hot = in(f0, f1);
    over = over || hot;
    field(a, dl, f0, f1, channelLabel(a, inst), inst != kNoId && a.midi, hot && inst != kNoId, inst == kNoId || !a.midi, fromCap);
    if (hot) {
      a.setHint(i18n::tr("MIDI From: the notes of every MIDI input, on all channels or on one (click to choose; your MIDI settings keep it)"));
    }
    if (hot && click && inst != kNoId && a.midi) {
      openMenu(t, Menu::Channel, ImVec2(f0.x, f1.y + 2));
    }
  }
  // Monitor: In, Auto, Off (an audio track's input; a MIDI track's MIDI keyboard).
  if (k == Kind::Audio) {
    const MixerTrack* lm = trackmodes::linkedMixer(s, t);
    int picked = -1;
    over = monitorRow(a, dl, m0, m1, lm ? lm->monitor : 0, false, hovered, click, &picked) || over;
    if (picked >= 0) {
      trackmodes::setMonitor(a.doc, t, static_cast<InputMonitor>(picked));
    }
  } else if (k == Kind::Midi && inst != kNoId) {
    int picked = -1;
    over = monitorRow(a, dl, m0, m1, midiMonitor(a, inst), true, hovered, click, &picked) || over;
    if (picked >= 0) {
      setMidiMonitor(a, inst, picked);
    }
  }
  // To
  {
    const MixerTrack* pm = playedThrough(s, t);
    const bool hot = in(o0, o1);
    over = over || hot;
    field(a, dl, o0, o1, toLabel(a, t), pm != nullptr, hot && pm, pm == nullptr, toCap);
    if (hot) {
      a.setHint(pm ? i18n::tr("Audio To: where the track's sound goes: Main, a group or a return (click to choose)")
                   : i18n::tr("Audio To: the track plays into Main (it has no mixer track of its own yet)"));
    }
    if (hot && click && pm) {
      openMenu(t, Menu::To, ImVec2(o0.x, o1.y + 2));
    }
  }
  return over;
}

bool drawMixerBlock(App& a, ImDrawList* dl, ObjectId mixer, ImVec2 p0, ImVec2 p1, bool hovered) {
  const Song& s = a.doc.song();
  const MixerTrack* mt = s.mixerTrack(mixer);
  if (!mt || p1.x - p0.x < 40 || p1.y - p0.y < 14) {
    return false;
  }
  const ImVec2 m = ImGui::GetIO().MousePos;
  const bool click = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  auto in = [&](ImVec2 a0, ImVec2 a1) { return hovered && m.x >= a0.x && m.x < a1.x && m.y >= a0.y && m.y < a1.y; };
  const bool master = mixer == s.masterId();
  bool over = false;
  // Main: Cue Out and Master Out (the output device's first pair: Gridline has no separate cue bus, the
  // preview plays through Master Out). A return: Audio To.
  struct Row {
    const char* name;
    std::string value;
    bool menu;
    const char* hint;
  };
  std::vector<Row> items;
  if (master) {
    items.push_back({i18n::tr("Cue Out"), "1/2", false, i18n::tr("Cue Out: the preview and the metronome play through Master Out (Gridline has no separate cue output)")});
    items.push_back({i18n::tr("Master Out"), "1/2", false, i18n::tr("Master Out: Main plays to the audio device's first two outputs (Settings, Audio)")});
  } else {
    const ObjectId out = trackmodes::outputOf(*mt);
    items.push_back({i18n::tr("Audio To"), out == kNoId ? std::string(i18n::tr("No output")) : destName(s, out), true,
                      i18n::tr("Audio To: where the return's sound goes: Main or a later return (click to choose)")});
  }
  // Checkpoint 57 (N56-06): on the tracks' rows (rowsOf): Audio To / Master Out on their To row, Cue Out on
  // their From row, each full width, so every strip's and header's rows line up.
  const Rows L = rowsOf(a, p0, p1);
  const bool withLabels = L.mode == Rows::Labelled || L.mode == Rows::Mid;
  auto put = [&](const Row& r, ImVec2 l0, ImVec2 l1, float y0, float y1) {
    if (withLabels) {
      label(a, dl, l0, l1, r.name);
    }
    const ImVec2 f0(p0.x, y0), f1(p1.x, y1);
    const bool hot = in(f0, f1);
    over = over || hot;
    field(a, dl, f0, f1, withLabels || !master ? r.value : std::string(r.name) + " " + r.value, r.menu, hot && r.menu, !r.menu,
          withLabels || master ? nullptr : i18n::tr("To"));
    if (hot) {
      a.setHint(r.hint);
    }
    if (hot && click && r.menu) {
      openMenu(-1, Menu::ReturnTo, ImVec2(f0.x, f1.y + 2), mixer);
    }
  };
  put(items.back(), L.lo0, L.lo1, L.o0.y, L.o1.y);
  if (items.size() > 1 && L.mode != Rows::One && L.mode != Rows::Two) {
    put(items.front(), L.lf0, L.lf1, L.f0.y, L.f1.y);
  }
  return over;
}

void drawMenus(App& a) {
  ImGui::SetNextWindowPos(gMenuAt, ImGuiCond_Appearing);
  if (!ImGui::BeginPopupEx(kMenuId, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
    return;
  }
  const Song& s = a.doc.song();
  const int t = gMenuTrack;
  auto destMenu = [&](ObjectId from, bool groups) {
    const ObjectId out = s.mixerTrack(from) ? trackmodes::outputOf(*s.mixerTrack(from)) : kNoId;
    std::vector<ObjectId> dests = {s.masterId()};
    if (groups) {
      const int used = trackmodes::usedTrackCount(s);
      for (int g = 0; g < used; ++g) {  // the group tracks
        if (trackmodes::isGroupTrack(s, g)) {
          const ObjectId gm = s.tracks[static_cast<size_t>(g)].mixer;
          if (gm != kNoId && gm != from) {
            dests.push_back(gm);
          }
        }
      }
    }
    for (size_t r : trackmodes::returnTracks(s)) {  // the returns
      if (s.mixer[r].id != from && std::find(dests.begin(), dests.end(), s.mixer[r].id) == dests.end()) {
        dests.push_back(s.mixer[r].id);
      }
    }
    for (ObjectId d : dests) {
      const std::string n = destName(s, d);
      if (n.empty()) {
        continue;
      }
      ImGui::PushID(static_cast<int>(d));
      if (ImGui::MenuItem(n.c_str(), nullptr, d == out, canRoute(s, from, d))) {
        setOutput(a, from, d);
      }
      ImGui::PopID();
    }
  };
  if (gMenu == Menu::From) {
    const MixerTrack* lm = trackmodes::linkedMixer(s, t);
    const int devCh = a.audioInputDevice.empty() ? 0 : a.engine.inputChannels();
    const int listCh = std::max(devCh, 2);
    auto pick = [&](int first, bool stereo) { trackmodes::setInput(a.doc, t, first, stereo); };
    if (ImGui::MenuItem(i18n::tr("No input"), nullptr, lm && lm->input < 0)) {
      pick(-1, true);
    }
    for (int c = 0; c + 1 < listCh; c += 2) {
      const std::string n = inputName(c, true);
      if (ImGui::MenuItem(n.c_str(), nullptr, lm && lm->input == c && lm->inputStereo)) {
        pick(c, true);
      }
    }
    for (int c = 0; c < listCh; ++c) {
      const std::string n = inputName(c, false);
      if (ImGui::MenuItem(n.c_str(), nullptr, lm && lm->input == c && !lm->inputStereo)) {
        pick(c, false);
      }
    }
    if (devCh == 0) {
      ImGui::TextDisabled("%s", i18n::tr("No input device open (Settings, Audio)"));
    }
  } else if (gMenu == Menu::Channel) {
    const ObjectId inst = instrumentOf(s, t);
    const int cur = routedChannel(a, inst);
    if (ImGui::MenuItem(i18n::tr("All Channels"), nullptr, cur < 0)) {
      setRoutedChannel(a, inst, -1);
    }
    for (int c = 0; c < 16; ++c) {
      const std::string n = i18n::tr("Ch") + std::string(" ") + std::to_string(c + 1);
      if (ImGui::MenuItem(n.c_str(), nullptr, cur == c)) {
        setRoutedChannel(a, inst, c);
      }
    }
  } else if (gMenu == Menu::ReturnTo) {
    if (s.mixerTrack(gMenuMixer)) {
      destMenu(gMenuMixer, false);
    }
  } else if (const MixerTrack* pm = playedThrough(s, t)) {
    destMenu(pm->id, true);
  }
  ImGui::EndPopup();
}

}  // namespace gl::liveio
