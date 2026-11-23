// Gridline: the audio clip editor's "Edit audio" operations. See ClipAudioEdit.h.
#include "ClipAudioEdit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../core/Automation.h"
#include "../packs/edison/edison.h"
#include "ClipOps.h"
#include "ClipSource.h"
#include "Sample.h"
#include "SampleWarpRender.h"

namespace gl::clipedit {

namespace ed = packs::edison;
namespace fs = std::filesystem;

namespace {

struct Info {
  const char* name;
  const char* hint;
  bool range;
  bool dialog;
};

const Info kInfo[static_cast<int>(Op::Count)] = {
    {"Cut", "Removes the selection from the file (the audio after it moves up)", true, false},
    {"Silence", "Makes the selection silent", true, false},
    {"Insert silence", "Inserts silence at the selection's start", true, true},
    {"Trim to selection", "Keeps only the selection", true, false},
    {"Trim side silence", "Removes the quiet parts at the file's start and end", false, false},
    {"Fade in", "Fades the selection in", true, false},
    {"Fade out", "Fades the selection out", true, false},
    {"Normalize", "Brings the selection's peak to 0 dB", true, false},
    {"Amplify", "Changes the selection's level by a number of dB", true, true},
    {"Reverse", "Plays the selection backwards (in the file)", true, false},
    {"Invert polarity", "Turns the selection's waveform upside down", true, false},
    {"Remove DC", "Removes a constant offset from the selection", true, false},
    {"Gate", "Silences the parts of the selection under a threshold", true, true},
    {"Denoise", "Reduces steady background noise (the quietest part of the selection is taken as the noise)", true, false},
    {"Declick", "Removes clicks and pops from the selection", true, false},
    {"Stretch and pitch", "Renders the selection longer or shorter and higher or lower, through the one stretch engine", true, true},
    {"Tune to note", "Shifts the selection so its pitch lands on the nearest note", true, false},
    {"Resample", "Converts the whole file to another sample rate", false, true},
    {"Equalize", "A low, mid and high tilt on the selection", true, true},
    {"Reverb", "Adds a reverb tail to the selection", true, true},
    {"Blur", "Smears the selection over a few milliseconds", true, true},
    {"Mono", "Mixes the file to one channel", false, false},
    {"Stereo", "Makes the file two channels", false, false},
    {"Swap channels", "Swaps left and right", false, false},
    {"Apply warp", "Renders the warp markers into a new file", false, false},
};

uint64_t hashAudio(const ed::Audio& a) {
  uint64_t h = 1469598103934665603ull;
  auto mixIn = [&h](const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  };
  mixIn(&a.rate, sizeof a.rate);
  mixIn(&a.channels, sizeof a.channels);
  if (!a.L.empty()) mixIn(a.L.data(), a.L.size() * sizeof(float));
  if (a.channels > 1 && !a.R.empty()) mixIn(a.R.data(), a.R.size() * sizeof(float));
  return h;
}

}  // namespace

const char* opName(Op o) { return o >= Op::Count ? "" : kInfo[static_cast<int>(o)].name; }
const char* opHint(Op o) { return o >= Op::Count ? "" : kInfo[static_cast<int>(o)].hint; }
bool opUsesRange(Op o) { return o < Op::Count && kInfo[static_cast<int>(o)].range; }
bool opHasDialog(Op o) { return o < Op::Count && kInfo[static_cast<int>(o)].dialog; }

Result apply(const std::string& src, int64_t a, int64_t b, Op op, const Args& args, const std::string& dir) {
  Result r;
  ed::Content c;
  std::string err;
  if (!ed::loadContent(src, c, &err) || c.frames() < 1) {
    r.error = err.empty() ? std::string("The audio could not be read") : err;
    return r;
  }
  const int64_t n = c.frames();
  ed::Range sel{std::clamp<int64_t>(a, 0, n), std::clamp<int64_t>(b, 0, n)};
  if (!opUsesRange(op) || sel.empty()) sel = ed::Range{0, n};
  ed::Content out;
  ed::EditMap map;
  switch (op) {
    case Op::Cut: out = ed::deleteRange(c, sel), map = ed::EditMap::replace(sel); break;
    case Op::Silence: out = ed::silenceRange(c, sel); break;
    case Op::InsertSilence: {
      const int64_t f = static_cast<int64_t>(std::llround(std::clamp(args.seconds, 0.001, 600.0) * c.rate()));
      out = ed::insertSilence(c, sel.a, f);
      map = ed::EditMap::replace(ed::Range{sel.a, sel.a});
      break;
    }
    case Op::TrimToSelection: out = ed::cropTo(c, sel), map = ed::EditMap::crop(sel); break;
    case Op::TrimSideSilence: out = ed::trimSilence(c, args.thresholdDb), map = ed::EditMap::trimSilence(args.thresholdDb); break;
    case Op::FadeIn: out = ed::fadeIn(c, sel, static_cast<ed::Curve>(std::clamp(args.curve, 0, static_cast<int>(ed::Curve::Count) - 1))); break;
    case Op::FadeOut: out = ed::fadeOut(c, sel, static_cast<ed::Curve>(std::clamp(args.curve, 0, static_cast<int>(ed::Curve::Count) - 1))); break;
    case Op::Normalize: out = ed::normalize(c, sel, std::min(0.0, args.db)); break;
    case Op::Amplify: out = ed::gain(c, sel, std::clamp(args.db, -60.0, 36.0)); break;
    case Op::Reverse: out = ed::reverse(c, sel); break;
    case Op::Invert: out = ed::invert(c, sel); break;
    case Op::RemoveDc: out = ed::removeDc(c, sel); break;
    case Op::Gate: out = ed::gate(c, sel, std::clamp(args.db, -96.0, 0.0), 1.0, 20.0, 60.0); break;
    case Op::Denoise: {
      // The quietest 100 ms of the selection is taken as the noise.
      const int64_t win = std::max<int64_t>(256, static_cast<int64_t>(c.rate() * 0.1));
      int64_t best = sel.a;
      double bestRms = 1e30;
      for (int64_t p = sel.a; p + win <= sel.b; p += win / 2) {
        double acc = 0;
        for (int64_t i = p; i < p + win; ++i) acc += static_cast<double>(c.a().L[static_cast<size_t>(i)]) * c.a().L[static_cast<size_t>(i)];
        if (acc < bestRms) bestRms = acc, best = p;
      }
      const ed::NoiseProfile prof = ed::acquireNoise(c.a(), ed::Range{best, std::min(sel.b, best + win)});
      out = prof.valid() ? ed::denoise(c, sel, prof) : c;
      break;
    }
    case Op::Declick: out = ed::declick(c, sel, 0.5); break;
    case Op::StretchPitch:
      out = ed::stretch(c, sel, std::clamp(args.factor, 0.125, 8.0), std::clamp(args.semitones, -24.0, 24.0), ed::Stretch::Elastic);
      map = ed::EditMap::replace(sel);
      break;
    case Op::TuneToNote: out = ed::tuneToNote(c, sel, args.note); break;
    case Op::Resample: out = ed::resample(c, std::clamp(args.rate, 8000.0, 192000.0)), map = ed::EditMap::scale(); break;
    case Op::Equalize:
      out = ed::equalize(c, sel, {{80, args.eqLowDb}, {200, args.eqLowDb * 0.5}, {1000, args.eqMidDb}, {5000, args.eqHighDb * 0.5}, {12000, args.eqHighDb}});
      break;
    case Op::Reverb: {
      ed::ReverbSettings rs;
      rs.wet = std::clamp(args.mix, 0.0, 1.0);
      out = ed::reverb(c, sel, rs);
      break;
    }
    case Op::Blur: out = ed::blur(c, sel, std::clamp(args.ms, 1.0, 1000.0), std::clamp(args.mix, 0.0, 1.0)); break;
    case Op::Mono: out = ed::toMono(c); break;
    case Op::Stereo: out = ed::toStereo(c); break;
    case Op::Swap: out = ed::swapChannels(c); break;
    case Op::ApplyWarp: {
      std::vector<WarpMarker> m = args.warp;
      normalizeWarp(m, c.a().seconds());
      if (m.size() < 2 || warpIsIdentity(m)) {
        r.error = "The warp markers move nothing";
        return r;
      }
      SamplePtr smp = ed::toSample(c.a(), src);
      SamplePtr w = smp ? renderWarpedSample(*smp, m) : nullptr;
      if (!w) {
        r.error = "The warp could not be rendered";
        return r;
      }
      out = ed::makeContent(ed::fromSample(*w));
      map = ed::EditMap::scale();
      break;
    }
    case Op::Count: r.error = "Unknown operation"; return r;
  }
  if (!out.audio || out.frames() < 1) {
    r.error = "The edit left no audio";
    return r;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  char name[96];
  std::snprintf(name, sizeof name, "%s %016llx.wav", fs::path(src).stem().string().substr(0, 40).c_str(),
                static_cast<unsigned long long>(hashAudio(out.a())));
  const std::string path = (fs::path(dir) / name).string();
  if (!fs::is_regular_file(path, ec) && !ed::writeWavFile(path, out, 32, &err)) {
    r.error = err.empty() ? std::string("The file could not be written") : err;
    return r;
  }
  r.path = path;
  r.frames = out.frames();
  r.rate = out.rate();
  const ed::Range after = map.kind == ed::EditMap::None ? sel : ed::mapSelection(c, out, map, sel);
  r.selA = after.a, r.selB = after.b;
  return r;
}

bool clipRangeFrames(const Song& s, ObjectId id, double t0, double t1, int64_t& a, int64_t& b) {
  const Clip* c = s.clip(id);
  const Channel* ch = c ? s.channel(c->source) : nullptr;
  if (!c || !ch || c->kind != ClipKind::Audio) return false;
  const ClipSource src = clipSource(*ch);
  if (!src.ok()) return false;
  TempoMap tm;
  tm.build(s);
  // The source the selection hears, pass by pass (a looping clip repeats its window): the range
  // from the earliest to the latest second it covers. Silence (after the audio ran out) adds none.
  const double lo = std::min(t0, t1), hi = std::max(t0, t1);
  double s0 = 0, s1 = 0;
  bool any = false;
  for (const AudibleSpan& sp : clipAudibleSpans(s, *c, tm)) {
    const double u0 = std::max(lo, sp.t0), u1 = std::min(hi, sp.t1);
    if (!(u1 > u0)) continue;
    const double a0 = std::clamp(clipSourceAt(s, sp.pass, tm, u0), sp.s0, sp.s1);
    const double a1 = u1 >= sp.t1 ? sp.s1 : std::clamp(clipSourceAt(s, sp.pass, tm, u1), sp.s0, sp.s1);
    s0 = any ? std::min(s0, std::min(a0, a1)) : std::min(a0, a1);
    s1 = any ? std::max(s1, std::max(a0, a1)) : std::max(a0, a1);
    any = true;
  }
  if (!any) return false;
  const double rate = src.sample->sampleRate;
  if (clipPlaysReversed(*ch, *c)) {
    const double last = static_cast<double>(src.sample->frames() - 1) / rate;
    s0 = last - s0, s1 = last - s1;
  }
  const int64_t n = static_cast<int64_t>(src.sample->frames());
  a = std::clamp<int64_t>(static_cast<int64_t>(std::llround(std::min(s0, s1) * rate)), 0, n);
  b = std::clamp<int64_t>(static_cast<int64_t>(std::llround(std::max(s0, s1) * rate)), 0, n);
  return b > a;
}

}  // namespace gl::clipedit
