// Gridline - the Novation Launchpad Pro [MK3] in the app (LaunchpadUi.h).
//
// The Session page shows the layout's own clips:
//   Clips   the clip grid (columns = its tracks, rows = scenes), launches as its launch triangles;
//   Studio, Smooth  the arrangement (rows = tracks, columns = bars): press = move the play position;
//   Tracker the patterns in order (press = select it, it plays next in pattern mode).
// Every project edit goes through Document (one undo step each, shared in a collaboration); launches,
// transport and selection stay local, as with the mouse.
#include "LaunchpadUi.h"
#include <algorithm>
#include <cmath>
#include "../../third_party/imgui/imgui.h"
#include "../core/Arrangements.h"
#include "../core/I18n.h"
#include "../core/Lanes.h"
#include "../core/Macros.h"
#include "../core/MenuOps.h"
#include "../core/NoteTools.h"
#include "../core/Params.h"
#include "../core/SongTools.h"
#include "../core/TrackModes.h"
#include "../core/surfaces/LaunchpadProMk3.h"
#include "App.h"
#include "LayoutPresets.h"
#include "LiveLook.h"
#include "Midi.h"
#include "Widgets.h"

namespace gl::surfaceui::launchpad {

namespace lp = surfaces::lpp3;
using lp::Cell;
using lp::State;

namespace {

constexpr Rgb kRed = 0xFF0000, kAmber = 0xFF8400, kBlue = 0x0000FF, kGreen = 0x00FF00;

enum class Kind { Clips, Overview, Patterns };

class AppModel : public lp::Model {
 public:
  explicit AppModel(App& a) : a_(a) {}

  Kind kind() const {
    if (livelook::active(a_)) return Kind::Clips;
    if (layoutui::active() == layout::Preset::Tracker) return Kind::Patterns;
    return Kind::Overview;
  }
  const Song& s() const { return a_.doc.song(); }

  // ---------------------------------------------------------------- session ----
  int sessionCols() const override {
    switch (kind()) {
      case Kind::Clips: return static_cast<int>(livelook::surface::columns(a_).size());
      case Kind::Patterns: return 8;
      case Kind::Overview: return bars() + 8;
    }
    return 0;
  }
  int sessionRows() const override {
    switch (kind()) {
      case Kind::Clips: return livelook::surface::scenes(a_) + 1;  // and an empty scene to record into
      case Kind::Patterns: return std::max(1, (static_cast<int>(s().patterns.size()) + 7) / 8);
      case Kind::Overview: return trackmodes::usedTrackCount(s());
    }
    return 0;
  }
  Cell sessionCell(int col, int row) const override {
    switch (kind()) {
      case Kind::Clips: return clipCell(col, row);
      case Kind::Patterns: return patternCell(row * 8 + col);
      case Kind::Overview: return overviewCell(col, row);
    }
    return {};
  }
  void sessionPress(int col, int row, lp::Mod mod) override {
    switch (kind()) {
      case Kind::Clips: clipPress(col, row, mod); break;
      case Kind::Patterns:
        if (row * 8 + col < static_cast<int>(s().patterns.size())) a_.selectPattern(s().patterns[static_cast<size_t>(row * 8 + col)].id);
        break;
      case Kind::Overview: overviewPress(col, row, mod); break;
    }
  }
  void sessionRelease(int col, int row) override {
    if (kind() != Kind::Clips) return;
    const int t = track(col);
    const ObjectId c = t >= 0 ? livelook::surface::slotClip(a_, t, row) : kNoId;
    if (c != kNoId) a_.engine.releaseClip(c);  // gate pads end when the pad is let go
  }
  Cell sideCell(int row, lp::TrackMode m) const override {
    if (kind() == Kind::Overview) return trackCell(row, m);
    if (kind() != Kind::Clips) return {};
    bool any = false, playing = false;
    for (int col = 0; col < sessionCols(); ++col) {
      const int t = track(col);
      const ObjectId c = livelook::surface::slotClip(a_, t, row);
      if (c == kNoId) continue;
      any = true;
      playing = playing || a_.engine.liveActive(t) == c;
    }
    return playing ? Cell{State::Filled, kGreen} : any ? Cell{State::Marker, 0} : Cell{};
  }
  void sidePress(int row, lp::TrackMode m, bool shift) override {
    if (kind() == Kind::Overview) {
      trackPress(row, m);
    } else if (kind() == Kind::Clips) {
      const std::vector<int> cols = livelook::surface::columns(a_);
      if (shift && !cols.empty()) livelook::surface::selectSlot(a_, cols.front(), row);
      else if (!shift && row < livelook::surface::scenes(a_)) livelook::surface::launchScene(a_, row);
    }
  }
  Cell bottomCell(int col, lp::TrackMode m) const override {
    if (kind() == Kind::Clips) return trackCell(track(col), m);
    if (kind() == Kind::Overview) return col == playBar() ? Cell{State::Selected, 0} : Cell{};
    // The Tracker: the instruments (the view's first eight channels).
    if (col >= channelCount()) return {};
    return col == selectedChannel() ? Cell{State::Selected, 0} : Cell{State::Filled, channelColour(col)};
  }
  void bottomPress(int col, lp::TrackMode m) override {
    if (kind() == Kind::Clips) trackPress(track(col), m);
    else if (kind() == Kind::Overview) a_.engine.setPosition(static_cast<Tick>(col) * s().barTicks());
    else selectChannel(col);
  }

