// Gridline - K-28: pitch bend lines, round 2. Curve shapes (straight, smooth, ease in, ease out,
// step) with a curve amount (tension): shapes, save / load (old files load as before), edits that
// carry them, playback following the drawn curve; FL Studio export as slide notes (one per ramp,
// curves as several ramps, steps as one-tick slides, start values in the note's own pitch, chords
// bending as one, lines next to other notes of the colour left out and counted), proven by
// rendering the original and the exported-then-reimported project and comparing their pitch
// tracks; zipped FL export goes through the same writer.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Grouping.h"
#include "core/Json.h"
#include "core/NoteBend.h"
#include "core/NoteTools.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/Render.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr double kPi = 3.14159265358979323846;

std::string sineWav(const std::string& name, double hz) {
  AudioBuffer b(2, static_cast<size_t>(kSr * 6));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * kPi * hz * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = std::filesystem::absolute("test_k28_" + name + ".wav").string();
  writeWav(path, b, kSr, 16);
  return path;
}

// 120 BPM (one beat = 96 ticks = 0.5 s), one 4-bar pattern placed once.
Song baseSong() {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 4;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  return s;
}

ObjectId addChannel(Song& s, const std::string& type, const std::string& data = "") {
  Channel c;
  c.id = s.allocateId();
  c.name = "Lead";
  c.plugin = defaultPluginState(type);
  c.plugin.data = data;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return c.id;
}

Note mk(Tick start, Tick len, int pitch, std::vector<BendPoint> b = {}) {
  Note n(start, len, pitch, 100);
  n.bend = std::move(b);
  return n;
}

AudioBuffer render(const Song& s, int block = 512) {
  RenderOptions o;
  o.blockSize = block;
  return renderSong(s, o);
}

// Pitch track: the frequency of every cycle (rising zero crossings, interpolated), as
// (time of the cycle's middle, semitones above `ref` Hz).
std::vector<std::pair<double, double>> pitchTrack(const AudioBuffer& b, double ref) {
  std::vector<double> ups;
  for (size_t i = 1; i < b.frames; ++i) {
    const double y0 = b.at(0, i - 1), y1 = b.at(0, i);
    if (y0 < 0 && y1 >= 0 && std::fabs(y1 - y0) > 1e-4) ups.push_back((static_cast<double>(i - 1) + -y0 / (y1 - y0)) / kSr);
  }
  std::vector<std::pair<double, double>> out;
  for (size_t i = 1; i < ups.size(); ++i) {
    const double p = ups[i] - ups[i - 1];
    if (p > 0) out.push_back({0.5 * (ups[i] + ups[i - 1]), 12.0 * std::log2(1.0 / p / ref)});
  }
  return out;
}
// The semitones of the cycle sounding at `t` (NaN when none).
double pitchAt(const std::vector<std::pair<double, double>>& tr, double t) {
  for (size_t i = 0; i < tr.size(); ++i)
    if (tr[i].first >= t) return i > 0 && t - tr[i - 1].first < tr[i].first - t ? tr[i - 1].second : tr[i].second;
  return std::nan("");
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Compare {
  double maxCents = 0, meanCents = 0;
  int n = 0;
};
// Pitch tracks of two renders compared every 5 ms in [t0, t1).
Compare compare(const AudioBuffer& x, const AudioBuffer& y, double ref, double t0, double t1) {
  const auto a = pitchTrack(x, ref), b = pitchTrack(y, ref);
  Compare c;
  for (double t = t0; t < t1; t += 0.005) {
    const double pa = pitchAt(a, t), pb = pitchAt(b, t);
    if (std::isnan(pa) || std::isnan(pb)) continue;
    const double d = std::fabs(pa - pb) * 100.0;
    c.maxCents = std::max(c.maxCents, d);
    c.meanCents += d;
    ++c.n;
  }
  if (c.n) c.meanCents /= c.n;
  return c;
}

// Export to FL, read it back (slides stay slides).
bool flRoundTrip(const Song& s, Song& back, flp::ExportReport& rep) {
  std::vector<uint8_t> bytes;
  flp::ExportOptions opt;
  opt.renderInstruments = false;
  if (!flp::exportFlp(s, bytes, rep, opt)) return false;
  flp::Report ir;
  std::string err;
  return importFlpBytes(bytes, back, ir, &err, "", "", false);
}
const std::vector<Note>* firstNotes(const Song& s) {
  for (const auto& p : s.patterns)
    for (const auto& [ch, ns] : p.notes)
      if (!ns.empty()) return &ns;
  return nullptr;
}

}  // namespace

