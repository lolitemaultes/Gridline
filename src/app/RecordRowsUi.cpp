// Gridline - K-123 (0.7.7 recording rows). See RecordRowsUi.h.
#include "RecordRowsUi.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../core/I18n.h"
#include "../core/Meter.h"
#include "../core/MidiRecord.h"
#include "../core/Timeline.h"
#include "../core/TrackModes.h"
#include "../engine/TakeLanes.h"
#include "App.h"
#include "LiveLook.h"
#include "MidiArm.h"
#include "RecordCard.h"
#include "RecordLook.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::recordrows {

using theme::pal;

namespace {

struct Listen {
  int track = -1, take = -1;
  bool audition = false;
  Tick returnTo = 0;     // an audition: where the song position goes back to
  PlayMode mode = PlayMode::Song;  // and the play mode it had
  int frames = 0;        // frames since the audition started (the transport takes a moment to start)
};
Listen& L() {
  static Listen l;
  return l;
}

// What the pointer is on in the lane headers this frame: 1..3 a rating dot, 4 the solo mark.
struct LaneHit {
  int track = -1, take = -1, what = 0;
  int frame = -1;
};
LaneHit gLaneHit;
bool gLastTimed = false;  // the last Recover put the notes where they were played (dev output)

std::string trackLabel(const Song& s, int track) {
  if (track < 0 || static_cast<size_t>(track) >= s.tracks.size()) return "Track " + std::to_string(track + 1);
  return playlistTrackName(s, static_cast<size_t>(track));
}

std::string barText(const Song& s, Tick t) {
  const MeterMap::Bbt b = MeterMap(s).toBbt(t);
  return std::to_string(b.bar + 1) + "." + std::to_string(b.beat + 1);
}

// Rating dots: three small circles, the first `rating` filled. Returns the dot under the pointer (1..3, 0 none).
int ratingDots(ImDrawList* dl, ImVec2 left, float cy, int rating, bool hovRow, float step = 9.0f, float r = 3.0f) {
  const auto& P = pal();
  const ImVec2 m = ImGui::GetIO().MousePos;
  int over = 0;
  for (int k = 1; k <= takes::kMaxRating; ++k) {
    const ImVec2 c(left.x + r + static_cast<float>(k - 1) * step, cy);
    if (hovRow && std::fabs(m.x - c.x) <= step * 0.5f && std::fabs(m.y - c.y) <= r + 4) over = k;
  }
  for (int k = 1; k <= takes::kMaxRating; ++k) {
    const ImVec2 c(left.x + r + static_cast<float>(k - 1) * step, cy);
    const bool lit = over > 0 ? k <= over : k <= rating;
    const ImU32 on = over > 0 ? theme::withAlpha(P.accent, 0.8f) : P.accent;
    if (lit) {
      dl->AddCircleFilled(c, r, on, 12);
    } else {
      dl->AddCircle(c, r - 0.5f, hovRow ? P.textDim : P.textFaint, 12, 1.1f);
    }
  }
  return over;
}

}  // namespace

// ---------------------------------------------------------------- takes ----

void rate(App& a, int track, int take, int value) {
  const int v = std::clamp(value, 0, takes::kMaxRating);
  if (takes::rating(a.doc.song(), track, take) == v) return;
  a.doc.editSong("rate take", [&](Song& x) { takes::setRating(x, track, take, v); });
}

bool soloed(int track, int take) { return L().track == track && L().take == take && L().track >= 0; }
int soloTrack() { return L().track; }
bool auditioning() { return L().audition && L().track >= 0; }

void stopListening(App& a) {
  Listen& l = L();
  if (l.track < 0) return;
  const bool aud = l.audition;
  const Tick back = l.returnTo;
  const PlayMode mode = l.mode;
  l = Listen{};
  if (aud) {
    if (a.engine.playing()) a.engine.pause();
    if (a.engine.mode() != mode) a.engine.setMode(mode);
    a.engine.setPosition(back);
  }
  a.forceRebuild();
}