  // ---------------------------------------------------------------- transport ----
  lp::Transport transport() const override {
    lp::Transport t;
    t.playing = a_.engine.playing();
    t.recording = kind() == Kind::Clips ? livelook::sessionRecording(a_) : a_.recordArmed;
    t.click = a_.metronome;
    t.loopRecord = a_.recOptions.takes;
    return t;
  }
  void button(lp::Button b) override {
    switch (b) {
      case lp::Button::PlayPause: a_.runAction(RemoteAction::PlayPause); break;
      case lp::Button::Stop: a_.runAction(RemoteAction::Stop); break;
      case lp::Button::Record:
        if (kind() == Kind::Clips) livelook::toggleSessionRecord(a_);
        else a_.runAction(RemoteAction::Record);
        break;
      case lp::Button::Capture: livelook::captureNow(a_); break;
      case lp::Button::Undo: a_.runAction(RemoteAction::Undo); break;
      case lp::Button::Redo: a_.runAction(RemoteAction::Redo); break;
      case lp::Button::Click: a_.runAction(RemoteAction::Metronome); break;
      case lp::Button::TapTempo: a_.runAction(RemoteAction::TapTempo); break;
      case lp::Button::StopAll:
        if (kind() == Kind::Clips) a_.engine.stopAllLive();
        else a_.runAction(RemoteAction::Stop);
        break;
      case lp::Button::LoopRecord: a_.runAction(RemoteAction::LoopRecord); break;
    }
  }

