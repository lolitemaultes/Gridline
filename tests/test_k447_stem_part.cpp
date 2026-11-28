// Gridline: splitting stems takes only the part of the audio the user chose.
//  * a clip that plays a short middle part of a long file splits that part alone: each stem file
//    holds the part (and a few frames of handles), not the file, and its audio is the separation
//    of that part; fewer segments are separated (it costs less time);
//  * the stems land lined up with the part, sample for sample: a stem that held exactly the part's
//    audio plays what the clip plays, for a slice, a piece the split tool left, a time selection
//    inside a clip, a reversed, a stretched, a looping clip, a channel whose sample page shapes its
//    sound (normalize, fades), and through warp markers;
//  * muting the original mutes only the part (the clip is cut at its edges);
//  * the placement and its undo reach the other person in a collaboration session.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "StemsTiny.h"
#include "collab/CollabSim.h"
#include "core/Automation.h"
#include "core/AudioBuffer.h"
#include "core/Platform.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "engine/ClipOps.h"
#include "engine/ClipRender.h"
#include "engine/ClipSource.h"
#include "engine/Export.h"
#include "engine/Sample.h"
#include "engine/stems/StemModels.h"
#include "engine/stems/StemPlace.h"
#include "engine/stems/StemSplit.h"

using namespace gl;
using namespace gl::stems;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 44100;
constexpr int64_t kFrames = int64_t(kRate) * 20;  // a 20 s "song"

std::string gDir;

// A 20 s stereo file whose every frame differs (sines that glide, a little noise).
std::string writeLongFile() {
  AudioBuffer b;
  b.resize(2, size_t(kFrames));
  uint64_t r = 99;
  for (int64_t i = 0; i < kFrames; ++i) {
    const double t = double(i) / kRate;
    r = r * 6364136223846793005ULL + 1442695040888963407ULL;
    const double noise = (double(r >> 40) / double(1ULL << 24) - 0.5) * 0.05;
    b.at(0, size_t(i)) = float(0.3 * std::sin(2 * kPi * (110 + 20 * t) * t) + noise);
    b.at(1, size_t(i)) = float(0.25 * std::sin(2 * kPi * (220 - 5 * t) * t + 0.3) - noise);
  }
  AudioFileOptions o;
  o.bits = 32;
  const std::string p = gDir + "/song.wav";
  writeAudioFile(p, b, kRate, o);
  return p;
}

struct Fixture {
  Song s;
  ObjectId ch = kNoId, clip = kNoId;
};

Tick ticksFor(const Song& s, Tick from, double seconds) {
  TempoMap tm;
  tm.build(s);
  return Tick(std::llround(tm.tickAtSeconds(tm.secondsAt(double(from)) + seconds) - double(from)));
}

Fixture fixture(const std::string& file, double audioStart, double seconds) {
  Fixture f;
  f.s = makeEmptySong();
  if (f.s.tracks.size() < 6) f.s.tracks.resize(6);
  Channel ch;
  ch.id = f.s.allocateId();
  ch.name = "Song";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = file;
  ch.audioClip = true;
  f.s.channels.push_back(ch);
  f.ch = ch.id;
  Clip c;
  c.id = f.s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.track = 1;
  c.start = 384;
  c.audioStart = audioStart;
  c.length = ticksFor(f.s, c.start, seconds);
  f.s.playlist.push_back(c);
  f.clip = c.id;
  // a clip further down, which the new tracks push down
  Clip below = c;
  below.id = f.s.allocateId();
  below.track = 2;
  below.audioStart = 0;
  f.s.playlist.push_back(below);
  return f;
}

void setParam(Song& s, ObjectId ch, int k, float v) {
  Channel* c = s.channel(ch);
  if (c->plugin.params.size() <= size_t(k)) c->plugin.params.resize(size_t(k) + 1, 0.f);
  c->plugin.params[size_t(k)] = v;
}

// Clip `id` cut at t0 and t1 (inside it): its piece in [t0, t1).
ObjectId pieceOf(Song& s, ObjectId id, Tick t0, Tick t1) {
  ObjectId mid = id;
  if (const Clip* c = s.clip(id); c && t0 > c->start)
    if (const ObjectId r = splitClipAt(s, id, t0); r != kNoId) mid = r;
  if (const Clip* c = s.clip(mid); c && t1 < c->start + s.clipLength(*c)) splitClipAt(s, mid, t1);
  return mid;
}

