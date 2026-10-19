// Gridline — keyboard / MIDI workflow (J-17): remote control mapping curves,
// takeover modes, relative encoders, buttons, controller templates, note and
// automation recording, step recording, typing keyboard, MIDI clock / MTC
// output timing, clock following, count-in, input routing and pedals.
// Everything runs on simulated MIDI messages (no hardware).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/MidiRecord.h"
#include "core/MidiSync.h"
#include "core/MidiTemplates.h"
#include "core/PatternAutomation.h"
#include "core/Project.h"
#include "core/Remote.h"
#include "engine/AudioEngine.h"

using namespace gl;

namespace {

MidiMessage cc(int n, int v, int ch = 0) { return {static_cast<uint8_t>(0xB0 | ch), static_cast<uint8_t>(n), static_cast<uint8_t>(v), -1}; }
MidiMessage noteOn(int n, int v, int ch = 0) { return {static_cast<uint8_t>(0x90 | ch), static_cast<uint8_t>(n), static_cast<uint8_t>(v), -1}; }
MidiMessage noteOff(int n, int ch = 0) { return {static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(n), 0, -1}; }
bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

std::vector<MidiOutEvent> drain(MidiEventRing& r) {
  std::vector<MidiOutEvent> v;
  MidiOutEvent e;
  while (r.pop(e)) v.push_back(e);
  return v;
}

float peak(const std::vector<float>& v, size_t from = 0, size_t to = ~size_t(0)) {
  float p = 0;
  for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}

}  // namespace