  // ---------------------------------------------------------------- channels ----
  int channelCount() const override { return static_cast<int>(s().channels.size()); }
  Rgb channelColour(int i) const override { return s().channels[static_cast<size_t>(i)].color; }
  int selectedChannel() const override { return s().channelIndex(a_.selChannel); }
  void selectChannel(int i) override {
    if (i >= 0 && i < channelCount()) a_.selectChannel(s().channels[static_cast<size_t>(i)].id);
  }
  lp::Keys keys() const override {
    lp::Keys k;
    k.root = std::clamp(a_.piano.scaleRoot, 0, 11);
    const auto& sc = tools::scales();
    if (a_.piano.scale > 0 && a_.piano.scale < static_cast<int>(sc.size())) {
      k.scale = 0;
      for (int st : sc[static_cast<size_t>(a_.piano.scale)].steps) k.scale = static_cast<uint16_t>(k.scale | (1u << (st % 12)));
    }
    if (const Channel* c = s().channel(a_.selChannel)) {
      k.colour = c->color;
      const std::string& t = c->plugin.type;
      k.drums = t == "gl.drumkit" || t == "gl.membranekit" || t == "gl.seltmann" || t == "gl.pads" || t == "gl.slicer";
    }
    return k;
  }
  void shiftRoot(int by) override {
    a_.piano.scaleRoot = ((a_.piano.scaleRoot + by) % 12 + 12) % 12;
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    a_.notify(std::string(i18n::tr("Key: ")) + names[a_.piano.scaleRoot]);
  }
  lp::Steps steps() const override {
    lp::Steps st;
    const Pattern* p = s().pattern(a_.curPattern);
    const Channel* c = s().channel(a_.selChannel);
    if (!p || !c) return st;
    st.colour = c->color;
    st.count = lanes::steps(*p, c->id);
    if (auto it = p->steps.find(c->id); it != p->steps.end()) st.velocity = it->second;
    const int grid = std::max(1, lanes::grid(*p, c->id));
    const Tick perStep = std::max<Tick>(1, s().patternBarTicks(*p) / grid);
    if (a_.engine.playing() && a_.engine.mode() == PlayMode::Pattern && st.count > 0) {
      int loop = lanes::length(*p, c->id);
      if (loop <= 0) loop = st.count;
      st.playhead = static_cast<int>(static_cast<Tick>(a_.engine.position()) / perStep) % loop;
    }
    return st;
  }
  void toggleStep(int i) override {
    const Pattern* p = s().pattern(a_.curPattern);
    if (!p || !s().channel(a_.selChannel)) return;
    uint8_t v = 0;
    if (auto it = p->steps.find(a_.selChannel); it != p->steps.end() && i < static_cast<int>(it->second.size())) v = it->second[static_cast<size_t>(i)];
    a_.doc.setStep(p->id, a_.selChannel, i, v ? 0 : 100);
  }
  void setStepLength(int n) override {
    const Pattern* p = s().pattern(a_.curPattern);
    const ObjectId ch = a_.selChannel;
    if (!p || !s().channel(ch)) return;
    const int count = lanes::steps(*p, ch);
    a_.doc.editPattern(p->id, "set step loop", [ch, n, count](Pattern& x) {
      if (n <= 0 || n >= count) x.stepLoop.erase(ch);
      else x.stepLoop[ch] = n;
    });
  }
  int patternCount() const override { return static_cast<int>(s().patterns.size()); }
  Rgb patternColour(int i) const override { return s().patterns[static_cast<size_t>(i)].color; }
  int selectedPattern() const override { return s().patternIndex(a_.curPattern); }
  void selectPattern(int i) override {
    if (i >= 0 && i < patternCount()) a_.selectPattern(s().patterns[static_cast<size_t>(i)].id);
  }

