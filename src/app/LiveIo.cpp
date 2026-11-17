// Gridline - the Clips layout's In/Out section (K-112c checkpoint 55). See LiveIo.h.
#include "LiveIo.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/TrackModes.h"
#include "App.h"
#include "AudioRecordUi.h"
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

// The menus: one open at a time, for one track.
const ImGuiID kMenuId = ImHashStr("##clips_io_menu");
int gMenuTrack = -1;
bool gMenuTo = false;
ImVec2 gMenuAt;

void openMenu(int track, bool to, ImVec2 at) {
  gMenuTrack = track, gMenuTo = to, gMenuAt = at;
  ImGui::OpenPopupEx(kMenuId, ImGuiPopupFlags_None);
}

void setOutput(App& a, ObjectId from, ObjectId to) {
  const Song& s = a.doc.song();
  if (from == kNoId || from == to || !canRoute(s, from, to)) {
    a.notify(i18n::tr("That would play the track into itself"), true);
    return;
  }
  a.doc.editMixerTrack(from, "track output", [&](MixerTrack& m) {
    // The output is the first send that is no sidechain; an aux send to the new output goes (it is the output now).
    int out = -1;
    for (size_t i = 0; i < m.sends.size() && out < 0; ++i) {
      if (!m.sends[i].sidechain && !m.sends[i].stageRoom) {
        out = static_cast<int>(i);
      }
    }
    for (size_t i = m.sends.size(); i-- > 0;) {
      if (static_cast<int>(i) != out && m.sends[i].to == to && !m.sends[i].sidechain) {
        m.sends.erase(m.sends.begin() + static_cast<std::ptrdiff_t>(i));
        if (static_cast<int>(i) < out) {
          --out;
        }
      }
    }
    if (out >= 0) {
      m.sends[static_cast<size_t>(out)].to = to;
    } else {
      m.sends.insert(m.sends.begin(), Send{to, 1.0f, false});
    }
  });
}

// One field: a recessed box with its text and a small down arrow when it opens a menu.
void field(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const std::string& text, bool menu, bool hot, bool dim) {
  const auto& P = theme::pal();
  dl->AddRectFilled(p0, p1, P.dark ? P.field : P.surface950, theme::kRadiusSm);
  if (hot) {
    dl->AddRect(p0, p1, theme::withAlpha(P.text, 0.35f), theme::kRadiusSm);
  }
  const float fs = theme::kFontLabel;
  const float aw = menu ? 9.0f : 0.0f;
  dl->PushClipRect(p0, p1, true);
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(p0.x + 4, theme::textTop(a.fontRegular, fs, p0.y, p1.y - p0.y)), p1.x - p0.x - 8 - aw,
                      dim ? P.textFaint : P.text, text.c_str());
  dl->PopClipRect();
  if (menu) {
    const float cx = p1.x - 7, cy = (p0.y + p1.y) * 0.5f;
    dl->AddTriangleFilled(ImVec2(cx - 3, cy - 1.5f), ImVec2(cx + 3, cy - 1.5f), ImVec2(cx, cy + 2), dim ? P.textFaint : P.textDim);
  }
}

}  // namespace

bool shown(const App& a) { return a.settings.getBool("clips.showIO"); }

void toggle(App& a) { a.settings.set("clips.showIO", !shown(a)); }

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