void solo(App& a, int track, int take) {
  if (soloed(track, take) && !L().audition) {
    stopListening(a);
    return;
  }
  if (L().audition) stopListening(a);
  L() = Listen{};
  L().track = track, L().take = take;
  a.forceRebuild();
}

void audition(App& a, int track, int take) {
  if (a.recordArmed) {
    a.notify(i18n::tr("Turn Record off to audition a take"), true);
    return;
  }
  std::vector<takes::TakeInfo> list = takes::takeList(a.doc.song(), track);
  const auto it = std::find_if(list.begin(), list.end(), [take](const takes::TakeInfo& t) { return t.take == take; });
  if (it == list.end()) return;
  if (L().track >= 0) stopListening(a);
  Listen& l = L();
  l.track = track, l.take = take, l.audition = true, l.frames = 0;
  l.returnTo = static_cast<Tick>(std::llround(std::max(0.0, a.engine.position())));
  l.mode = a.engine.mode();
  if (a.engine.playing()) a.engine.pause();
  if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);
  a.forceRebuild();
  a.engine.setPosition(it->start);
  a.engine.play();
}

void listenFilter(App& a, Song& played) {
  Listen& l = L();
  if (l.track < 0) return;
  if (!takes::listenTo(played, l.track, l.take)) {  // the take is gone (undone, removed, a collaborator's edit)
    const bool aud = l.audition;
    l = Listen{};
    if (aud) a.later([&a] { a.engine.pause(); });
  }
}

void frame(App& a) {
  Listen& l = L();
  if (l.track < 0 || !l.audition) return;
  ++l.frames;
  if (l.frames > 2 && !a.engine.playing()) stopListening(a);
}

namespace {
constexpr float kWideHeader = 104;   // room for "Take N" beside the controls
constexpr float kNarrowHeader = 60;  // below this: the label and the comp dot only
}  // namespace

std::string laneLabel(int take, float width) {
  return (width < kWideHeader && width >= kNarrowHeader ? std::string() : std::string("Take ")) + std::to_string(take + 1);
}

float drawLaneHeader(App& a, ImDrawList* dl, int track, int take, ImVec2 h0, ImVec2 h1, bool hov) {
  const auto& P = pal();
  const float w = h1.x - h0.x;
  if (w < kNarrowHeader) return h1.x - 20;  // too narrow: the label and the comp dot only
  const bool compact = w < kWideHeader;   // the label is the take's number: smaller controls
  const Song& s = a.doc.song();
  const float cy = std::round((h0.y + h1.y) * 0.5f);
  const int r = takes::rating(s, track, take);
  const bool solo = soloed(track, take);
  // Right to left from the comp dot's column: the solo mark, then the rating dots.
  const float sx1 = h1.x - (compact ? 17 : 20), sx0 = sx1 - (compact ? 12 : 14);
  const ImVec2 m = ImGui::GetIO().MousePos;
  const bool sHot = hov && m.x >= sx0 - 1 && m.x < sx1 + 1 && std::fabs(m.y - cy) <= 8;
  if (solo || hov) {
    const ImVec2 b0(sx0, cy - 6), b1(sx1, cy + 6);
    if (solo) dl->AddRectFilled(b0, b1, theme::withAlpha(P.solo, sHot ? 1.0f : 0.9f), 3.0f);
    else dl->AddRect(b0, b1, sHot ? P.text : P.textFaint, 3.0f);
    theme::centreText(dl, a.fontBold, theme::kFontCaption - 2, b0, b1, solo ? P.ink : sHot ? P.text : P.textDim, "S");
  }
  const float step = compact ? 7.0f : 9.0f, dotR = compact ? 2.5f : 3.0f;
  const float dx0 = sx0 - (compact ? 4 : 6) - 2 * step - 2 * dotR;
  const int over = (r > 0 || hov) ? ratingDots(dl, ImVec2(dx0, cy), cy, r, hov, step, dotR) : 0;
  if (hov) {
    gLaneHit = LaneHit{track, take, sHot ? 4 : over, ImGui::GetFrameCount()};
    if (sHot) a.setHint(solo ? "Solo: only this take plays on the track (listening only, the comp stays) \xC2\xB7 click: back to the comp"
                              : "Solo this take: hear it alone on the track without changing the comp");
    else if (over > 0) a.setHint("Rate this take " + std::to_string(over) + " of 3 (click the lit last dot again: no rating)");
  }
  return dx0 - 4;
}