  // ---------------------------------------------------------------- faders ----
  // Tracks: Clips' columns' mixer tracks, else the mixer inserts.
  std::vector<ObjectId> faderMixers() const {
    std::vector<ObjectId> out;
    if (kind() == Kind::Clips) {
      for (int t : livelook::surface::columns(a_))
        if (const MixerTrack* m = trackmodes::playedMixer(s(), t)) out.push_back(m->id);
        else out.push_back(kNoId);
    } else {
      for (size_t i = 1; i < s().mixer.size(); ++i) out.push_back(s().mixer[i].id);
    }
    return out;
  }
  bool fadersFollowSession() const override { return kind() == Kind::Clips; }
  int faderCount(lp::Bank b) const override {
    return b == lp::Bank::Device ? 8 : static_cast<int>(faderMixers().size());
  }
  ParamId faderParam(lp::Bank b, int i, Rgb* colour) const {
    if (b == lp::Bank::Device) {
      const Channel* c = s().channel(a_.selChannel);
      if (!c || i >= 8) return {kNoId, 0};
      if (colour) *colour = macro::assigned(s(), c->id, i) ? c->color : surfaces::dim(c->color, 0.3f);
      return layerMacroParam(c->id, i);
    }
    const std::vector<ObjectId> mx = faderMixers();
    if (i < 0 || i >= static_cast<int>(mx.size()) || mx[static_cast<size_t>(i)] == kNoId) return {kNoId, 0};
    const MixerTrack* m = s().mixerTrack(mx[static_cast<size_t>(i)]);
    if (!m) return {kNoId, 0};
    if (colour) {
      *colour = m->color;
      if (kind() == Kind::Clips) {
        const std::vector<int> cols = livelook::surface::columns(a_);
        if (i < static_cast<int>(cols.size())) *colour = s().tracks[static_cast<size_t>(cols[static_cast<size_t>(i)])].color;
      }
    }
    if (b == lp::Bank::Volume) return mixerParam(m->id, MixerParam::Volume);
    if (b == lp::Bank::Pan) return mixerParam(m->id, MixerParam::Pan);
    const std::vector<size_t> rets = trackmodes::returnTracks(s());  // Sends: to the first return
    if (rets.empty() || s().mixer[rets.front()].id == m->id) return {kNoId, 0};
    return sendParam(m->id, static_cast<int>(rets.front()));
  }
  lp::Fader fader(lp::Bank b, int i) const override {
    lp::Fader f;
    Rgb colour = 0;
    const ParamId pid = faderParam(b, i, &colour);
    if (pid.owner == kNoId) return f;
    auto d = describeParam(s(), pid);
    auto v = getParam(s(), pid);
    if (!d || !v) return f;
    f.shown = true;
    f.value = d->toNormalized(*v);
    f.bipolar = b == lp::Bank::Pan;
    f.colour = colour ? colour : 0x848484;
    return f;
  }
  void setFader(lp::Bank b, int i, double v) override {
    const ParamId pid = faderParam(b, i, nullptr);
    if (pid.owner == kNoId) return;
    if (auto d = describeParam(s(), pid)) a_.doc.setParam(pid, d->fromNormalized(std::clamp(v, 0.0, 1.0)), true);  // merges until the hand pauses
  }