int main() {
  // ---------------------------------------------------------------- mapping curves
  {
    ControllerLink l;
    l.cc = 74;
    RemoteMapper m;
    double out = -1;
    expect(remoteMatches(l, cc(74, 10)) && !remoteMatches(l, cc(75, 10)), "CC link matches its controller only");
    expect(m.process(l, cc(74, 127), 0.3, &out) && near(out, 1.0), "linear: CC 127 → 1");
    expect(m.process(l, cc(74, 0), 0.3, &out) && near(out, 0.0), "linear: CC 0 → 0");
    l.min = 0.2f;
    l.max = 0.6f;
    m.process(l, cc(74, 127), 0.3, &out);
    expect(near(out, 0.6, 1e-6), "range: CC 127 → max");
    m.process(l, cc(74, 0), 0.3, &out);
    expect(near(out, 0.2, 1e-6), "range: CC 0 → min");
    l.invert = true;
    m.process(l, cc(74, 0), 0.3, &out);
    expect(near(out, 0.6, 1e-6), "invert: CC 0 → max");
    l = ControllerLink{};
    l.cc = 74;
    l.curve = ModCurve::Formula;
    l.formula = "input^2";
    m.process(l, cc(74, 127), 0, &out);
    double a = out;
    MidiMessage half{0xE0, 0x00, 0x40, -1};  // pitch bend centre: 8192/16383
    ControllerLink pb = l;
    pb.source = RemoteSource::PitchBend;
    m.process(pb, half, 0, &out);
    expect(near(a, 1.0) && near(out, std::pow(8192.0 / 16383.0, 2), 1e-9), "formula curve (input^2) on CC and 14-bit pitch bend");
    l.formula = "this is not ( valid";
    m.process(l, cc(74, 100), 0, &out);
    expect(std::isfinite(out), "a broken formula still yields a finite value");
    // Presets: the "centre at value 64" preset puts CC 64 exactly at the middle.
    ControllerLink c = ControllerLink{};
    c.cc = 1;
    for (const auto& p : remotePresets())
      if (std::string(p.name) == "Centre at value 64") applyRemotePreset(c, p);
    m.process(c, cc(1, 64), 0, &out);
    double mid = out;
    m.process(c, cc(1, 127), 0, &out);
    expect(near(mid, 0.5, 1e-9) && near(out, 1.0, 1e-9), "preset: centred around 64 (64 → 0.5, 127 → 1)");
    c.curve = ModCurve::Switch;
    c.formula.clear();
    m.process(c, cc(1, 63), 0, &out);
    double lo = out;
    m.process(c, cc(1, 65), 0, &out);
    expect(lo == 0.0 && out == 1.0, "switch curve");
    expect(remoteCurve(c, 0.3) == applyModCurve(ModCurve::Switch, 0.3), "remoteCurve shares the internal link curves");
    expect(remotePresets().size() >= 20, "preset menu has absolute, relative and button presets");
  }

  // ---------------------------------------------------------------- channel / port / sources
  {
    ControllerLink l;
    l.cc = 10;
    l.midiChannel = 3;
    l.port = 2;
    MidiMessage x = cc(10, 5, 3);
    x.port = 2;
    MidiMessage y = cc(10, 5, 4);
    y.port = 2;
    MidiMessage z = cc(10, 5, 3);
    z.port = 1;
    expect(remoteMatches(l, x) && !remoteMatches(l, y) && !remoteMatches(l, z), "link filters MIDI channel and port");
    ControllerLink at;
    at.source = RemoteSource::ChannelPressure;
    expect(remoteMatches(at, {0xD0, 100, 0, -1}) && near(remoteInput(at, {0xD0, 100, 0, -1}), 100 / 127.0),
           "channel aftertouch link");
    ControllerLink kp;
    kp.source = RemoteSource::KeyPressure;
    kp.cc = 60;
    expect(remoteMatches(kp, {0xA0, 60, 30, -1}) && !remoteMatches(kp, {0xA0, 61, 30, -1}), "key aftertouch link");
    ControllerLink pc;
    pc.source = RemoteSource::Program;
    pc.cc = 4;
    expect(remoteMatches(pc, {0xC0, 4, 0, -1}) && remoteSourceLabel(pc).find("Program 5") == 0, "program change link + label");
    expect(midiNoteName(60) == "C5" && midiNoteName(61) == "C#5", "note names (60 = C5)");
  }

  // ---------------------------------------------------------------- takeover modes
  {
    ControllerLink l;
    l.cc = 7;
    l.takeover = RemoteTakeover::Pickup;
    RemoteMapper m;
    double out = -1, cur = 0.5;
    bool changed = m.process(l, cc(7, 10), cur, &out);
    expect(!changed && m.waiting(l), "pickup: far below the value → ignored, waiting");
    changed = m.process(l, cc(7, 40), cur, &out);
    expect(!changed, "pickup: still below → ignored");
    changed = m.process(l, cc(7, 70), cur, &out);
    expect(changed && near(out, 70 / 127.0) && !m.waiting(l), "pickup: passing the value picks it up");
    cur = out;
    changed = m.process(l, cc(7, 60), cur, &out);
    expect(changed && near(out, 60 / 127.0), "pickup: follows once picked up");
    cur = 0.95;  // the value was changed elsewhere (mouse, automation)
    changed = m.process(l, cc(7, 62), cur, &out);
    expect(!changed, "pickup: re-arms after an outside change");
    changed = m.process(l, cc(7, 127), cur, &out);
    expect(changed && near(out, 1.0), "pickup: crossing again takes over");
    RemoteMapper m2;
    changed = m2.process(l, cc(7, 64), 0.5, &out);
    expect(changed, "pickup: starting within tolerance takes over at once");

    ControllerLink j;
    j.cc = 8;
    RemoteMapper mj;
    expect(mj.process(j, cc(8, 0), 0.9, &out) && near(out, 0), "jump: value jumps to the control");

    ControllerLink s;
    s.cc = 9;
    s.takeover = RemoteTakeover::Scale;
    RemoteMapper ms;
    cur = 0.8;
    changed = ms.process(s, cc(9, 20), cur, &out);
    expect(!changed, "scale: first message only sets the direction reference");
    changed = ms.process(s, cc(9, 30), cur, &out);
    double expectUp = 0.8 + (1.0 - 0.8) * (30 / 127.0 - 20 / 127.0) / (1.0 - 20 / 127.0);
    expect(changed && near(out, expectUp, 1e-9), "scale: moving up closes the gap proportionally");
    cur = out;
    for (int v = 31; v <= 127; ++v)
      if (ms.process(s, cc(9, v), cur, &out)) cur = out;
    expect(near(cur, 1.0, 1e-9), "scale: both reach the top together");
    for (int v = 126; v >= 0; --v)
      if (ms.process(s, cc(9, v), cur, &out)) cur = out;
    expect(near(cur, 0.0, 1e-9), "scale: and the bottom");
  }

  // ---------------------------------------------------------------- relative encoders
  {
    expect(decodeRelative(RemoteEncoder::TwosComplement, 1) == 1 && decodeRelative(RemoteEncoder::TwosComplement, 127) == -1 &&
               decodeRelative(RemoteEncoder::TwosComplement, 65) == -63,
           "two's complement decode");
    expect(decodeRelative(RemoteEncoder::BinaryOffset, 65) == 1 && decodeRelative(RemoteEncoder::BinaryOffset, 63) == -1 &&
               decodeRelative(RemoteEncoder::BinaryOffset, 64) == 0,
           "offset-64 decode");
    expect(decodeRelative(RemoteEncoder::SignBit, 3) == 3 && decodeRelative(RemoteEncoder::SignBit, 65) == -1 &&
               decodeRelative(RemoteEncoder::SignBit, 0x40 | 10) == -10,
           "sign-bit decode");
    bool round = true;
    for (RemoteEncoder e : {RemoteEncoder::TwosComplement, RemoteEncoder::BinaryOffset, RemoteEncoder::SignBit})
      for (int d = -63; d <= 63; ++d) round = round && decodeRelative(e, encodeRelative(e, d)) == d;
    expect(round, "encode/decode round trip for all three encodings");
    ControllerLink l;
    l.cc = 20;
    l.encoder = RemoteEncoder::TwosComplement;
    RemoteMapper m;
    double out = -1;
    m.process(l, cc(20, 1), 0.5, &out);
    expect(near(out, 0.5 + 1 / 128.0), "relative: +1 detent = 1/128");
    m.process(l, cc(20, 126), 0.5, &out);
    expect(near(out, 0.5 - 2 / 128.0), "relative: -2 detents");
    l.sensitivity = 4;
    m.process(l, cc(20, 3), 0.5, &out);
    expect(near(out, 0.5 + 12 / 128.0), "relative: sensitivity scales the step");
    m.process(l, cc(20, 63), 0.99, &out);
    expect(near(out, 1.0), "relative: clamps at the top of the range");
    l.encoder = RemoteEncoder::BinaryOffset;
    l.sensitivity = 1;
    l.invert = true;
    m.process(l, cc(20, 66), 0.5, &out);
    expect(near(out, 0.5 - 2 / 128.0), "relative: invert flips direction");
    l.invert = false;
    expect(!m.process(l, cc(20, 64), 0.5, &out), "relative: zero delta is no change");
  }

  // ---------------------------------------------------------------- buttons & actions
  {
    ControllerLink t;
    t.source = RemoteSource::Note;
    t.cc = 36;
    t.button = RemoteButton::Toggle;
    RemoteMapper m;
    double out = -1;
    bool a = m.process(t, noteOn(36, 100), 0, &out);
    double first = out;
    bool b = m.process(t, noteOff(36), first, &out);
    bool c = m.process(t, noteOn(36, 90), first, &out);
    expect(a && first == 1.0 && !b && c && out == 0.0, "toggle: press on, release nothing, press off");
    ControllerLink h = t;
    h.button = RemoteButton::Momentary;
    h.cc = 37;
    m.process(h, noteOn(37, 50), 0, &out);
    double on = out;
    m.process(h, noteOff(37), on, &out);
    expect(on == 1.0 && out == 0.0, "momentary: max while held, min on release");
    ControllerLink inc;
    inc.cc = 50;
    inc.button = RemoteButton::Increment;
    double cur = 0;
    std::vector<double> seq;
    for (int i = 0; i < 6; ++i) {
      if (m.process(inc, cc(50, 127), cur, &out, 0.25)) cur = out;
      m.process(inc, cc(50, 0), cur, &out, 0.25);
      seq.push_back(cur);
    }
    expect(seq == std::vector<double>({0.25, 0.5, 0.75, 1.0, 0.0, 0.25}), "increment: steps up and wraps");
    ControllerLink v;
    v.source = RemoteSource::Note;
    v.cc = 40;
    m.process(v, noteOn(40, 127), 0, &out);
    double vel = out;
    expect(vel == 1.0 && !m.process(v, noteOff(40), vel, &out), "note value: velocity sets, note-off keeps");
    ControllerLink act;
    act.cc = 117;
    act.action = RemoteAction::PlayPause;
    RemoteMapper ma;
    bool p1 = ma.trigger(act, cc(117, 127));
    bool p2 = ma.trigger(act, cc(117, 127));
    bool p3 = ma.trigger(act, cc(117, 0));
    bool p4 = ma.trigger(act, cc(117, 127));
    expect(p1 && !p2 && !p3 && p4, "action: fires on the press edge only");
    ControllerLink an;
    an.source = RemoteSource::Note;
    an.cc = 93;
    an.action = RemoteAction::Stop;
    expect(ma.trigger(an, noteOn(93, 1)) && ma.trigger(an, noteOn(93, 1)) && !ma.trigger(an, noteOff(93)),
           "action on a note: every note-on fires");
  }

  // ---------------------------------------------------------------- velocity curves
  {
    expect(applyVelocityCurve(VelocityCurve::Linear, 64) == 64 && applyVelocityCurve(VelocityCurve::Linear, 0) == 0,
           "velocity: linear is identity, 0 stays note-off");
    expect(applyVelocityCurve(VelocityCurve::Soft, 64) > 64 && applyVelocityCurve(VelocityCurve::Hard, 64) < 64,
           "velocity: soft louder, hard quieter in the middle");
    expect(applyVelocityCurve(VelocityCurve::Soft, 127) == 127 && applyVelocityCurve(VelocityCurve::Hard, 1) >= 1,
           "velocity: ends preserved");
    expect(applyVelocityCurve(VelocityCurve::Fixed, 12, 99) == 99, "velocity: fixed");
  }

  // ---------------------------------------------------------------- controller templates
  {
    std::vector<std::string> errs;
    auto all = loadControllerTemplates({"assets/midi/controllers"}, &errs);
    expect(errs.empty() && all.size() >= 5, "shipped templates load without errors");
    const ControllerTemplate* k = matchControllerTemplate(all, "nanoKONTROL2 SLIDER/KNOB");
    expect(k && k->id == "korg-nanokontrol2", "device name match: nanoKONTROL2");
    const ControllerTemplate* lk = matchControllerTemplate(all, "Launchkey Mini MK3 MIDI Port");
    expect(lk && lk->id == "novation-launchkey", "device name match: Launchkey (case-insensitive substring)");
    const ControllerTemplate* xl = matchControllerTemplate(all, "Launch Control XL");
    expect(xl && xl->id == "novation-launch-control-xl", "longest match wins");
    const ControllerTemplate* g = matchControllerTemplate(all, "Some Unknown Keys");
    expect(g && g->id == "generic", "unknown devices fall back to the generic template");
    if (k) {
      TemplateHit h = templateLookup(*k, cc(41, 127));
      TemplateHit kn = templateLookup(*k, cc(18, 64));
      TemplateHit fd = templateLookup(*k, cc(3, 64));
      TemplateHit none = templateLookup(*k, cc(100, 64));
      expect(h.kind == TemplateHit::Action && h.action == RemoteAction::PlayPause, "template transport: play");
      expect(kn.kind == TemplateHit::Knob && kn.index == 2 && fd.kind == TemplateHit::Fader && fd.index == 3 &&
                 none.kind == TemplateHit::None,
             "template knobs → focused plugin params, faders → inserts");
      expect(k->takeover == RemoteTakeover::Pickup, "template takeover mode");
      ControllerTemplate back;
      expect(parseControllerTemplate(controllerTemplateJson(*k), back) && back.name == k->name &&
                 back.transport == k->transport && back.knobs == k->knobs && back.faders == k->faders && back.takeover == k->takeover,
             "template JSON round trip");
    }
    if (const ControllerTemplate* e = findControllerTemplate(all, "endless-encoders"))
      expect(e->knobEncoder == RemoteEncoder::BinaryOffset, "template relative encoder setting");
    ControllerTemplate bad;
    std::string err;
    expect(!parseControllerTemplate("{ nope", bad, &err) && !err.empty(), "bad template JSON is reported");
    // A user copy overrides the shipped one by id.
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "gridline_j17_templates";
    fs::create_directories(dir);
    {
      ControllerTemplate u = genericTemplate();
      u.name = "My generic";
      u.knobs.resize(2);
      FILE* f = std::fopen((dir / "generic.json").string().c_str(), "w");
      std::string j = controllerTemplateJson(u);
      std::fwrite(j.data(), 1, j.size(), f);
      std::fclose(f);
    }
    auto merged = loadControllerTemplates({"assets/midi/controllers", dir.string()});
    const ControllerTemplate* gu = findControllerTemplate(merged, "generic");
    expect(gu && gu->name == "My generic" && gu->knobs.size() == 2, "user template overrides the shipped one");
    fs::remove_all(dir);
    expect(parseMmc({0xF0, 0x7F, 0x7F, 0x06, 0x02, 0xF7}) == RemoteAction::PlayPause &&
               parseMmc({0xF0, 0x7F, 0x00, 0x06, 0x01, 0xF7}) == RemoteAction::Stop &&
               parseMmc({0xF0, 0x7F, 0x7F, 0x06, 0x06, 0xF7}) == RemoteAction::Record &&
               parseMmc({0xF0, 0x43, 0x7F, 0x06, 0x02, 0xF7}) == RemoteAction::None,
           "MMC transport sysex");
  }

  // ---------------------------------------------------------------- recording
  Document doc;
  doc.reset(makeEmptySong());  // 96 PPQ, 4/4, one pattern
  const ObjectId ch = doc.addChannel("Synth", "gl.synth");
  const ObjectId pat = doc.song().patterns[0].id;
  const Tick bar = doc.song().barTicks();
  {
    expect(quantizeTick(25, 24) == 24 && quantizeTick(37, 24) == 48 && quantizeTick(5, 1) == 5, "quantize to the grid");
    doc.addNote(pat, ch, Note(192, 24, 50, 80));  // existing note
    NoteRecorder r;
    RecordOptions o;
    o.grid = 24;
    r.beginPattern(doc, pat, o, bar);
    r.noteOn(doc, ch, 60, 100, 25.0);
    r.noteOff(doc, ch, 60, 70.0);
    const auto& ns = doc.song().pattern(pat)->notes.at(ch);
    bool found = false, kept = false;
    for (const auto& n : ns) {
      found = found || (n.pitch == 60 && n.start == 24 && n.length == 45 && n.velocity == 100);
      kept = kept || n.pitch == 50;
    }
    expect(found, "record: start quantized to the grid, length kept");
    expect(kept, "overdub keeps existing notes");
    // A note held across the loop end.
    r.position(doc, 300);
    r.noteOn(doc, ch, 62, 90, 370.0);
    r.position(doc, 380);
    r.position(doc, 10);  // wrapped
    r.noteOff(doc, ch, 62, 20.0);
    bool wrapped = false;
    for (const auto& n : doc.song().pattern(pat)->notes.at(ch)) wrapped = wrapped || (n.pitch == 62 && n.start == 360 && n.length == 34);
    expect(wrapped && r.pass() == 1, "record: note across the loop wrap gets the full length");
    r.end(doc, 30);
    expect(!r.active() && r.notesRecorded() == 2, "record session ends");
    doc.undo();
    bool undone = true;
    for (const auto& n : doc.song().pattern(pat)->notes.at(ch)) undone = undone && n.pitch != 62;
    expect(undone, "recorded notes are undoable");
  }
  {
    NoteRecorder r;
    RecordOptions o;
    o.overdub = false;
    o.quantizeLength = true;
    o.grid = 24;
    r.beginPattern(doc, pat, o, bar);
    r.noteOn(doc, ch, 64, 100, 49.0);
    r.noteOff(doc, ch, 64, 60.0);
    r.noteOn(doc, ch, 65, 100, 96.0);
    r.noteOff(doc, ch, 65, 150.0);
    r.end(doc, 160);
    const auto& ns = doc.song().pattern(pat)->notes.at(ch);
    expect(ns.size() == 2 && ns[0].pitch == 64 && ns[0].start == 48 && ns[0].length == 24 && ns[1].length == 48,
           "replace mode clears the channel once per pass; length quantize");
  }
  {
    // Loop recording into takes.
    const size_t before = doc.song().patterns.size();
    NoteRecorder r;
    RecordOptions o;
    o.takes = true;
    o.grid = 1;
    o.quantize = false;
    r.beginPattern(doc, pat, o, bar);
    r.noteOn(doc, ch, 70, 100, 10);
    r.noteOff(doc, ch, 70, 20);
    r.position(doc, 380);
    r.position(doc, 5);  // pass 2 → take 2
    r.noteOn(doc, ch, 71, 100, 10);
    r.noteOff(doc, ch, 71, 20);
    r.end(doc, 30);
    const Song& s = doc.song();
    const Pattern* take = s.pattern(r.pattern());
    bool t1 = false, t2 = false, t2clean = true;
    for (const auto& n : s.pattern(pat)->notes.at(ch)) t1 = t1 || n.pitch == 70;
    if (take && take->notes.count(ch))
      for (const auto& n : take->notes.at(ch)) {
        t2 = t2 || n.pitch == 71;
        t2clean = t2clean && n.pitch != 70;
      }
    expect(s.patterns.size() == before + 1 && take && take->id != pat && take->name.find("take 2") != std::string::npos,
           "loop recording: the second pass records into a new take pattern");
    expect(t1 && t2 && t2clean && r.takes() == 2, "each take holds its own pass (takes start from the original content)");
  }
  {
    // Controller moves → pattern automation events.
    NoteRecorder r;
    RecordOptions o;
    r.beginPattern(doc, pat, o, bar);
    ParamId vol = channelParam(ch, ChannelParam::Volume);
    for (int t = 0; t <= 100; t += 10) r.controller(vol, t / 100.0, t);
    r.end(doc, 110);
    const PatternAutomation* lane = findPatternLane(*doc.song().pattern(pat), vol.owner, vol.index);
    expect(lane && lane->points.size() == 11 && lane->points[5].pos == 50 && near(lane->points[5].value, 0.5, 1e-6) &&
               lane->points[5].shape == AutoShape::Hold,
           "controller recording writes pattern automation (hold points)");
    // Song mode: automation clips + notes into a placed clip.
    NoteRecorder rs;
    RecordOptions os;
    os.grid = 24;
    rs.beginSong(doc, pat, os);
    rs.noteOn(doc, ch, 72, 100, static_cast<double>(2 * bar + 30));
    rs.noteOff(doc, ch, 72, static_cast<double>(2 * bar + 60));
    rs.controller(vol, 0.25, static_cast<double>(2 * bar + 10));
    rs.controller(vol, 0.75, static_cast<double>(2 * bar + 200));
    rs.end(doc, static_cast<double>(2 * bar + 300));
    const Song& s = doc.song();
    bool clip = false, autoClip = false, note = false;
    for (const auto& c : s.playlist) {
      clip = clip || (c.kind == ClipKind::Pattern && c.pattern == pat && c.start == 2 * bar);
      if (c.kind == ClipKind::Automation && c.start == 2 * bar)
        if (const AutomationClip* ac = s.automation(c.source))
          autoClip = ac->owner == vol.owner && ac->index == vol.index && ac->points.size() == 2 && ac->points[0].pos == 10;
    }
    for (const auto& n : s.pattern(pat)->notes.at(ch)) note = note || (n.pitch == 72 && n.start == 24);
    expect(clip && note, "song mode: a clip is placed on the bar and the note lands inside it");
    expect(autoClip, "song mode: controller moves become an automation clip");
  }

  // ---------------------------------------------------------------- step record
  {
    StepRecorder st;
    st.step = 24;
    const ObjectId p2 = doc.addPattern("Steps");
    st.press(60, 100);
    st.press(64, 90);
    bool w1 = st.release(doc, p2, ch, 60);
    bool w2 = st.release(doc, p2, ch, 64);
    expect(!w1 && w2 && st.cursor == 24, "step record: a chord is written when the last key is released");
    st.rest();
    st.press(67, 80);
    st.release(doc, p2, ch, 67);
    const auto& ns = doc.song().pattern(p2)->notes.at(ch);
    expect(ns.size() == 3 && ns[0].start == 0 && ns[1].start == 0 && ns[2].start == 48 && ns[2].length == 24 &&
               ns[2].velocity == 80 && st.cursor == 72,
           "step record: rests skip a step, notes are one step long");
    st.backspace(doc, p2, ch);
    expect(st.cursor == 48 && doc.song().pattern(p2)->notes.at(ch).size() == 2, "step record: backspace removes the last step");
  }

  // ---------------------------------------------------------------- typing keyboard
  {
    TypingKeyboard k;
    auto z = k.keyDown('z');
    auto q = k.keyDown('q');
    expect(z == std::vector<int>{48} && q == std::vector<int>{60}, "typing keyboard: Z = C4 (48), Q = one octave up");
    expect(k.keyDown('z').empty(), "held key does not retrigger");
    expect(k.keyUp('z') == std::vector<int>{48} && k.keyUp('q') == std::vector<int>{60}, "key-up releases its notes");
    k.keyDown('[');
    expect(k.keyDown('z') == std::vector<int>{36}, "[ lowers the octave");
    k.keyUp('z');
    k.keyDown('-');
    expect(k.velocity == 90, "- lowers the velocity");
    k.keyDown('\\');
    auto chord = k.keyDown('c');  // E
    expect(k.chordMode && chord == std::vector<int>({40, 44, 47}), "chord mode plays a major triad");
    k.keyDown('\'');
    expect(k.chord == ChordType::Minor, "' cycles the chord type");
    expect(k.releaseAll().size() == 3 && !k.anyHeld(), "release all");
    expect(TypingKeyboard::keyOffset('/') == 16 && TypingKeyboard::keyOffset('p') == 28 && TypingKeyboard::keyOffset('a') == -1,
           "FL-like layout extents");
  }

  // ---------------------------------------------------------------- clock out (direct)
  {
    MidiEventRing ring;
    MidiClockOut c;
    c.out = &ring;
    c.setMode(SyncOut::Clock);
    c.setPpq(96);
    c.setSampleRate(48000);
    // 120 BPM: 250 samples per tick, a clock every 4 ticks = 1000 samples.
    const double spt = 250.0;
    c.start(0, 0, 0);
    int64_t at = 0;
    for (int b = 0; b < 40; ++b) {
      const int len = 256;
      double t0 = at / spt, t1 = (at + len) / spt;
      c.segment(t0, t1, t0 / 192.0, t1 / 192.0, at, len, [&](double tk) { return tk * spt - static_cast<double>(at); });
      at += len;
    }
    auto ev = drain(ring);
    bool startFirst = !ev.empty() && ev[0].data[0] == 0xFA && ev[0].sample == 0;
    std::vector<int64_t> clocks;
    for (const auto& e : ev)
      if (e.data[0] == 0xF8) clocks.push_back(e.sample);
    bool exact = !clocks.empty();
    for (size_t i = 0; i < clocks.size(); ++i) exact = exact && clocks[i] == static_cast<int64_t>(i) * 1000;
    expect(startFirst && exact && clocks.size() == 11, "clock out: start, then a clock every 1000 samples (120 BPM @ 48 kHz)");
    c.stop(at);
    auto st = drain(ring);
    expect(st.size() == 1 && st[0].data[0] == 0xFC && st[0].sample == at, "clock out: stop message");
    // Continue from bar 2: SPP + continue.
    c.start(384, 0, 5000);
    c.segment(384, 385, 0, 0, 5000, 250, [&](double tk) { return (tk - 384) * spt; });
    auto co = drain(ring);
    expect(co.size() >= 3 && co[0].data[0] == 0xF2 && co[0].data[1] == 16 && co[0].data[2] == 0 && co[1].data[0] == 0xFB &&
               co[2].data[0] == 0xF8 && co[2].sample == 5000,
           "clock out: continue sends song position (16 sixteenths) then continue");
  }

  // ---------------------------------------------------------------- clock out through the engine
  {
    auto run = [&](int block, std::vector<MidiOutEvent>& out, double tempo, double sr) {
      Song s = makeEmptySong();
      s.tempo = tempo;
      ProgramBuilder b;
      AudioEngine eng(sr);
      eng.setProgram(b.build(s));
      eng.setPattern(s.patterns[0].id);
      eng.setMode(PlayMode::Pattern);
      eng.setSyncOut(SyncOut::Clock);
      std::vector<float> L(4096), R(4096);
      eng.process(L.data(), R.data(), 100);  // a little idle time first
      eng.play();
      int total = static_cast<int>(sr * 2.5);  // past the 1-bar loop (2 s at 120 BPM)
      for (int done = 0; done < total; done += block) eng.process(L.data(), R.data(), std::min(block, total - done));
      eng.stop();
      eng.process(L.data(), R.data(), 64);
      out = drain(eng.syncEvents());
    };
    std::vector<MidiOutEvent> a, b, c;
    run(64, a, 120.0, 48000);
    run(1000, b, 120.0, 48000);
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); ++i) same = a[i].sample == b[i].sample && a[i].data[0] == b[i].data[0];
    expect(!a.empty() && same, "engine clock out: identical at block sizes 64 and 1000 (sample accurate)");
    std::vector<int64_t> clocks;
    for (const auto& e : a)
      if (e.data[0] == 0xF8) clocks.push_back(e.sample - 100);
    bool exact = clocks.size() > 48;
    for (size_t i = 0; exact && i < 48; ++i) exact = clocks[i] == static_cast<int64_t>(i) * 1000;
    expect(a.size() > 0 && a[0].data[0] == 0xFA && a[0].sample == 100 && exact, "engine clock out: start at play, clocks every 1000 samples");
    // The 1-bar pattern loop wraps at 2 s: stop, song position 0, continue at that sample.
    bool wrap = false;
    for (size_t i = 0; i + 2 < a.size(); ++i)
      if (a[i].data[0] == 0xFC && a[i + 1].data[0] == 0xF2 && a[i + 2].data[0] == 0xFB)
        wrap = a[i].sample == 100 + 96000 && a[i + 1].data[1] == 0;
    expect(wrap, "engine clock out: loop wrap re-sends the position at the wrap sample");
    expect(a.back().data[0] == 0xFC, "engine clock out: stop on stop");
    run(256, c, 140.0, 44100);
    std::vector<int64_t> ck;
    for (const auto& e : c)
      if (e.data[0] == 0xF8) ck.push_back(e.sample - 100);
    bool ok140 = ck.size() > 10;
    for (size_t i = 0; ok140 && i < 10; ++i) ok140 = ck[i] == static_cast<int64_t>(std::ceil(i * 787.5 - 1e-9));
    expect(ok140, "engine clock out at 140 BPM / 44.1 kHz: ceil(k × 787.5)");
  }

  // ---------------------------------------------------------------- MTC
  {
    MidiEventRing ring;
    MidiClockOut c;
    c.out = &ring;
    c.setMode(SyncOut::Mtc25);
    c.setSampleRate(48000);
    c.start(0, 0, 0);
    for (int64_t at = 0; at < 48000; at += 480) {
      double s0 = at / 48000.0, s1 = (at + 480) / 48000.0;
      c.segment(s0 * 192, s1 * 192, s0, s1, at, 480, [](double) { return 0.0; });
    }
    auto ev = drain(ring);
    int qf = 0;
    bool spacing = true;
    int64_t prev = -1;
    for (const auto& e : ev)
      if (e.data[0] == 0xF1) {
        if (prev >= 0) spacing = spacing && e.sample - prev == 480;
        prev = e.sample;
        ++qf;
      }
    expect(!ev.empty() && ev[0].size == 10 && ev[0].data[0] == 0xF0 && ev[0].data[5] == (1 << 5), "MTC: full frame (25 fps) on start");
    expect(qf == 100 && spacing, "MTC: 100 quarter frames per second at 25 fps, evenly spaced");
    Timecode t = mtcTimecode(SyncOut::Mtc2997Drop, 1800);
    Timecode t10 = mtcTimecode(SyncOut::Mtc2997Drop, 17982);
    expect(t.m == 1 && t.s == 0 && t.f == 2 && t10.m == 10 && t10.f == 0, "MTC: drop-frame labels skip 00/01 except every 10th minute");
  }

  // ---------------------------------------------------------------- clock in
  {
    MidiClockFollower f;
    double t = 10.0;
    const double iv = 60.0 / (128.0 * 24.0);
    f.message(0xFA, 0, 0, t);
    for (int i = 0; i < 48; ++i) f.message(0xF8, 0, 0, t += iv);
    expect(f.hasTempo() && near(f.bpm(), 128.0, 1e-6) && f.running() && f.clocks() == 48, "clock in: tempo from the clock interval");
    f.message(0xFC, 0, 0, t);
    f.message(0xF2, 16, 1, t);  // 16 + 128 = 144 sixteenths
    f.message(0xFB, 0, 0, t);
    auto cmds = f.takeCommands();
    expect(cmds.size() == 4 && cmds[0].kind == MidiClockFollower::Cmd::Start && cmds[1].kind == MidiClockFollower::Cmd::Stop &&
               cmds[2].kind == MidiClockFollower::Cmd::Locate && near(cmds[2].sixteenths, 144) &&
               cmds[3].kind == MidiClockFollower::Cmd::Continue,
           "clock in: start / stop / song position / continue");
    for (int i = 0; i < 30; ++i) f.message(0xF8, 0, 0, t += 60.0 / (90.0 * 24.0));
    expect(near(f.bpm(), 90.0, 1e-6), "clock in: follows a tempo change within one beat");
    f.message(0xF8, 0, 0, t + 2.0);  // a gap: the clock stopped
    expect(!f.hasTempo(), "clock in: a long gap restarts the estimate");
  }

  // ---------------------------------------------------------------- count-in
  {
    Song s = makeEmptySong();  // 140 BPM
    ProgramBuilder b;
    AudioEngine eng(44100);
    eng.setProgram(b.build(s));
    eng.setPattern(s.patterns[0].id);
    eng.setMode(PlayMode::Pattern);
    eng.playWithCountIn(static_cast<double>(s.barTicks()));
    const int countIn = static_cast<int>(std::ceil(44100.0 * 60.0 / 140.0 * 4));
    std::vector<float> L(countIn - 100), R(countIn - 100);
    eng.process(L.data(), R.data(), countIn - 100);
    expect(eng.countingIn() && eng.position() == 0, "count-in: the transport waits for one bar");
    expect(peak(L, 0, 2000) > 0.05f && peak(L, 4000, 18000) < 1e-4f && peak(L, 18900, 20900) > 0.05f, "count-in: clicks on the beats");
    std::vector<float> L2(4410), R2(4410);
    eng.process(L2.data(), R2.data(), 4410);
    expect(!eng.countingIn() && eng.position() > 0, "count-in: then playback starts");
  }

  // ---------------------------------------------------------------- input routing & pedals
  {
    Document d;
    d.reset(makeEmptySong());
    ObjectId a = d.addChannel("A", "gl.synth");
    ProgramBuilder b;
    AudioEngine eng(44100);
    eng.setProgram(b.build(d.song()));
    std::vector<float> L(8192), R(8192);
    eng.setMidiTarget(kNoId);
    eng.midiEvent(0x91, 60, 100);  // channel 2: no route, no selection
    eng.process(L.data(), R.data(), 8192);
    float none = peak(L);
    eng.midiEvent(0x81, 60, 0);
    eng.setMidiChannelRoute(1, a);
    eng.midiEvent(0x91, 62, 100);
    eng.process(L.data(), R.data(), 8192);
    float routed = peak(L);
    expect(none < 1e-6f && routed > 0.01f, "routing: MIDI channel 2 → its mapped rack channel");
    // Sostenuto: holds the notes that were down when it was pressed.
    eng.midiEvent(0xB1, 66, 127);
    eng.midiEvent(0x81, 62, 0);
    for (int i = 0; i < 20; ++i) eng.process(L.data(), R.data(), 8192);
    float held = peak(L);
    eng.midiEvent(0xB1, 66, 0);
    for (int i = 0; i < 40; ++i) eng.process(L.data(), R.data(), 8192);
    float released = peak(L);
    expect(held > 0.01f && released < 1e-3f, "sostenuto holds the note until the pedal is released");
    // Soft pedal: quieter notes.
    eng.midiEvent(0x91, 64, 100);
    eng.process(L.data(), R.data(), 8192);
    float loud = peak(L);
    eng.midiEvent(0x81, 64, 0);
    for (int i = 0; i < 40; ++i) eng.process(L.data(), R.data(), 8192);
    eng.midiEvent(0xB1, 67, 127);
    eng.midiEvent(0x91, 64, 100);
    eng.process(L.data(), R.data(), 8192);
    float soft = peak(L);
    expect(soft < loud * 0.9f && soft > 0.0f, "soft pedal lowers the velocity");
  }

  // ---------------------------------------------------------------- project round trip
  {
    Document d;
    d.reset(makeEmptySong());
    ObjectId a = d.addChannel("A", "gl.synth");
    d.editSong("links", [&](Song& s) {
      ControllerLink l;
      l.cc = 74;
      l.owner = a;
      l.index = static_cast<uint32_t>(ChannelParam::Volume);
      l.source = RemoteSource::Note;
      l.port = 3;
      l.curve = ModCurve::Formula;
      l.formula = "input^2";
      l.invert = true;
      l.takeover = RemoteTakeover::Scale;
      l.encoder = RemoteEncoder::SignBit;
      l.sensitivity = 2.5f;
      l.button = RemoteButton::Toggle;
      s.links.push_back(l);
      ControllerLink t;
      t.cc = 41;
      t.action = RemoteAction::PlayPause;
      s.links.push_back(t);
    });
    Song back;
    std::string err;
    expect(songFromJson(songToJson(d.song()), back, &err) && back.links == d.song().links,
           "project: extended controller links (and action links) survive save / load");
  }

  return finish();
}