bool laneHeaderClick(App& a, int track, int take) {
  const LaneHit h = gLaneHit;
  if (h.frame != ImGui::GetFrameCount() || h.track != track || h.take != take || h.what == 0) return false;
  if (h.what == 4) {
    solo(a, track, take);
  } else {
    rate(a, track, take, takes::rating(a.doc.song(), track, take) == h.what ? 0 : h.what);
  }
  return true;
}

void laneMenuItems(App& a, int track, int take) {
  const Song& s = a.doc.song();
  if (track < 0 || take < 0 || take >= takes::laneCount(s, track)) return;
  const std::string t = "Take " + std::to_string(take + 1);
  ImGui::TextDisabled("%s", t.c_str());
  if (ImGui::BeginMenu(i18n::tr("Rating"))) {
    static const char* kNames[] = {i18n::N_("No rating"), i18n::N_("One dot"), i18n::N_("Two dots"), i18n::N_("Three dots")};
    const int r = takes::rating(s, track, take);
    for (int k = 0; k <= takes::kMaxRating; ++k) {
      if (ImGui::MenuItem(i18n::tr(kNames[k]), nullptr, r == k)) rate(a, track, take, k);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Solo this take"), nullptr, soloed(track, take) && !auditioning())) solo(a, track, take);
  ui::Hint("Hear only this take on the track; the comp stays as it is");
  if (ImGui::MenuItem(i18n::tr("Audition from its start"), nullptr, false, !a.recordArmed)) audition(a, track, take);
  ui::Hint("Plays this take alone from where it starts; stopping goes back to where the song position was");
  ImGui::Separator();
}

void takesList(App& a, bool recording) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const std::vector<int> tracks = takes::tracksWithTakes(s);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  reclook::rlSection("TAKES ON THE TRACKS");  // K-325: a section of the Takes page
  if (tracks.empty()) {
    reclook::rlValue(i18n::tr("No takes yet: loop passes of a recording land here, one row per take."), P.textFaint);
    return;
  }
  // K-325: one grid for the rows: the name, the bars, then from the right Audition and Solo (one
  // width, so nothing moves when Audition reads Stop) and the rating dots just before them.
  const bool trk = reclook::tracker();
  const float fs = trk ? ImGui::GetFontSize() : theme::kFontBody - 1, cs = trk ? ImGui::GetFontSize() : theme::kFontCaption;
  const float bh = reclook::buttonHeight(), rowH = std::max(24.0f, bh + 4);
  const float bw = std::max({reclook::buttonWidth("Audition"), reclook::buttonWidth("Stop"), reclook::buttonWidth("Solo")});
  const float dotStep = 12.0f, dotR = 4.0f, dotsW = 2 * dotStep + 2 * dotR;
  for (int tr : tracks) {
    ImGui::PushID(tr);
    const std::vector<takes::TakeInfo> list = takes::takeList(s, tr);
    {  // the track's name
      const ImVec2 p = ImGui::GetCursorScreenPos();
      dl->AddText(a.fontBold, fs, ImVec2(p.x, p.y + 1), P.text, trackLabel(s, tr).c_str());
      const std::string n = std::to_string(list.size()) + (list.size() == 1 ? " take" : " takes");
      const float nx = p.x + a.fontBold->CalcTextSizeA(fs, FLT_MAX, 0, trackLabel(s, tr).c_str()).x + 8;
      dl->AddText(a.fontRegular, cs, ImVec2(nx, p.y + 1 + fs - cs), P.textFaint, n.c_str());
      ImGui::Dummy(ImVec2(0, std::ceil(fs + 4)));
    }
    for (const takes::TakeInfo& t : list) {
      ImGui::PushID(t.take);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float W = ImGui::GetContentRegionAvail().x;
      const bool hovRow = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + W, p.y + rowH)) && ImGui::IsWindowHovered();
      if (hovRow) dl->AddRectFilled(p, ImVec2(p.x + W, p.y + rowH), theme::withAlpha(P.surface700, 0.5f), trk ? 0.0f : 4.0f);
      const float cy = std::round(p.y + rowH * 0.5f);
      // In the comp: a lit mark at the left.
      dl->AddRectFilled(ImVec2(p.x + 2, cy - 7), ImVec2(p.x + 5, cy + 7), t.inComp ? P.accent : P.lineStrong, 1.5f);
      const std::string name = "Take " + std::to_string(t.take + 1);
      dl->AddText(a.fontRegular, fs, ImVec2(p.x + 14, std::round(cy - fs * 0.5f - 1)), t.inComp ? P.text : P.textDim, name.c_str());
      const std::string range = "bar " + barText(s, t.start) + " to " + barText(s, t.end) + (t.inComp ? "  \xC2\xB7  in the comp" : "");
      const float bx1 = p.x + W - 4, ax = bx1 - bw, sx = ax - 8 - bw;  // Audition, then Solo, from the right
      const float dx = sx - 16 - dotsW;                                  // the dots' left edge
      theme::textEllipsis(dl, a.fontRegular, cs, ImVec2(p.x + 80, std::round(cy - cs * 0.5f - 1)), std::max(20.0f, dx - 16 - (p.x + 80)), P.textFaint, range.c_str());
      // The rating dots (click: rate; the lit last dot again: none).
      const int over = ratingDots(dl, ImVec2(dx, cy), cy, t.rating, hovRow, dotStep, dotR);
      ImGui::SetCursorScreenPos(ImVec2(dx - 4, p.y));
      if (ImGui::InvisibleButton("##rate", ImVec2(dotsW + 8, rowH)) && over > 0) rate(a, tr, t.take, t.rating == over ? 0 : over);
      ui::Hint("Rating: one to three dots (click the lit last dot again: no rating). Saved with the song and shared");
      // Solo and audition at the right.
      const bool aud = auditioning() && soloed(tr, t.take);
      const float by = std::round(cy - bh * 0.5f);
      ImGui::SetCursorScreenPos(ImVec2(sx, by));
      if (reclook::rlButton(a, "Solo", reclook::Kind::Normal, bw, "Hear only this take on its track; the comp stays as it is", soloed(tr, t.take) && !auditioning())) {
        solo(a, tr, t.take);
      }
      ImGui::SetCursorScreenPos(ImVec2(ax, by));
      ImGui::BeginDisabled(recording || a.recordArmed);
      if (reclook::rlButton(a, aud ? "Stop" : "Audition", reclook::Kind::Normal, bw, "Plays this take alone from its start; Stop (or stopping the transport) goes back", aud)) {
        if (aud) {
          stopListening(a);
        } else {
          audition(a, tr, t.take);
        }
      }
      ImGui::EndDisabled();
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
      ImGui::Dummy(ImVec2(0, 0));
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PopID();
  }
}