 private:
  // ---- Clips ----
  int track(int col) const {
    const std::vector<int> cols = livelook::surface::columns(a_);
    return col >= 0 && col < static_cast<int>(cols.size()) ? cols[static_cast<size_t>(col)] : -1;
  }
  static Rgb clipColour(const Song& s, const Clip& c) {
    if (c.kind == ClipKind::Pattern) {
      if (const Pattern* p = s.pattern(c.pattern)) return p->color;
    } else if (c.kind == ClipKind::Audio) {
      if (const Channel* ch = s.channel(c.source)) return ch->color;
    } else if (const AutomationClip* au = s.automation(c.source)) {
      return au->color;
    }
    return 0x50565E;
  }
  Cell clipCell(int col, int row) const {
    const int t = track(col);
    if (t < 0) return {};
    const ObjectId id = livelook::surface::slotClip(a_, t, row);
    if (id == kNoId) return livelook::trackArmed(a_, t) ? Cell{State::ArmedEmpty, 0} : Cell{};
    const Clip* c = s().clip(id);
    const Rgb col0 = c ? clipColour(s(), *c) : 0x50565E;
    if (id == livelook::sessionRecordClip(a_)) return {a_.engine.playing() ? State::Recording : State::RecordQueued, col0};
    if (a_.engine.liveActive(t) == id) return {a_.engine.liveStopping(t) ? State::Queued : State::Playing, col0};
    if (a_.engine.liveQueued(t) == id) return {State::Queued, col0};
    return {State::Filled, col0};
  }
  void clipPress(int col, int row, lp::Mod mod) {
    const int t = track(col);
    if (t < 0) return;
    const ObjectId id = livelook::surface::slotClip(a_, t, row);
    switch (mod) {
      case lp::Mod::Shift: livelook::surface::selectSlot(a_, t, row); return;
      case lp::Mod::Clear: livelook::surface::deleteSlot(a_, t, row); return;
      case lp::Mod::Duplicate: livelook::surface::duplicateSlot(a_, t, row); return;
      case lp::Mod::Quantise: quantise(id); return;
      case lp::Mod::Double: doubleClip(id); return;
      case lp::Mod::None: break;
    }
    if (id != kNoId) {
      livelook::surface::launchClip(a_, id);
    } else if (livelook::trackArmed(a_, t)) {
      livelook::startSessionRecord(a_, t, row);
    } else if (a_.engine.liveActive(t) != kNoId) {
      a_.engine.stopLiveTrack(t);
    }
  }
  // A pattern clip's notes to the sixteenth (one undo step).
  void quantise(ObjectId clip) {
    const Clip* c = s().clip(clip);
    if (!c || c->kind != ClipKind::Pattern) return;
    const Pattern* p = s().pattern(c->pattern);
    if (!p) return;
    const Tick grid = std::max<Tick>(2, s().ppq / 4);
    const ObjectId pat = p->id;
    a_.doc.editPattern(pat, "quantise clip", [grid](Pattern& x) {
      for (auto& [ch, notes] : x.notes) ops::quantizeStarts(notes, grid);
    });
  }
  // A pattern clip twice as long, its notes and steps played twice (one undo step).
  void doubleClip(ObjectId clip) {
    const Clip* c = s().clip(clip);
    if (!c || c->kind != ClipKind::Pattern) return;
    const Pattern* p = s().pattern(c->pattern);
    if (!p || p->bars >= 64) return;
    const Tick len = s().patternLength(*p), clen = c->length;
    a_.doc.beginGroup("double clip");
    a_.doc.editPattern(p->id, "double clip", [len](Pattern& x) {
      for (auto& [ch, notes] : x.notes) {
        const size_t n = notes.size();
        for (size_t i = 0; i < n; ++i) {
          Note q = notes[i];
          if (q.start >= len) continue;
          q.start += len;
          notes.push_back(q);
        }
      }
      for (auto& [ch, row] : x.steps) {
        const size_t n = static_cast<size_t>(std::max(0, lanes::steps(x, ch)));
        row.resize(n);
        row.resize(n * 2);
        std::copy_n(row.begin(), n, row.begin() + static_cast<std::ptrdiff_t>(n));
      }
      x.bars *= 2;
    });
    if (clen > 0) a_.doc.setClipLength(clip, clen * 2);
    a_.doc.endGroup();
  }
  // A track (playlist index) in a track control's meaning.
  bool soloed(int t) const {
    const MixerTrack* mx = trackmodes::playedMixer(s(), t);
    if (mx && !s().mixer.empty() && mx->id != s().mixer[0].id) return mx->solo;
    if (t < 0 || t >= static_cast<int>(s().tracks.size()) || s().tracks[static_cast<size_t>(t)].muted || s().tracks.size() < 2) return false;
    for (size_t k = 0; k < s().tracks.size(); ++k)
      if (static_cast<int>(k) != t && !s().tracks[k].muted) return false;
    return true;
  }
  bool armed(int t) const {
    if (kind() == Kind::Clips) return livelook::trackArmed(a_, t);
    const MixerTrack* m = trackmodes::linkedMixer(s(), t);
    return m && m->armed;
  }
  Cell trackCell(int t, lp::TrackMode m) const {
    if (t < 0 || t >= static_cast<int>(s().tracks.size())) return {};
    const PlaylistTrack& tr = s().tracks[static_cast<size_t>(t)];
    switch (m) {
      case lp::TrackMode::Select: return a_.playlist.trackSel.count(t) ? Cell{State::Selected, 0} : Cell{State::Filled, tr.color};
      case lp::TrackMode::Arm: return armed(t) ? Cell{State::Filled, kRed} : Cell{State::Dim, kRed};
      case lp::TrackMode::Mute: return tr.muted ? Cell{State::Dim, kAmber} : Cell{State::Filled, kAmber};
      case lp::TrackMode::Solo: return soloed(t) ? Cell{State::Filled, kBlue} : Cell{State::Dim, kBlue};
      case lp::TrackMode::Stop:
        if (kind() != Kind::Clips) return {};
        return a_.engine.liveActive(t) != kNoId ? Cell{State::Filled, kRed} : Cell{State::Dim, kRed};
    }
    return {};
  }
  void trackPress(int t, lp::TrackMode m) {
    if (t < 0 || t >= static_cast<int>(s().tracks.size())) return;
    switch (m) {
      case lp::TrackMode::Select:
        a_.playlist.trackSel = {t};
        for (const Channel& c : s().channels)  // the track's instrument becomes the played channel
          if (const Channel* lc = trackmodes::linkedChannel(s(), t); lc && lc->id == c.id) a_.selectChannel(c.id);
        break;
      case lp::TrackMode::Mute:
        if (kind() == Kind::Clips) livelook::clickHeader(a_, t, livelook::HeaderPart::Activator, false);
        else a_.doc.editPlaylistTrack(t, "mute playlist track", [](PlaylistTrack& x) { x.muted = !x.muted; });
        break;
      case lp::TrackMode::Solo:
        if (kind() == Kind::Clips) {
          livelook::clickHeader(a_, t, livelook::HeaderPart::Solo, false);
        } else if (const MixerTrack* mx = trackmodes::playedMixer(s(), t)) {
          ops::soloMixerTrack(a_.doc, mx->id);
        }
        break;
      case lp::TrackMode::Arm:
        if (kind() == Kind::Clips) livelook::clickHeader(a_, t, livelook::HeaderPart::Arm, false);
        else if (const MixerTrack* mx = trackmodes::linkedMixer(s(), t)) trackmodes::setArmed(a_.doc, t, !mx->armed);
        break;
      case lp::TrackMode::Stop:
        if (kind() == Kind::Clips) a_.engine.stopLiveTrack(t);
        break;
    }
  }