bool drawBlock(App& a, ImDrawList* dl, int t, ImVec2 p0, ImVec2 p1, bool hovered) {
  const auto& P = theme::pal();
  const Song& s = a.doc.song();
  const Kind k = kindOf(s, t);
  if (k == Kind::None || p1.x - p0.x < 40 || p1.y - p0.y < 14) {
    return false;
  }
  const ImVec2 m = ImGui::GetIO().MousePos;
  const bool click = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  auto in = [&](ImVec2 a0, ImVec2 a1) { return hovered && m.x >= a0.x && m.x < a1.x && m.y >= a0.y && m.y < a1.y; };
  const float rowH = std::min(16.0f, std::floor((p1.y - p0.y - 4) / 3.0f));
  const bool rows = rowH >= 12.0f;  // three rows; else two (From, then monitor | To), else one row
  const float rowH2 = std::min(16.0f, std::floor((p1.y - p0.y - 2) / 2.0f));
  const bool two = !rows && rowH2 >= 12.0f;
  const float W = p1.x - p0.x;
  ImVec2 f0, f1, m0, m1, o0, o1;
  if (two) {
    f0 = p0, f1 = ImVec2(p1.x, p0.y + rowH2);
    const float y = f1.y + 2, half = std::floor((W - 2) * 0.5f);
    m0 = ImVec2(p0.x, y), m1 = ImVec2(p0.x + half, y + rowH2);
    o0 = ImVec2(m1.x + 2, y), o1 = ImVec2(p1.x, y + rowH2);
  } else if (rows) {
    f0 = p0, f1 = ImVec2(p1.x, p0.y + rowH);
    m0 = ImVec2(p0.x, f1.y + 2), m1 = ImVec2(p1.x, m0.y + rowH);
    o0 = ImVec2(p0.x, m1.y + 2), o1 = ImVec2(p1.x, o0.y + rowH);
  } else {
    const float h = std::min(16.0f, p1.y - p0.y), y = p0.y;
    const float w3 = std::floor((W - 4) / 3.0f);
    f0 = ImVec2(p0.x, y), f1 = ImVec2(p0.x + w3, y + h);
    m0 = ImVec2(f1.x + 2, y), m1 = ImVec2(m0.x + w3, y + h);
    o0 = ImVec2(m1.x + 2, y), o1 = ImVec2(p1.x, y + h);
  }
  bool over = false;
  // From
  if (k != Kind::Group) {
    const bool hot = in(f0, f1);
    over = over || hot;
    field(a, dl, f0, f1, fromLabel(a, t), k == Kind::Audio, hot && k == Kind::Audio, false);
    if (hot) {
      a.setHint(k == Kind::Audio ? i18n::tr("Audio From: the device input this track records and monitors (click to choose)")
                                 : i18n::tr("MIDI From: this track plays the notes of every MIDI input while it is armed or selected"));
    }
    if (hot && click && k == Kind::Audio) {
      openMenu(t, false, ImVec2(f0.x, f1.y + 2));
    }
  }
  // Monitor: In, Auto, Off (an audio track's input monitoring)
  if (k == Kind::Audio) {
    const MixerTrack* lm = trackmodes::linkedMixer(s, t);
    const int mon = lm ? lm->monitor : 0;
    const InputMonitor order[3] = {InputMonitor::On, InputMonitor::Auto, InputMonitor::Off};
    const char* names[3] = {i18n::tr("In"), i18n::tr("Auto"), i18n::tr("Off")};
    const float sw = std::floor((m1.x - m0.x - 2) / 3.0f);
    for (int i = 0; i < 3; ++i) {
      const ImVec2 a0(m0.x + static_cast<float>(i) * (sw + 1), m0.y), a1(i == 2 ? m1.x : a0.x + sw, m1.y);
      const bool on = mon == static_cast<int>(order[i]);
      const bool hot = in(a0, a1);
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
        a.setHint(i == 0 ? i18n::tr("Monitor In: the input is always heard through the track")
                  : i == 1 ? i18n::tr("Monitor Auto: the input is heard while the track is armed")
                           : i18n::tr("Monitor Off: the input is never heard (it still records)"));
      }
      if (hot && click && !on) {
        trackmodes::setMonitor(a.doc, t, order[i]);
      }
    }
  } else if (rows && k == Kind::Midi) {
    const char* lb = i18n::tr("Monitor: Auto");
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(m0.x + 4, theme::textTop(a.fontRegular, theme::kFontLabel, m0.y, m1.y - m0.y)), P.textFaint, lb);
  }
  // To
  {
    const MixerTrack* pm = playedThrough(s, t);
    const bool hot = in(o0, o1);
    over = over || hot;
    field(a, dl, o0, o1, toLabel(a, t), pm != nullptr, hot && pm, pm == nullptr);
    if (hot) {
      a.setHint(pm ? i18n::tr("Audio To: where the track's sound goes: Main, a group or a return (click to choose)")
                   : i18n::tr("Audio To: the track plays into Main (it has no mixer track of its own yet)"));
    }
    if (hot && click && pm) {
      openMenu(t, true, ImVec2(o0.x, o1.y + 2));
    }
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
  if (!gMenuTo) {
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
  } else if (const MixerTrack* pm = playedThrough(s, t)) {
    const ObjectId from = pm->id, out = trackmodes::outputOf(*pm);
    std::vector<ObjectId> dests = {s.masterId()};
    const int used = trackmodes::usedTrackCount(s);
    for (int g = 0; g < used; ++g) {  // the group tracks
      if (trackmodes::isGroupTrack(s, g)) {
        const ObjectId gm = s.tracks[static_cast<size_t>(g)].mixer;
        if (gm != kNoId && gm != from) {
          dests.push_back(gm);
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
  }
  ImGui::EndPopup();
}

}  // namespace gl::liveio