// ---------------------------------------------------------------- notes ----

void filterMenuItems(App& a) {
  // integ-1124: one submenu, so the record options menu (with K-123 core A's rows) still fits 1440x900.
  if (!ImGui::BeginMenu(i18n::tr("Other messages"))) return;
  RecordOptions& o = a.recOptions;
  ImGui::MenuItem(i18n::tr("Pitch bend"), nullptr, &o.bend);
  ImGui::MenuItem(i18n::tr("Pressure (aftertouch)"), nullptr, &o.pressure);
  ImGui::MenuItem(i18n::tr("Program changes"), nullptr, &o.program);
  ImGui::EndMenu();
}

int recoverPlayed(App& a) {
  if (livelook::active(a)) return livelook::captureNow(a);  // Clips: its Capture (into the clip grid)
  if (a.capture.size() == 0) {
    a.notify(i18n::tr("Nothing to recover: play some notes first (while not recording)"));
    return 0;
  }
  RecordOptions o = a.recOptions;
  const Tick g = a.piano.effGrid > 0 ? a.piano.effGrid : snapTicks(a.doc.song(), a.piano.snap);
  o.grid = std::max<Tick>(1, g);
  const NoteCapture::Recovered r = a.capture.recover(a.doc, a.curPattern, a.doc.song().tempo, o, a.now);
  gLastTimed = r.timed;
  if (r.notes <= 0) {
    a.notify(i18n::tr("Nothing to recover: play some notes first (while not recording)"));
    return 0;
  }
  if (r.pattern != kNoId && r.pattern != a.curPattern) a.selectPattern(r.pattern);
  const Pattern* p = a.doc.song().pattern(r.pattern);
  a.notify(std::to_string(r.notes) + (r.notes == 1 ? " note" : " notes") + (r.timed ? " put back where they were played in " : " placed in ") +
           (p ? p->name : std::string("the pattern")));
  return r.notes;
}

