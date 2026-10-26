// Gridline — playlist track modes UI (PL-08). See TrackModeUi.h.
#include "../core/I18n.h"
#include "TrackModeUi.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include "../core/Mastering.h"
#include "../core/PluginInfo.h"
#include "../core/TrackModes.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::trackui {

using theme::pal;

namespace {

enum Control { kNone = 0, kArm, kMonitor, kInput, kKeys };

int inputCount(const App& a) { return a.audioInputDevice.empty() ? 0 : a.engine.inputChannels(); }

void report(App& a, bool ok, const std::string& why) {
  if (!ok && !why.empty()) a.notify(why, true);
}

// Input choices: stereo pairs, then mono inputs (FL lists both).
void inputItems(App& a, int track, const MixerTrack& m) {
  const int n = inputCount(a);
  if (ImGui::MenuItem(i18n::tr("(none)"), nullptr, m.input < 0)) trackmodes::setInput(a.doc, track, -1, true);
  if (n == 0) {
    ImGui::BeginDisabled();
    ImGui::MenuItem(i18n::tr("No audio input device (Settings \xE2\x86\x92 Audio)"));
    ImGui::EndDisabled();
    return;
  }
  ImGui::SeparatorText(i18n::tr("Stereo"));
  for (int i = 0; i + 1 < n; i += 2)
    if (ImGui::MenuItem(inputName(i, true).c_str(), nullptr, m.input == i && m.inputStereo)) trackmodes::setInput(a.doc, track, i, true);
  ImGui::SeparatorText(i18n::tr("Mono"));
  for (int i = 0; i < n; ++i)
    if (ImGui::MenuItem(inputName(i, false).c_str(), nullptr, m.input == i && !m.inputStereo)) trackmodes::setInput(a.doc, track, i, false);
}

std::string mixerLabel(const Song& s, const MixerTrack& m) {
  const int idx = s.mixerIndex(m.id);
  return "Insert " + std::to_string(idx) + (m.name.empty() ? std::string() : "  " + m.name);
}

// SVG icons on the old glyphs' px grids (input-jack: 6.5 px units, 1.3 px lines;
// speaker: 6 / 7.5 px units, waves 1.2 px).
void inputGlyph(ImDrawList* dl, ImVec2 c, ImU32 col) { ui::drawIconGlyph(dl, ui::Icon::InputJack, c, 6.5f, 1.3f, col); }

void speakerGlyph(ImDrawList* dl, ImVec2 c, ImU32 col, bool waves) {
  if (waves) ui::drawIconGlyph(dl, ui::Icon::SpeakerWaves, c, 7.5f, 1.2f, col);
  else ui::drawIconGlyph(dl, ui::Icon::Speaker, c, 6.0f, 0.0f, col);
}

}  // namespace