int main() {
  // ------------------------------------------------ shapes and the curve amount
  {
    bool ends = true, mono = true, finite = true;
    for (uint8_t c = 0; c < bend::CurveCount; ++c)
      for (double t : {-1.0, -0.5, 0.0, 0.4, 1.0, 7.0, -9.0}) {
        ends = ends && near(bend::shape(c, t, 0.0), 0, 1e-9) && near(bend::shape(c, t, 1.0), 1, 1e-9);
        double prev = -1;
        for (int i = 0; i <= 200; ++i) {
          const double v = bend::shape(c, t, i / 200.0);
          finite = finite && std::isfinite(v);
          mono = mono && v >= prev - 1e-12;
          prev = v;
        }
      }
    expect(ends && mono && finite, "every shape runs 0 to 1, never backwards, at any curve amount");
    expect(near(bend::shape(bend::Smooth, 0, 0.25), 0.5 - 0.5 * std::cos(kPi * 0.25), 1e-12) && near(bend::shape(bend::Straight, 0, 0.3), 0.3, 1e-12),
           "at curve amount 0 smooth and straight are exactly the shapes older files played");
    expect(bend::shape(bend::EaseIn, 0, 0.5) < 0.3 && bend::shape(bend::EaseOut, 0, 0.5) > 0.7, "ease in starts slowly, ease out fast");
    expect(near(bend::shape(bend::EaseIn, -1, 0.37), 0.37, 1e-9) && near(bend::shape(bend::Smooth, -1, 0.37), 0.37, 1e-9),
           "the gentlest ease or smooth is a straight glide");
    expect(bend::shape(bend::Smooth, 1, 0.3) < bend::shape(bend::Smooth, 0, 0.3) && bend::shape(bend::Straight, 0.5, 0.5) < 0.5 &&
               bend::shape(bend::Straight, -0.5, 0.5) > 0.5,
           "a positive amount makes smooth steeper and bends straight into a slow start; negative the other way");
    const std::vector<BendPoint> b = {{48, 4, bend::EaseIn, 0.3f}};
    expect(near(bend::value(b, 24), 4 * bend::shape(bend::EaseIn, 0.3, 0.5), 1e-6), "the line's value uses the segment's curve amount");
    std::vector<BendPoint> wild = {{10, 1, 9, 5.0f}, {20, 2, bend::EaseOut, std::nanf("")}};
    bend::normalize(wild);
    expect(wild[0].curve == bend::Straight && wild[0].tension == 1.0f && wild[1].tension == 0.0f, "normalize clamps the curve amount, drops unknown curves");
  }

  // ------------------------------------------------ save / load; older files; edits carry it
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.synth");
    s.patterns[0].notes[ch] = {mk(0, 192, 60, {{48, 2, bend::EaseOut, -0.4f}, {96, 5, bend::Straight, 0.6f}, {150, 0, bend::Smooth}})};
    const std::string text = jsonStringify(songToJson(s));
    std::string tight;
    for (char c : text)
      if (c != ' ' && c != '\n') tight += c;
    Json j;
    Song back;
    expect(jsonParse(text, j) && songFromJson(j, back) && back == s, "curve shapes and amounts survive save and load");
    expect(tight.find("[48,2,4,-0.4") != std::string::npos && tight.find("[150,0,1]") != std::string::npos,
           "the amount is a fourth value, written only when set (other points keep the older form)");
    Json old;
    Song ob;
    expect(jsonParse(R"({"format":"gridline-project","version":4,"ppq":96,"tempo":120,"channels":[{"id":5,"name":"x","plugin":{"type":"gl.synth","params":[]}}],
      "patterns":[{"id":2,"name":"p","notes":{"5":[{"start":0,"length":96,"pitch":60,"velocity":100,"bend":[[24,2],[48,-1,1],[60,3,2]]}]}}],"playlist":[],"mixer":[]})",
                     old) &&
               songFromJson(old, ob) && !ob.patterns.empty() && ob.patterns[0].notes[5].size() == 1,
           "a file from the first bend build loads");
    if (!ob.patterns.empty() && ob.patterns[0].notes[5].size() == 1) {
      const auto& ln = ob.patterns[0].notes[5][0].bend;
      expect(ln.size() == 3 && ln[0].tension == 0 && ln[1].curve == bend::Smooth && ln[2].curve == bend::Step && near(bend::value(ln, 36), 0.5, 1e-6),
             "its lines play as they did (no curve amount, same shapes)");
    }
    const Note n = mk(0, 96, 60, {{48, 4, bend::EaseIn, 0.5f}, {96, 0, bend::Smooth, -0.3f}});
    Note l = n, r = n;
    l.length = 24, r.start = 24, r.length = 72;
    bend::split(n, 24, l, r);
    expect(r.bend.size() == 3 && r.bend[1].curve == bend::EaseIn && r.bend[1].tension == 0.5f && r.bend[2].tension == -0.3f,
           "a split keeps the later points' shapes and amounts");
    std::vector<BendPoint> a = {{24, 2, bend::Straight}};
    bend::append(a, {{24, -3, bend::EaseOut, 0.7f}}, 48);
    expect(a.back().curve == bend::EaseOut && a.back().tension == 0.7f, "glue keeps the second line's shapes and amounts");
    std::vector<Note> cp = {n};
    std::vector<bool> sel = {true};
    tools::cloneNotes(cp, sel);
    expect(cp.size() == 2 && cp[1].bend == n.bend, "copies carry shapes and amounts");
  }

  // ------------------------------------------------ playback follows the drawn curve
  const double f0 = 441.0;  // C5 (60) plays the sample at 441 Hz
  const std::string wav = sineWav("a441", f0);
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", wav);
    s.patterns[0].notes[ch] = {mk(0, 384, 60, {{192, 12, bend::EaseIn, 0.3f}})};  // an octave up over 1 s, slow start
    const auto tr = pitchTrack(render(s), f0);
    double worst = 0;
    for (double t = 0.1; t < 0.95; t += 0.05) worst = std::max(worst, std::fabs(pitchAt(tr, t) - bend::value(s.patterns[0].notes[ch][0].bend, t * 192)));
    std::printf("# ease in played: worst %.3f st from the drawn curve\n", worst);
    expect(worst < 0.15, "a built-in instrument's pitch follows an eased curve as drawn");
  }

  // ------------------------------------------------ the slide plan
  {
    const bend::SlidePlan st = bend::toSlides({{48, 5, bend::Straight}}, 96);
    expect(st.start == 0 && st.pieces.size() == 1 && st.pieces[0].tick == 0 && st.pieces[0].length == 48 && st.pieces[0].target == 5,
           "a straight ramp is one slide, as long as the ramp, aiming at its end");
    const bend::SlidePlan sm = bend::toSlides({{96, 7, bend::Smooth}}, 192);
    double worst = 0;
    for (const auto& pc : sm.pieces) worst = std::max(worst, std::fabs(pc.target - bend::value({{96, 7, bend::Smooth}}, static_cast<double>(pc.tick + pc.length))));
    expect(sm.curves == 1 && sm.pieces.size() >= 4 && sm.pieces.size() <= 16 && worst < 1e-6 && sm.pieces.back().tick + sm.pieces.back().length == 96,
           "a smooth glide is several shorter slides landing on the curve");
    const bend::SlidePlan sp = bend::toSlides({{0, 2}, {40, 2}, {40, -3, bend::Step}, {80, -3}}, 96);
    expect(near(sp.start, 2, 1e-9) && sp.pieces.size() == 1 && sp.pieces[0].tick == 40 && sp.pieces[0].length == 1 && sp.pieces[0].target == -3,
           "a start value goes to the note itself, a step is a one-tick slide, a hold is nothing");
    const bend::SlidePlan sr = bend::toSlides({{10, 3, bend::Step}, {30, 6}}, 96);
    expect(sr.pieces.size() == 2 && sr.pieces[1].tick == 11 && sr.pieces[1].length == 19, "pieces never overlap (a ramp starts once a step has landed)");
    const bend::SlidePlan past = bend::toSlides({{48, 4}, {200, -8}}, 96);
    expect(!past.pieces.empty() && past.pieces.back().tick + past.pieces.back().length <= 96, "a line past the note's end stops at its end");
  }

  // ------------------------------------------------ FL export: slides, proven by ear
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", wav);
    // A start a tone up, a straight ramp, a smooth S, an ease out, a hold, a step down.
    s.patterns[0].notes[ch] = {mk(0, 768, 60,
                                  {{0, 2}, {96, 2}, {192, 7, bend::Straight}, {288, 0, bend::Smooth}, {384, 5, bend::EaseOut, 0.4f}, {480, 5},
                                   {480, -3, bend::Step}, {672, 4, bend::EaseIn}})};
    Song back;
    flp::ExportReport rep;
    expect(flRoundTrip(s, back, rep), "a bent note exports to FL and reads back");
    std::printf("# report:");
    for (const auto& [k, v] : rep.counts)
      if (k.find("pitch bend") != std::string::npos) std::printf(" [%s: %d]", k.c_str(), v);
    std::printf("\n");
    const std::vector<Note>* ns = firstNotes(back);
    int slides = 0, plain = 0;
    bool noLines = true;
    if (ns)
      for (const Note& n : *ns) {
        slides += n.slide, plain += !n.slide;
        noLines = noLines && n.bend.empty();
      }
    std::printf("# reimported: %d note, %d slide notes\n", plain, slides);
    expect(rep.counts["notes: pitch bend lines written as slide notes"] == 1 && rep.counts["notes: pitch bend curves written as several shorter slides (approximated)"] == 3 &&
               rep.counts["notes: pitch bend steps written as one-tick slides"] == 1,
           "the report counts the line, its approximated curves and its step");
    expect(plain == 1 && slides >= 7 && noLines, "reimported: the note plus its slide notes (they stay slides, no line is guessed back)");
    if (ns)
      for (const Note& n : *ns)
        if (!n.slide) expect(n.pitch == 62 && n.fine == 0, "the note starts where its line starts (a tone up)");
    const AudioBuffer orig = render(s), fl = render(back);
    // (the cycle that straddles the step's jump at 2.5 s is measured as neither pitch: skipped)
    Compare c = compare(orig, fl, f0, 0.02, 2.49), c2 = compare(orig, fl, f0, 2.52, 3.95);
    c.meanCents = (c.meanCents * c.n + c2.meanCents * c2.n) / std::max(1, c.n + c2.n);
    c.maxCents = std::max(c.maxCents, c2.maxCents);
    c.n += c2.n;
    std::printf("# pitch tracks, Gridline line vs FL slides: %d windows, mean %.1f cents, max %.1f cents\n", c.n, c.meanCents, c.maxCents);
    expect(c.n > 600 && c.meanCents < 3.0 && c.maxCents < 30.0, "the exported slides play the bend's pitch track (mean within 3 cents, at most 30)");
    // Away from the step (where block timing decides within a few ms) the tracks agree closely.
    const Compare calm = compare(orig, fl, f0, 0.02, 2.45);
    std::printf("# before the step: mean %.1f cents, max %.1f cents\n", calm.meanCents, calm.maxCents);
    expect(calm.maxCents < 25.0, "ramps and curves stay within 25 cents of the line");

    // Fractions of a semitone: fine pitch, in 10 cent steps.
    Song f = baseSong();
    const ObjectId fc = addChannel(f, "gl.sampler", wav);
    f.patterns[0].notes[fc] = {mk(0, 384, 60, {{96, 1.5f}, {192, -0.75f}})};
    Song fb;
    flp::ExportReport fr;
    expect(flRoundTrip(f, fb, fr), "fractional bends export");
    const Compare cf = compare(render(f), render(fb), f0, 0.02, 1.9);
    std::printf("# fractional: mean %.1f cents, max %.1f cents; rounded %d\n", cf.meanCents, cf.maxCents,
                fr.counts["notes: pitch bend lines rounded to 10 cent fine pitch steps"]);
    expect(cf.maxCents < 25.0 && fr.counts["notes: pitch bend lines rounded to 10 cent fine pitch steps"] == 1,
           "fractional targets play through fine pitch (rounded to 10 cents, reported)");
  }

  // ------------------------------------------------ chords and neighbours
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", wav);
    const std::vector<BendPoint> up = {{96, 2}};
    // A chord bending as one (same start, same line), then a bent note under a held note.
    s.patterns[0].notes[ch] = {mk(0, 192, 60, up), mk(0, 192, 64, up), mk(0, 192, 67, up), mk(384, 192, 48), mk(384, 96, 60, {{48, 5}})};
    Song back;
    flp::ExportReport rep;
    expect(flRoundTrip(s, back, rep), "chords export");
    expect(rep.counts["notes: pitch bend lines written as slide notes"] == 3 &&
               rep.counts["notes: pitch bend lines not written (other notes of their colour sound with them, a slide would glide those too)"] == 1,
           "a chord bending as one becomes one set of slides; a line over another sounding note is left out and reported");
    const std::vector<Note>* ns = firstNotes(back);
    int slides = 0;
    if (ns)
      for (const Note& n : *ns) slides += n.slide;
    expect(slides == 1, "one slide glides the whole chord");
    const AudioBuffer a = render(s), b = render(back);
    double worst = 0;
    for (double hz : {f0, f0 * std::pow(2.0, 4 / 12.0), f0 * std::pow(2.0, 7 / 12.0)}) {
      // the chord's top at the end of the bend: each voice a tone up
      const double target = hz * std::pow(2.0, 2 / 12.0);
      auto lvl = [&](const AudioBuffer& x) {
        const size_t i0 = static_cast<size_t>(0.6 * kSr), i1 = static_cast<size_t>(0.9 * kSr);
        const double w = 2 * kPi * target / kSr, c = 2 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (size_t i = i0; i < i1; ++i) {
          const double s0 = x.at(0, i) + c * s1 - s2;
          s2 = s1, s1 = s0;
        }
        return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(i1 - i0);
      };
      worst = std::max(worst, std::fabs(lvl(a) - lvl(b)) / std::max(1e-9, lvl(a)));
    }
    std::printf("# chord voices a tone up: worst level difference %.1f %%\n", worst * 100);
    expect(worst < 0.15, "every chord voice lands a tone up after the FL round trip, as in Gridline");

    // Colours keep slides apart: a bent note over a held note of another colour converts.
    Song c = baseSong();
    const ObjectId cc = addChannel(c, "gl.sampler", wav);
    Note held = mk(0, 192, 48);
    held.color = 3;
    c.patterns[0].notes[cc] = {held, mk(0, 192, 60, {{96, 3}})};
    Song cb;
    flp::ExportReport cr;
    expect(flRoundTrip(c, cb, cr) && cr.counts["notes: pitch bend lines written as slide notes"] == 1,
           "a note of another colour does not stop the conversion (slides glide their own colour)");
  }

  // ------------------------------------------------ an imported FL pattern that gets a bend is written again
  {
    Song s = baseSong();
    const ObjectId ch = addChannel(s, "gl.sampler", wav);
    s.patterns[0].notes[ch] = {mk(0, 192, 60)};
    Song back;
    flp::ExportReport rep;
    std::vector<uint8_t> bytes;
    flp::ExportOptions opt;
    opt.renderInstruments = false;
    flp::Report ir;
    std::string err;
    expect(flp::exportFlp(s, bytes, rep, opt) && importFlpBytes(bytes, back, ir, &err), "a plain pattern round trips (with its FL origin)");
    for (auto& p : back.patterns)
      for (auto& [c, ns] : p.notes)
        for (Note& n : ns) n.bend = {{96, 4}};
    flp::ExportReport rep2;
    std::vector<uint8_t> again;
    Song third;
    flp::Report ir2;
    expect(flp::exportFlp(back, again, rep2, opt) && importFlpBytes(again, third, ir2, &err, "", "", false) &&
               rep2.counts["notes: pitch bend lines written as slide notes"] == 1,
           "a bend drawn on an imported note changes the pattern, which is written again with its slides");
    const std::vector<Note>* tn = firstNotes(third);
    int slides = 0;
    if (tn)
      for (const Note& n : *tn) slides += n.slide;
    expect(slides == 1, "the slide is in the file");
  }
  std::remove(wav.c_str());
  return finish();
}