void recoverMenuItem(App& a) {
  if (ImGui::MenuItem(i18n::tr("Recover what I just played"), nullptr, false, a.capture.size() > 0)) recoverPlayed(a);
  ui::Hint("The notes played in the last two minutes while nothing recorded: the last phrase goes in where it was played (or at the pattern's start when the transport was stopped)");
}

// ---------------------------------------------------------------- dev ----

bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame) {
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
  auto num = [&](size_t i) { return std::atoi(arg(i).c_str()); };
  const Song& s = a.doc.song();
  if (cmd == "rectake") {
    const std::string w = arg(0);
    if (w == "list") {
      for (int tr : takes::tracksWithTakes(s)) {
        std::printf("rectake frame %d: track %d", frame, tr);
        for (const takes::TakeInfo& t : takes::takeList(s, tr)) {
          std::printf(" take %d rating %d comp %d clips %d;", t.take, t.rating, t.inComp ? 1 : 0, t.clips);
        }
        std::printf("\n");
      }
      std::printf("rectake frame %d: listen track %d take %d audition %d playing %d undo \"%s\"\n", frame, L().track, L().take, L().audition ? 1 : 0,
                  a.engine.playing() ? 1 : 0, a.doc.history().undoLabel().c_str());
      std::fflush(stdout);
    } else if (w == "make") {  // three loop-pass takes (pattern clips) on a track at bar 2, the newest playing
      const int tr = num(1);
      a.doc.editSong("make takes", [&](Song& x) {
        if (x.patterns.empty() || tr < 0 || static_cast<size_t>(tr) >= x.tracks.size()) return;
        const Tick bar = x.barTicks();
        const ObjectId g = x.allocateId();
        for (int k = 0; k < 3; ++k) {
          Clip c;
          c.id = x.allocateId();
          c.kind = ClipKind::Pattern;
          c.pattern = x.patterns[0].id;
          c.track = tr;
          c.start = bar;
          c.length = bar;
          c.takeGroup = g;
          c.take = k;
          c.muted = k != 2;
          x.playlist.push_back(c);
        }
        x.tracks[static_cast<size_t>(tr)].takeLanes = true;
      });
    } else if (w == "rate") {
      rate(a, num(1), num(2), num(3));
    } else if (w == "solo") {
      solo(a, num(1), num(2));
    } else if (w == "audition") {
      audition(a, num(1), num(2));
    } else if (w == "unsolo") {
      stopListening(a);
    } else if (w == "played") {  // what the engine's copy plays on a track: the takes whose clips are not muted
      Song p = a.playedSong(s);
      std::printf("rectake frame %d: played track %d:", frame, num(1));
      for (const Clip& c : p.playlist) {
        if (c.track == num(1) && c.takeGroup != kNoId && !c.muted) std::printf(" %d", c.take);
      }
      std::printf(" | song:");
      for (const Clip& c : s.playlist) {
        if (c.track == num(1) && c.takeGroup != kNoId && !c.muted) std::printf(" %d", c.take);
      }
      std::printf("\n");
      std::fflush(stdout);
    }
    return true;
  }
  if (cmd == "recfilter") {
    RecordOptions& o = a.recOptions;
    const std::string w = arg(0);
    const bool v = num(1) != 0;
    if (w == "show") {
      std::printf("recfilter frame %d: %s\n", frame, recordFilterText(o).c_str());
      std::fflush(stdout);
    } else if (w == "test") {
      const auto st = static_cast<uint8_t>(std::strtol(arg(1).c_str(), nullptr, 0));
      const auto d1 = static_cast<uint8_t>(std::strtol(arg(2).c_str(), nullptr, 0));
      std::printf("recfilter frame %d: test %s %s takes %d\n", frame, arg(1).c_str(), arg(2).c_str(), recordFilterTakes(o, st, d1) ? 1 : 0);
      std::fflush(stdout);
    } else if (w == "notes") {
      o.notes = v;
    } else if (w == "controllers") {
      o.automation = v;
    } else if (w == "bend") {
      o.bend = v;
    } else if (w == "pressure") {
      o.pressure = v;
    } else if (w == "program") {
      o.program = v;
    }
    return true;
  }
  if (cmd == "recwindow") {  // the Record window on a page
    recordcard::openWindow(a, num(0));
    return true;
  }
  if (cmd == "recover") {
    if (arg(0) == "show") {
      std::printf("recover frame %d: kept %d playing %d mode %d\n", frame, a.capture.size(), a.engine.playing() ? 1 : 0,
                  static_cast<int>(a.engine.mode()));
    } else {
      const int n = recoverPlayed(a);
      std::printf("recover frame %d: placed %d timed %d pattern %d undo \"%s\"\n", frame, n, gLastTimed ? 1 : 0, s.patternIndex(a.curPattern),
                  a.doc.history().undoLabel().c_str());
    }
    std::fflush(stdout);
    return true;
  }
  if (cmd == "midiarm") {
    const std::string w = arg(0);
    if (w == "show") {
      std::printf("midiarm frame %d: applies %d live %d armed", frame, midiarm::applies(a) ? 1 : 0, s.channelIndex(midiarm::liveChannel(a)));
      for (ObjectId id : midiarm::armedChannels(a)) std::printf(" %d", s.channelIndex(id));
      std::printf(" | lives");
      for (ObjectId id : midiarm::liveChannels(a)) std::printf(" %d", s.channelIndex(id));
      std::printf("\n");
      std::fflush(stdout);
    } else if (w == "clear") {
      a.playlist.midiArm.clear();
    } else if (w == "insttrack") {  // midiarm insttrack <track> <channel index>: make an instrument track (screenshots)
      const int ci = num(2);
      std::string why;
      if (ci >= 0 && static_cast<size_t>(ci) < s.channels.size()) trackmodes::makeInstrumentTrack(a.doc, num(1), s.channels[static_cast<size_t>(ci)].id, &why);
    } else if (w == "select") {
      const int ci = num(1);
      if (ci >= 0 && static_cast<size_t>(ci) < s.channels.size()) a.selectChannel(s.channels[static_cast<size_t>(ci)].id);
    } else {
      const int ci = num(0);
      if (ci >= 0 && static_cast<size_t>(ci) < s.channels.size()) midiarm::setArmed(a, s.channels[static_cast<size_t>(ci)].id, num(1) != 0);
    }
    return true;
  }
  return false;
}

}  // namespace gl::recordrows