// A "stem" file holding exactly frames [from, to) of the part's sound (a separation that put
// everything into one stem), and a silent one.
std::vector<StemFile> identityStems(const StemPart& p, const std::string& tag) {
  AudioBuffer b, z;
  const size_t n = size_t(p.to - p.from);
  b.resize(2, n);
  z.resize(2, n);
  for (size_t i = 0; i < n; ++i) {
    b.at(0, i) = p.sound->left[size_t(p.from) + i];
    b.at(1, i) = p.sound->right[size_t(p.from) + i];
  }
  AudioFileOptions o;
  o.bits = 32;
  const std::string a = gDir + "/" + tag + " - Other.wav", q = gDir + "/" + tag + " - Vocals.wav";
  writeAudioFile(a, b, kRate, o);
  writeAudioFile(q, z, kRate, o);
  return {{"other", a}, {"vocals", q}};
}

// The stem channel plays its file as it is: no handles, no sample shaping left on.
bool plainChannel(const Channel& ch) {
  double a = 0, b = 1;
  channelTrim(ch, a, b);
  auto par = [&](int k) { return size_t(k) < ch.plugin.params.size() ? ch.plugin.params[size_t(k)] : 0.f; };
  const PluginInfo* info = findPlugin(ch.plugin.type);
  auto def = [&](int k) { return info && size_t(k) < info->params.size() ? info->params[size_t(k)].def : 0.f; };
  for (int k : {int(pp::SmpNormalize), int(pp::SmpFadeIn), int(pp::SmpPreBoost), int(pp::SmpPreReverbMix)})
    if (par(k) != def(k)) return false;
  return a == 0 && b == 1;
}

double maxDiff(const AudioBuffer& x, const AudioBuffer& y) {
  if (x.frames != y.frames || x.channels != y.channels) return 1e9;
  double m = 0;
  for (size_t i = 0; i < x.data.size(); ++i) m = std::max(m, double(std::fabs(x.data[i] - y.data[i])));
  return m;
}

double rms(const AudioBuffer& x) {
  double e = 0;
  for (float v : x.data) e += double(v) * v;
  return x.data.empty() ? 0 : std::sqrt(e / double(x.data.size()));
}

// How lineUp compares the renders: not at all, sample for sample, or (a clip the stretch engines
// play) the first and last 100 ms against the clip's own.
enum class Render { None, Exact, Edges };
// The stretch engine feeds its stream whole frames, rounding the map's position: two maps that agree
// to 1e-11 frames (the clip's, in seconds of the long file, and the stem's, in seconds of the short
// one) can round a position that falls on half a frame apart, and the phase vocoder then drifts in
// phase, not in time or level. So: each edge correlates at lag 0 to at least kEdgeCorrelation, better
// than at a lag of one to eight frames, at a level within kEdgeLevelDb.
constexpr double kEdgeCorrelation = 0.99, kEdgeLevelDb = 0.5;

double correlation(const AudioBuffer& x, const AudioBuffer& y, size_t i0, size_t i1, int lag) {
  double xy = 0, xx = 0, yy = 0;
  for (size_t ch = 0; ch < x.channels; ++ch)
    for (size_t i = i0; i < i1; ++i) {
      const int64_t j = int64_t(i) + lag;
      if (j < 0 || j >= int64_t(y.frames)) continue;
      const double a = x.at(ch, i), c = y.at(ch, size_t(j));
      xy += a * c, xx += a * a, yy += c * c;
    }
  return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0.0;
}

double levelDb(const AudioBuffer& x, const AudioBuffer& y, size_t i0, size_t i1) {
  double ex = 0, ey = 0;
  for (size_t ch = 0; ch < x.channels; ++ch)
    for (size_t i = i0; i < i1; ++i) ex += double(x.at(ch, i)) * x.at(ch, i), ey += double(y.at(ch, i)) * y.at(ch, i);
  return ex > 0 && ey > 0 ? 10.0 * std::log10(ey / ex) : 99.0;
}