void trackMenuItems(App& a, int track) {
  const Song& s = a.doc.song();
  const TrackMode mode = trackmodes::modeOf(s, track);
  if (ImGui::BeginMenu(i18n::tr("Track mode"))) {
    if (ImGui::MenuItem(i18n::tr("Ordinary track"), nullptr, mode == TrackMode::Unassigned, mode != TrackMode::Unassigned))
      trackmodes::unassign(a.doc, track);
    if (ImGui::BeginMenu(i18n::tr("Audio track"))) {
      std::string why;
      if (ImGui::MenuItem(i18n::tr("On the next free mixer track"), nullptr, false)) report(a, trackmodes::makeAudioTrack(a.doc, track, kNoId, &why), why);
      ImGui::SeparatorText(i18n::tr("Link to"));
      int listed = 0;
      for (size_t i = 1; i < s.mixer.size() && listed < 40; ++i) {
        const MixerTrack& m = s.mixer[i];
        const bool cur = mode == TrackMode::Audio && s.tracks[static_cast<size_t>(track)].mixer == m.id;
        if (!cur && !trackmodes::mixerLinkable(s, m.id, nullptr, track)) continue;
        ++listed;
        if (ImGui::MenuItem(mixerLabel(s, m).c_str(), nullptr, cur, !cur)) report(a, trackmodes::makeAudioTrack(a.doc, track, m.id, &why), why);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Instrument track"))) {
      std::string why;
      if (ImGui::BeginMenu(i18n::tr("Play a rack channel"))) {
        int listed = 0;
        for (const auto& c : s.channels) {
          const bool cur = mode == TrackMode::Instrument && s.tracks[static_cast<size_t>(track)].channel == c.id;
          if (!cur && !trackmodes::channelLinkable(s, c.id, nullptr, track)) continue;
          ++listed;
          ImGui::PushID(static_cast<int>(c.id));
          if (ImGui::MenuItem(c.name.c_str(), nullptr, cur, !cur)) report(a, trackmodes::makeInstrumentTrack(a.doc, track, c.id, &why), why);
          ImGui::PopID();
        }
        if (listed == 0) {
          ImGui::BeginDisabled();
          ImGui::MenuItem(i18n::tr("No free instrument channel"));
          ImGui::EndDisabled();
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(i18n::tr("New instrument"))) {
        for (const auto& info : pluginCatalog()) {
          if (info.kind != PluginKind::Generator) continue;
          if (ImGui::MenuItem(info.name.c_str())) {
            const ObjectId ch = trackmodes::newInstrumentTrack(a.doc, track, info.type, &why);
            report(a, ch != kNoId, why);
            if (ch != kNoId) a.selectChannel(ch);
          }
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (mode == TrackMode::Audio) {
    const MixerTrack* m = trackmodes::linkedMixer(s, track);
    ImGui::SeparatorText(i18n::tr("Audio track"));
    if (ImGui::BeginMenu(i18n::tr("Audio input"))) {
      inputItems(a, track, *m);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Arm for recording"), nullptr, m->armed, !m->armed)) trackmodes::setArmed(a.doc, track, true);
    if (ImGui::MenuItem(i18n::tr("Disarm"), nullptr, false, m->armed)) trackmodes::setArmed(a.doc, track, false);
    if (ImGui::BeginMenu(i18n::tr("Input monitoring"))) {
      static const char* names[] = {"Auto (while armed or stopped)", "Always", "Never"};
      for (int k = 0; k < static_cast<int>(InputMonitor::Count); ++k)
        if (ImGui::MenuItem(names[k], nullptr, m->monitor == k)) trackmodes::setMonitor(a.doc, track, static_cast<InputMonitor>(k));
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Show its mixer track"))) {
      a.selMixer = m->id;
      a.showMixer = true;
      a.focusWindow = "Mixer";
    }
  } else if (mode == TrackMode::Instrument) {
    const Channel* c = trackmodes::linkedChannel(s, track);
    ImGui::SeparatorText(i18n::tr("Instrument track"));
    if (ImGui::MenuItem(i18n::tr("Select its channel"))) a.selectChannel(c->id);
    if (const MixerTrack* m = trackmodes::linkedMixer(s, track); m && ImGui::MenuItem(i18n::tr("Show its mixer track"))) {
      a.selMixer = m->id;
      a.showMixer = true;
      a.focusWindow = "Mixer";
    }
  }
}

float drawNameGlyph(App& a, ImDrawList* dl, int track, ImVec2 p) {
  const auto& P = pal();
  const TrackMode mode = trackmodes::modeOf(a.doc.song(), track);
  if (mode == TrackMode::Audio) {
    ui::drawIcon(dl, ui::Icon::Wave, ImVec2(p.x + 5, p.y + theme::kFontCaption * 0.5f + 1), 10, theme::rgb(theme::role::kStamp));
    return 13;
  }
  if (mode == TrackMode::Instrument) {
    ui::drawIcon(dl, ui::Icon::Keyboard, ImVec2(p.x + 5, p.y + theme::kFontCaption * 0.5f + 1), 10, P.textDim);
    return 13;
  }
  return 0;
}

int drawHeader(App& a, ImDrawList* dl, int track, float x0, float x1, float lineY, bool hovHead, bool roomy) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const TrackMode mode = trackmodes::modeOf(s, track);
  if (mode == TrackMode::Unassigned || !roomy) return kNone;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  auto hot = [&](ImVec2 c, float r) { return hovHead && std::fabs(mouse.x - c.x) <= r && std::fabs(mouse.y - c.y) <= r; };
  int over = kNone;
  if (mode == TrackMode::Audio) {
    const MixerTrack* m = trackmodes::linkedMixer(s, track);
    // Right to left before the mute switch: arm, monitoring; the input name after the handle.
    const ImVec2 arm(x1 - 7, lineY);
    if (arm.x - 6 < x0) return kNone;
    const bool armHot = hot(arm, 7);
    ui::drawIconFitC(dl, ui::Icon::Record, arm, ImVec2(10, 10), m->armed ? P.danger : theme::mix(P.ink, P.surface800, 0.4f));
    ui::drawIconFitC(dl, ui::Icon::Ring, arm, ImVec2(11.2f, 11.2f), armHot ? P.hot : m->armed ? theme::mix(P.danger, P.paper, 0.3f) : P.textFaint);
    if (armHot) over = kArm;
    const ImVec2 mon(arm.x - 17, lineY);
    if (mon.x - 7 > x0) {
      const bool monOn = m->monitor == static_cast<uint8_t>(InputMonitor::On) ||
                         (m->monitor == static_cast<uint8_t>(InputMonitor::Auto) && (m->armed || !a.engine.playing()) && m->input >= 0);
      const bool monHot = hot(mon, 7);
      const ImU32 col = monHot ? P.hot : m->monitor == static_cast<uint8_t>(InputMonitor::Off) ? P.textFaint
                                        : monOn ? theme::rgb(theme::role::kStamp) : P.textDim;
      speakerGlyph(dl, ImVec2(mon.x + 1, mon.y), col, m->monitor != static_cast<uint8_t>(InputMonitor::Off));
      if (monHot) over = kMonitor;
    }
    const float ix = x0 + 2, iw = mon.x - 10 - ix;
    if (iw > 28) {
      const bool inHot = hovHead && mouse.x >= ix && mouse.x < ix + iw && std::fabs(mouse.y - lineY) < 7;
      inputGlyph(dl, ImVec2(ix + 4, lineY), inHot ? P.hot : P.textDim);
      const std::string in = m->input >= 0 ? inputName(m->input, m->inputStereo) : std::string("No input");
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption - 1, ImVec2(ix + 13, lineY - theme::kFontCaption * 0.5f), iw - 13,
                          inHot ? P.text : m->input >= 0 ? P.textDim : P.textFaint, in.c_str());
      if (inHot && over == kNone) over = kInput;
    }
  } else {
    const Channel* c = trackmodes::linkedChannel(s, track);
    const float ix = x0 + 2, iw = x1 - ix;
    if (iw > 28 && c) {
      const bool hotK = hovHead && mouse.x >= ix && mouse.x < x1 && std::fabs(mouse.y - lineY) < 7;
      const bool sel = a.selChannel == c->id;
      ui::drawIcon(dl, ui::Icon::Piano, ImVec2(ix + 5, lineY), 10, hotK || sel ? P.hot : P.textDim);
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption - 1, ImVec2(ix + 14, lineY - theme::kFontCaption * 0.5f), iw - 14,
                          hotK ? P.text : P.textDim, c->name.c_str());
      if (hotK) over = kKeys;
    }
  }
  return over;
}

void headerClick(App& a, int track, int control) {
  const Song& s = a.doc.song();
  switch (control) {
    case kArm:
      if (const MixerTrack* m = trackmodes::linkedMixer(s, track)) trackmodes::setArmed(a.doc, track, !m->armed);
      break;
    case kMonitor:
      if (const MixerTrack* m = trackmodes::linkedMixer(s, track))  // auto → always → never → auto
        trackmodes::setMonitor(a.doc, track, static_cast<InputMonitor>((m->monitor + 1) % static_cast<int>(InputMonitor::Count)));
      break;
    case kInput: ImGui::OpenPopup("##trackinput"); break;
    case kKeys:
      if (const Channel* c = trackmodes::linkedChannel(s, track)) a.selectChannel(c->id);
      break;
    default: break;
  }
}

void inputPopup(App& a, int track) {
  if (!ImGui::BeginPopup("##trackinput")) return;
  if (const MixerTrack* m = trackmodes::modeOf(a.doc.song(), track) == TrackMode::Audio ? trackmodes::linkedMixer(a.doc.song(), track) : nullptr) {
    ImGui::SeparatorText(i18n::tr("Audio input"));
    inputItems(a, track, *m);
  } else {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

const char* headerHint(App& a, int track, int control) {
  static std::string h;
  const Song& s = a.doc.song();
  switch (control) {
    case kArm: return "Arm / disarm the track's mixer track for recording   (recordings land on this track)";
    case kMonitor: return "Input monitoring: auto (while armed or stopped) / always / never   (click to step)";
    case kInput: return "Audio input of the track   (click to choose)";
    case kKeys: return "Instrument track   (click: select the channel, MIDI input plays it)";
    default: break;
  }
  const TrackMode mode = trackmodes::modeOf(s, track);
  if (mode == TrackMode::Audio) {
    h = "Audio track \xC2\xB7 mixer insert " + std::to_string(s.mixerIndex(trackmodes::linkedMixer(s, track)->id)) +
        "   (right-click: track menu \xE2\x86\x92 track mode, input, arm)";
    return h.c_str();
  }
  if (mode == TrackMode::Instrument) {
    h = "Instrument track \xC2\xB7 " + trackmodes::linkedChannel(s, track)->name + "   (pattern clips only; right-click: track menu)";
    return h.c_str();
  }
  return "";
}

void devDemo(App& a) {
  // Screenshots: GRIDLINE_PL08_DEMO=1 turns three playlist tracks of the
  // loaded song into an armed audio track, a second audio track and an
  // instrument track; GRIDLINE_MASTER_DEMO=1 puts a master chain on.
  static int frames = 0;
  ++frames;
  // GRIDLINE_MI02_DEMO=1: the transport follows a virtual MTC master (25 fps,
  // from 00:59:58:00) fed into the time code receiver as if from a device.
  if (const char* e = std::getenv("GRIDLINE_MI02_DEMO"); e && *e && a.midi && frames >= 3) {
    static MidiEventRing ring;
    static MidiClockOut gen;
    static double t0 = -1, done = 0;
    const double sr = 48000, start = 3598.0, now = MidiManager::clockNow();
    if (t0 < 0) {
      t0 = now;
      a.midi->syncSource = SyncSource::Mtc;
      gen.out = &ring;
      gen.setMode(SyncOut::Mtc25);
      gen.setSampleRate(sr);
      gen.start(0, start, 0);
    }
    const double upTo = now - t0;
    const int64_t at = static_cast<int64_t>(done * sr), len = static_cast<int64_t>(upTo * sr) - at;
    if (len > 0) {
      gen.segment(0, 0, start + done, start + done + len / sr, at, static_cast<int>(len), [](double) { return 0.0; });
      done += len / sr;
      std::lock_guard<std::mutex> lk(a.midi->followerMutex());
      MidiOutEvent ev;
      while (ring.pop(ev)) a.midi->mtc().message(ev.data, ev.size, t0 + ev.sample / sr);
    }
  }
  if (frames != 3) return;
  if (const char* e = std::getenv("GRIDLINE_PL08_DEMO"); e && *e) {
    a.doc.editSong("PL-08 demo", [](Song& s) {
      if (s.tracks.size() < 8) s.tracks.resize(8);
      const int t0 = trackmodes::freeTrack(s, 2);
      if (t0 < 0) return;
      s.tracks[static_cast<size_t>(t0)].name = "Lead vocal";
      s.tracks[static_cast<size_t>(t0)].color = 0xC8724E;
      trackmodes::makeAudioTrack(s, t0);
      trackmodes::setArmed(s, t0, true);
      trackmodes::setInput(s, t0, 0, false);
      const int t1 = trackmodes::freeTrack(s, t0 + 1);
      if (t1 >= 0) {
        s.tracks[static_cast<size_t>(t1)].name = "Guitar DI";
        trackmodes::makeAudioTrack(s, t1);
        trackmodes::setInput(s, t1, 0, true);
        trackmodes::setMonitor(s, t1, InputMonitor::Off);
      }
      const int t2 = trackmodes::freeTrack(s, t1 + 1);
      for (const auto& c : s.channels)
        if (t2 >= 0 && trackmodes::channelLinkable(s, c.id)) {
          trackmodes::makeInstrumentTrack(s, t2, c.id);
          break;
        }
    });
  }
  if (const char* e = std::getenv("GRIDLINE_MASTER_DEMO"); e && *e) {
    mastering::installMasterChain(a.doc, mastering::presets()[1].macros);
    a.showMaster = true;
  }
}

void mixerMenuItems(App& a, ObjectId mixer) {
  const Song& s = a.doc.song();
  if (mixer == s.masterId()) return;
  const int linked = trackmodes::audioTrackOf(s, mixer);
  std::string why;
  if (ImGui::MenuItem(i18n::tr("Make an audio track for it"), nullptr, false, linked < 0)) {
    const int t = trackmodes::assignMixerToNewTrack(a.doc, mixer, &why);
    report(a, t >= 0, why);
    if (t >= 0) a.notify("Audio track on playlist track " + std::to_string(t + 1));
  }
  if (ImGui::MenuItem(i18n::tr("Drop its audio track link"), nullptr, false, linked >= 0)) trackmodes::unassignMixer(a.doc, mixer);
}

void channelMenuItems(App& a, ObjectId channel) {
  const Song& s = a.doc.song();
  const int linked = trackmodes::instrumentTrackOf(s, channel);
  std::string why;
  if (ImGui::MenuItem(i18n::tr("Make an instrument track for it"), nullptr, false, linked < 0 && trackmodes::channelLinkable(s, channel))) {
    const int t = trackmodes::assignChannelToNewTrack(a.doc, channel, &why);
    report(a, t >= 0, why);
    if (t >= 0) a.notify("Instrument track on playlist track " + std::to_string(t + 1));
  }
  if (ImGui::MenuItem(i18n::tr("Drop its instrument track link"), nullptr, false, linked >= 0)) trackmodes::unassignChannel(a.doc, channel);
}

}  // namespace gl::trackui