  // ---- the Tracker's patterns ----
  Cell patternCell(int i) const {
    if (i < 0 || i >= static_cast<int>(s().patterns.size())) return {};
    const Pattern& p = s().patterns[static_cast<size_t>(i)];
    if (p.id != a_.curPattern) return {State::Filled, p.color};
    return a_.engine.playing() && a_.engine.mode() == PlayMode::Pattern ? Cell{State::Playing, p.color} : Cell{State::Selected, 0};
  }

  // ---- Studio and Smooth: the arrangement ----
  int bars() const {
    const Tick bar = std::max<Tick>(1, s().barTicks());
    Tick end = 0;
    for (const Clip& c : s().playlist) end = std::max(end, c.start + s().clipLength(c));
    return static_cast<int>((end + bar - 1) / bar);
  }
  int playBar() const { return static_cast<int>(static_cast<Tick>(a_.engine.position()) / std::max<Tick>(1, s().barTicks())); }
  const Clip* clipAt(int bar, int t) const {
    const Tick bt = std::max<Tick>(1, s().barTicks()), a0 = static_cast<Tick>(bar) * bt, a1 = a0 + bt;
    for (const Clip& c : s().playlist)
      if (c.track == t && c.start < a1 && c.start + std::max<Tick>(1, s().clipLength(c)) > a0) return &c;
    return nullptr;
  }
  Cell overviewCell(int bar, int t) const {
    const Clip* c = clipAt(bar, t);
    const bool here = bar == playBar();
    if (!c) return here ? Cell{State::Marker, 0} : Cell{};
    const bool muted = t < static_cast<int>(s().tracks.size()) && s().tracks[static_cast<size_t>(t)].muted;
    if (here && a_.engine.playing() && a_.engine.mode() == PlayMode::Song && !muted) return {State::Playing, clipColour(s(), *c)};
    return {muted ? State::Dim : State::Filled, clipColour(s(), *c)};
  }
  void overviewPress(int bar, int t, lp::Mod mod) {
    if (mod != lp::Mod::Shift) a_.engine.setPosition(static_cast<Tick>(bar) * s().barTicks());
    if (const Clip* c = clipAt(bar, t)) {
      a_.playlist.selected = {c->id};
      if (c->kind == ClipKind::Pattern) a_.selectPattern(c->pattern);
    }
    a_.playlist.trackSel = {t};
  }