// The part of the clip in [t0, t1) split with identity stems: the stem clip must play sample for
// sample what the clip's piece plays, where the piece is.
void lineUp(const char* what, Fixture f, Tick t0, Tick t1, Render render = Render::Exact) {
  StemPart part;
  const bool ok = clipStemPart(f.s, f.clip, t0, t1, part);
  expect(ok, (std::string(what) + ": the clip's part is found").c_str());
  if (!ok) return;
  const Clip& pc = *f.s.clip(f.clip);
  const int64_t handle = stemHandleFrames(f.s, *f.s.channel(pc.source), pc, kRate);
  expect(part.handle == handle && part.from == std::max<int64_t>(0, part.heardFrom - handle) &&
             part.to == std::min<int64_t>(part.frames, part.heardTo + handle),
         (std::string(what) + ": the split takes the heard frames and the handles the clip's playback reads").c_str());
  expect(part.to - part.from < part.frames / 2, (std::string(what) + ": far less than the file").c_str());
  Song before = f.s;
  const ObjectId pieceId = pieceOf(before, f.clip, part.t0, part.t1);
  const Clip piece = *before.clip(pieceId);
  const std::vector<StemFile> files = identityStems(part, what);
  Song after = f.s;
  const std::vector<ObjectId> made = placeStems(after, part, files, "Song", false);
  expect(made.size() == 2, (std::string(what) + ": two stem clips").c_str());
  if (made.size() != 2) return;
  const Clip* other = after.clip(made[1]);
  const Clip* vocals = after.clip(made[0]);
  expect(other && vocals && other->start == piece.start && other->length == piece.length && vocals->start == piece.start &&
             vocals->length == piece.length && after.clipLength(*other) == before.clipLength(piece),
         (std::string(what) + ": each stem clip has the part's position and length").c_str());
  expect(vocals && other && vocals->track == 2 && other->track == 3 && after.clip(f.clip)->track == 1,
         (std::string(what) + ": on new tracks right under the clip").c_str());
  const Channel* oc = other ? after.channel(other->source) : nullptr;
  double sa = 0, sb = 1;
  if (oc) channelTrim(*oc, sa, sb);
  expect(oc && sa == 0 && sb == 1 && plainChannel(*oc), (std::string(what) + ": the stem plays its file as it is").c_str());
  // the time map: the stem file's frame + the frames cut away = the clip's frame
  TempoMap tm;
  tm.build(after);
  double worst = 0;
  const Tick len = before.clipLength(piece);
  for (int k = 0; k < 16; ++k) {
    const double tick = double(piece.start) + double(len) * (k + 0.5) / 16.0;
    double so = clipSourceAt(before, piece, tm, tick), ss = clipSourceAt(after, *other, tm, tick);
    if (part.reversed) {
      so = double(part.frames - 1) - so * kRate, ss = double(part.to - part.from - 1) - ss * kRate;
    } else {
      so *= kRate, ss *= kRate;
    }
    worst = std::max(worst, std::fabs((ss + double(part.from)) - so));
  }
  char b[200];
  std::snprintf(b, sizeof b, "%s: the stem clip reads the same audio at every tick (worst %.2e frames)", what, worst);
  expect(worst < 1e-4, b);
  if (render == Render::None) return;
  const RenderResult ro = renderClipAlone(before, pieceId, kRate);
  const RenderResult rs = renderClipAlone(after, made[1], kRate);
  if (render == Render::Edges) {
    const size_t n = ro.audio.frames, e = size_t(kRate / 10);
    bool sized = ro.error.empty() && rs.error.empty() && n == rs.audio.frames && n > 4 * e && ro.audio.channels == rs.audio.channels;
    const double level = rms(ro.audio);
    for (int edge = 0; edge < 2 && sized; ++edge) {
      const size_t i0 = edge == 0 ? 0 : n - e, i1 = edge == 0 ? e : n;
      double d = 0, off = 0;
      for (size_t ch = 0; ch < ro.audio.channels; ++ch)
        for (size_t i = i0; i < i1; ++i) d = std::max(d, double(std::fabs(ro.audio.at(ch, i) - rs.audio.at(ch, i))));
      const double c0 = correlation(ro.audio, rs.audio, i0, i1, 0), db = levelDb(ro.audio, rs.audio, i0, i1);
      for (int lag = 1; lag <= 8; ++lag) off = std::max({off, correlation(ro.audio, rs.audio, i0, i1, lag), correlation(ro.audio, rs.audio, i0, i1, -lag)});
      std::snprintf(b, sizeof b, "%s: the stem clip's %s 100 ms play what the clip plays (correlation %.6f, %.6f at best off it, level %+.3f dB, max diff %.2e, rms %.3f)",
                    what, edge == 0 ? "first" : "last", c0, off, db, d, level);
      expect(level > 0.05 && c0 >= kEdgeCorrelation && c0 > off && std::fabs(db) <= kEdgeLevelDb, b);
    }
    expect(sized, (std::string(what) + ": the stem clip renders as long as the clip").c_str());
    return;
  }
  const double d = maxDiff(ro.audio, rs.audio);
  std::snprintf(b, sizeof b, "%s: the stem clip plays sample for sample what the clip plays (%zu frames, max diff %.2e, rms %.3f)", what,
                ro.audio.frames, d, rms(ro.audio));
  expect(ro.error.empty() && rs.error.empty() && ro.audio.frames > 1000 && rms(ro.audio) > 0.05 && d < 1e-5, b);
}

}  // namespace