  App& a_;
};

class LaunchpadDevice : public Device {
 public:
  LaunchpadDevice(App& a, std::function<void(const Bytes&)> out)
      : model_(a), ctl_(model_, lp::Options{}, std::move(out)), pads_(std::make_shared<SurfacePads>()) {}

  void connect() override { ctl_.connect(); }
  void disconnect() override { ctl_.disconnect(); }
  void midi(const MidiMessage& m, double now) override { ctl_.onMidi(m, now); }
  void sysex(const Bytes& b, double now) override { ctl_.onSysex(b, now); }
  void idle(double now) override {
    ctl_.idle(now);
    if (ctl_.padSerial() != serial_) {  // the keys / drums map to the MIDI driver
      serial_ = ctl_.padSerial();
      for (size_t i = 0; i < 128; ++i) pads_->pitch[i].store(ctl_.padPitch()[i]);
    }
    pads_->velocity.store(ctl_.options().velocity);
  }
  void options(const std::string& saved) override { ctl_.setOptions(lp::Options::load(saved)); }
  bool settingsUi(std::string& saved) override {
    lp::Options o = lp::Options::load(saved);
    bool ch = false;
    ch |= ImGui::Checkbox(i18n::tr("Note and Sequencer open Gridline's pages"), &o.modePages);
    ui::Hint("Off: the unit's own Note mode and Sequencer, which play the selected instrument through its MIDI port");
    ch |= ImGui::Checkbox(i18n::tr("Keys and Drums pages"), &o.keysPage);
    ImGui::SameLine();
    ch |= ImGui::Checkbox(i18n::tr("Steps page"), &o.stepsPage);
    ImGui::SameLine();
    ch |= ImGui::Checkbox(i18n::tr("Faders"), &o.faders);
    const char* vel[] = {i18n::tr("As played"), i18n::tr("Soft"), i18n::tr("Hard"), i18n::tr("Fixed")};
    const char* bright[] = {i18n::tr("Full"), i18n::tr("Medium"), i18n::tr("Low")};
    ImGui::SetNextItemWidth(130);
    ch |= ImGui::Combo(i18n::tr("Pad velocity"), &o.velocity, vel, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ch |= ImGui::Combo(i18n::tr("LED brightness"), &o.brightness, bright, 3);
    ch |= ImGui::Checkbox(i18n::tr("Exact clip colours (RGB)"), &o.exactColours);
    ui::Hint("Sends each clip's own colour instead of the nearest of the unit's 128 colours");
    if (ch) saved = o.save();
    return ch;
  }
  std::string status() const override {
    static const char* pages[] = {"Session", "Keys", "Drums", "Steps", "Faders", "unit's own mode"};
    std::string s = std::string("page ") + pages[static_cast<int>(ctl_.page())];
    if (ctl_.page() == lp::Page::Faders) s += " bank " + std::to_string(static_cast<int>(ctl_.bank()));
    s += " view " + std::to_string(ctl_.colOffset()) + "," + std::to_string(ctl_.rowOffset());
    s += " octave " + std::to_string(ctl_.octave()) + " layout " + std::to_string(ctl_.lastLayout());
    if (!ctl_.firmware().empty()) s += " firmware " + ctl_.firmware();
    return s;
  }
  std::shared_ptr<SurfacePads> pads() override { return pads_; }

 private:
  AppModel model_;
  lp::Controller ctl_;
  std::shared_ptr<SurfacePads> pads_;
  uint64_t serial_ = 0;
};

}  // namespace

Type type() {
  Type t;
  t.id = "launchpad-pro-mk3";
  t.label = "Novation Launchpad Pro [MK3]";
  t.detect = [](const std::string& port) { return lp::isDawPort(port); };
  t.make = [](App& a, const std::string&, std::function<void(const Bytes&)> out) -> std::unique_ptr<Device> {
    return std::make_unique<LaunchpadDevice>(a, std::move(out));
  };
  t.output = [](const std::string& port, const std::vector<std::string>& outs) { return lp::outputFor(port, outs); };
  return t;
}

}  // namespace gl::surfaceui::launchpad