int main() {
  const std::string home = gl::test::hermeticTestEnv("k447stems");
  gDir = (fs::path(home) / "work").u8string();
  std::error_code ec;
  fs::create_directories(gDir, ec);
  const std::string tiny = gDir + "/tiny.th";
  // the test model: in the catalogue when the variable is set before its first use
  platform::setEnv("GRIDLINE_STEMS_TEST_MODEL", tiny.c_str());
  platform::setEnv("GRIDLINE_STEM_MODELS", (gDir + "/models").c_str());
  std::string err;
  expect(gl::stemtiny::writeTiny(tiny, &err), "the tiny model is written");
  const std::string file = writeLongFile();

  // ---- a long file whose clip plays a 2 s middle part: only that part is separated
  {
    Fixture f = fixture(file, 8.0, 2.0);
    StemPart part;
    expect(clipStemPart(f.s, f.clip, 0, 0, part), "the clip's part");
    const int64_t want0 = int64_t(8.0 * kRate);
    char b[240];
    std::snprintf(b, sizeof b, "the part is the 2 s the clip plays from 8 s (frames %lld..%lld of %lld)", static_cast<long long>(part.heardFrom),
                  static_cast<long long>(part.heardTo), static_cast<long long>(part.frames));
    TempoMap tm;
    tm.build(f.s);
    const double heardSec = tm.secondsAt(double(f.s.clip(f.clip)->start + f.s.clip(f.clip)->length)) - tm.secondsAt(double(f.s.clip(f.clip)->start));
    expect(part.frames == kFrames && part.heardFrom == want0 && std::llabs(part.heardTo - (want0 + int64_t(std::llround(heardSec * kRate)))) <= 2 &&
               part.wholeClip,
           b);
    ExtractRequest req;
    req.input = file;
    req.modelId = "test";
    req.outDir = gDir + "/Stems";
    req.baseName = "Song";
    req.threads = 1;
    req.sound = part.sound;
    req.start = part.startSeconds();
    req.end = part.endSeconds();
    int calls = 0;
    std::vector<std::pair<std::string, std::string>> written;
    const bool ok = extractStems(req, [&](double) { ++calls; }, nullptr, &written, &err);
    expect(ok && written.size() == 4, (std::string("the part splits into four stems ") + err).c_str());
    // the same part separated on its own, and the whole file's segment count
    AudioBuffer in;
    in.resize(2, size_t(part.to - part.from));
    for (int64_t i = part.from; i < part.to; ++i) in.at(0, size_t(i - part.from)) = part.sound->left[size_t(i)], in.at(1, size_t(i - part.from)) = part.sound->right[size_t(i)];
    SplitRequest sr;
    sr.modelFiles = {tiny};
    sr.threads = 1;
    SplitResult ref;
    int refCalls = 0, wholeCalls = 0;
    separate(in, kRate, sr, ref, [&](double) { ++refCalls; }, nullptr, &err);
    {
      AudioBuffer whole;
      whole.resize(2, size_t(kFrames));
      SplitResult wr;
      separate(whole, kRate, sr, wr, [&](double) { ++wholeCalls; }, nullptr, &err);
    }
    std::snprintf(b, sizeof b, "only the part's segments are separated (%d, the whole file has %d)", calls, wholeCalls);
    expect(calls == refCalls && calls > 0 && calls * 5 < wholeCalls, b);
    bool lengths = ok, same = ok;
    double worst = 0;
    for (size_t k = 0; k < written.size(); ++k) {
      SamplePtr w = loadSample(written[k].second);
      lengths = lengths && w && int64_t(w->frames()) == part.to - part.from;
      size_t si = 0;
      while (si < ref.sources.size() && ref.sources[si] != written[k].first) ++si;
      if (!w || si >= ref.sources.size()) {
        same = false;
        continue;
      }
      for (size_t i = 0; i < w->frames(); ++i) {
        worst = std::max(worst, double(std::fabs(w->left[i] - ref.stems[si].at(0, i))));
        worst = std::max(worst, double(std::fabs(w->right[i] - ref.stems[si].at(1, i))));
      }
    }
    std::snprintf(b, sizeof b, "each stem file holds the part alone: %lld frames (the file has %lld)", static_cast<long long>(part.to - part.from),
                  static_cast<long long>(kFrames));
    expect(lengths && part.to - part.from <= part.heardTo - part.heardFrom + 2 * kStemHandleFrames, b);
    std::snprintf(b, sizeof b, "... and its sound is the separation of that part (max diff %.2e)", worst);
    expect(same && worst < 1e-6, b);
    // the real stems, placed: each clip at the part, reading its stem from the part's first frame
    std::vector<StemFile> files;
    for (const auto& w : written) files.push_back({w.first, w.second});
    Song after = f.s;
    const std::vector<ObjectId> made = placeStems(after, part, files, "Song", true);
    expect(made.size() == 4 && after.clip(f.clip)->muted && after.clip(f.s.playlist[1].id)->track == 6,
           "four stem clips under the clip, the clip muted, the clip below moved down");
    // (the render carries the channel's level and the clip's edge declick: compared by correlation,
    // away from the first and last 10 ms, at lag 0 against lags of 1 to 8 frames either way)
    bool aligned = made.size() == 4;
    double wa = 1, worstOff = 0;
    for (ObjectId id : made) {
      const Clip* c = after.clip(id);
      RenderResult r = renderClipAlone(after, id, kRate);
      SamplePtr w = c ? loadSample(after.channel(c->source)->plugin.data) : nullptr;
      aligned = aligned && c && w && c->start == f.s.clip(f.clip)->start && c->length == f.s.clip(f.clip)->length && r.error.empty();
      if (!aligned) break;
      const int64_t lead = part.heardFrom - part.from, edge = kRate / 100;
      auto corr = [&](int64_t lag) {
        double xy = 0, xx = 0, yy = 0;
        for (int64_t i = edge; i + edge < int64_t(r.audio.frames); ++i) {
          const int64_t j = lead + i + lag;
          if (j < 0 || j >= int64_t(w->frames())) continue;
          const double x = r.audio.at(0, size_t(i)), y = w->left[size_t(j)];
          xy += x * y, xx += x * x, yy += y * y;
        }
        return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0.0;
      };
      const double c0 = corr(0);
      wa = std::min(wa, c0);
      for (int64_t lag = 1; lag <= 8; ++lag) worstOff = std::max({worstOff, corr(lag), corr(-lag)});
    }
    std::snprintf(b, sizeof b, "each placed stem plays its file from the part's first frame on, in time (correlation %.6f at lag 0, %.6f at best off it)", wa,
                  worstOff);
    expect(aligned && wa > 0.9999 && worstOff < wa, b);
    // the request's range: frames by rounding, clamped to the file
    ExtractRequest q;
    int64_t a0 = 0, a1 = 0;
    extractRange(q, kRate, 1000, a0, a1);
    expect(a0 == 0 && a1 == 1000, "no range: the whole file");
    q.start = 0.01, q.end = 99;
    extractRange(q, kRate, 1000, a0, a1);
    expect(a0 == 441 && a1 == 1000, "a range: rounded to frames, clamped to the file");
  }

  // ---- the stems line up with the part, sample for sample
  lineUp("a slice of a long file", fixture(file, 8.0, 2.0), 0, 0);
  {  // the split tool: the middle piece of a clip that played the whole song
    Fixture f = fixture(file, 0.0, 19.0);
    const Tick a = 384 + ticksFor(f.s, 384, 6.0), b = 384 + ticksFor(f.s, 384, 7.5);
    const ObjectId mid = pieceOf(f.s, f.clip, a, b);
    f.clip = mid;
    lineUp("a piece the split tool left", f, 0, 0);
  }
  {  // a time selection inside a clip
    Fixture f = fixture(file, 3.0, 8.0);
    const Tick a = 384 + ticksFor(f.s, 384, 2.0), b = 384 + ticksFor(f.s, 384, 3.25);
    StemPart p;
    expect(clipStemPart(f.s, f.clip, a, b, p) && p.t0 == a && p.t1 == b && !p.wholeClip && std::llabs(p.heardFrom - int64_t(5.0 * kRate)) < 400,
           "a time selection takes the clip's part inside it");
    lineUp("a time selection inside the clip", f, a, b);
    // muting the original: only that part
    std::vector<StemFile> files = identityStems(p, "mute");
    Song after = f.s;
    placeStems(after, p, files, "Song", true);
    int muted = 0, pieces = 0;
    for (const Clip& c : after.playlist)
      if (c.source == f.ch && c.track == 1) {
        ++pieces;
        if (c.muted) muted += (c.start == a && after.clipLength(c) == b - a) ? 1 : 100;
      }
    expect(pieces == 3 && muted == 1, "muting the original mutes only the part: the clip is cut at its edges");
    StemPart none;
    expect(!clipStemPart(f.s, f.clip, 0, 300, none), "a selection that misses the clip has no part");
  }
  {  // reversed
    Fixture f = fixture(file, 0.0, 1.5);
    Clip& c = *f.s.clip(f.clip);
    c.audioStart = 11.0;
    c.audioEnd = 12.5;
    toggleClipReverse(f.s, c);  // the same window, backwards
    lineUp("a reversed clip", f, 0, 0);
  }
  {  // stretched (resampled: between frames)
    Fixture f = fixture(file, 6.0, 3.0);
    f.s.clip(f.clip)->stretch = 1.37;
    lineUp("a stretched clip", f, 0, 0);
  }
  {  // the channel's sample page shapes the sound: the stems hold it
    Fixture f = fixture(file, 4.0, 2.0);
    setParam(f.s, f.ch, pp::SmpNormalize, 1.f);
    setParam(f.s, f.ch, pp::SmpFadeIn, 0.25f);
    setParam(f.s, f.ch, pp::SmpPreBoost, 0.2f);
    lineUp("a channel with normalize, a fade and a boost", f, 0, 0);
  }
  {  // looping: a clip longer than its window
    Fixture f = fixture(file, 9.0, 3.0);
    f.s.clip(f.clip)->audioEnd = 10.0;
    f.s.clip(f.clip)->loop = true;
    lineUp("a looping clip", f, 0, 0);
    StemPart p;
    clipStemPart(f.s, f.clip, 0, 0, p);
    expect(p.heardFrom == int64_t(9.0 * kRate) && p.heardTo <= int64_t(10.0 * kRate) + 2, "a loop splits its window once");
  }
  {  // warp markers: the time map stays the same (the sections stretch with the pitch kept)
    Fixture f = fixture(file, 5.0, 2.0);
    Clip& c = *f.s.clip(f.clip);
    c.warp = {{1, 4.0, 4.0}, {2, 5.5, 5.8}, {3, 6.5, 6.6}, {4, 9.0, 9.1}};
    lineUp("a clip with warp markers", f, 0, 0, Render::None);
  }
  {  // the Stretch mode: longer with the pitch kept, the handles hold what the stretch reads
    Fixture f = fixture(file, 6.0, 3.0);
    f.s.clip(f.clip)->stretch = 1.37;
    setParam(f.s, f.ch, pp::SmpStretchMode, 1.f);
    StemPart p;
    clipStemPart(f.s, f.clip, 0, 0, p);
    expect(p.handle == int64_t(std::ceil(kStemStretchHandleSeconds * kRate)), "the Stretch mode's handles are half a second");
    lineUp("a clip in the Stretch mode", f, 0, 0, Render::Edges);
  }
  {  // warped: through the markers, rendered
    Fixture f = fixture(file, 5.0, 2.0);
    f.s.clip(f.clip)->warp = {{1, 4.0, 4.0}, {2, 5.5, 5.8}, {3, 6.5, 6.6}, {4, 9.0, 9.1}};
    StemPart p;
    clipStemPart(f.s, f.clip, 0, 0, p);
    expect(p.handle >= int64_t(std::ceil(kStemStretchHandleSeconds * kRate)), "a warped clip's handles are at least half a second");
    lineUp("a warped clip", f, 0, 0, Render::Edges);
  }

  // ---- collaboration: the placement and its undo reach the other person
  {
    using namespace gl::collab;
    using namespace sim;
    Clock clock;
    Net net(447);
    Peer a, b;
    a.label = "Ann", b.label = "Ben";
    for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
    Fixture f = fixture(file, 3.0, 8.0);
    a.doc.reset(f.s);
    a.folder = tmpDir("k447_a");
    b.folder = tmpDir("k447_b");
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
    expect(a.s != nullptr, "a session");
    if (a.s) {
      net.add(&a);
      b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
      a.s->admitMember(memberOf(b));
      net.add(&b);
      net.connect(&a, &b);
      settle(net, {&a, &b}, clock);
      const Tick t0 = 384 + ticksFor(f.s, 384, 2.0), t1 = 384 + ticksFor(f.s, 384, 3.25);
      StemPart p;
      clipStemPart(a.doc.song(), f.clip, t0, t1, p);
      const std::vector<StemFile> files = identityStems(p, "collab");
      std::vector<ObjectId> made;
      a.doc.editSong("split stems", [&](Song& x) { made = placeStems(x, p, files, "Song", true); });
      settle(net, {&a, &b}, clock);
      run(net, {&a, &b}, clock, 1.0);
      const Clip* bo = made.size() == 2 ? b.doc.song().clip(made[1]) : nullptr;
      const Clip* ao = made.size() == 2 ? a.doc.song().clip(made[1]) : nullptr;
      expect(bo && ao && bo->start == t0 && bo->length == t1 - t0 && bo->audioStart == ao->audioStart && bo->audioEnd == ao->audioEnd,
             "the stem clips reach Ben lined up with the part");
      int bMuted = 0;
      for (const Clip& c : b.doc.song().playlist) bMuted += c.source == f.ch && c.muted ? 1 : 0;
      expect(bMuted == 1, "the muted part of the original reaches Ben");
      const Channel* bc = bo ? b.doc.song().channel(bo->source) : nullptr;
      const bool fileHere = bc && fs::is_regular_file(fs::u8path(bc->plugin.data), ec) && !samePath(bc->plugin.data, files[0].path);
      expect(fileHere, "the stem file arrives as a file of Ben's own");
      if (fileHere) {
        SamplePtr mine = loadSample(files[0].path), theirs = loadSample(bc->plugin.data);
        expect(mine && theirs && mine->left == theirs->left, "with the same sound");
      }
      a.doc.undo();
      settle(net, {&a, &b}, clock);
      int clipsOf = 0;
      bool unmuted = true;
      for (const Clip& c : b.doc.song().playlist)
        if (c.source == f.ch && c.track == 1) ++clipsOf, unmuted = unmuted && !c.muted;
      expect(made.size() == 2 && !b.doc.song().clip(made[0]) && clipsOf == 1 && unmuted && b.doc.song().clip(f.clip)->length == f.s.clip(f.clip)->length,
             "Ann's undo takes the stems away on Ben's computer and gives the clip back whole");
      a.doc.redo();
      settle(net, {&a, &b}, clock);
      expect(made.size() == 2 && b.doc.song().clip(made[1]) != nullptr, "her redo brings them back");
    }
  }
  return finish();
}
