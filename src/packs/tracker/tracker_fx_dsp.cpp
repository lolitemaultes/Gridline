// Gridline - "tracker" pack, K-435 / K-436: a Tracker Sampler's samples as files of their own, and
// Gridline's own effects run over a sample (the Sample editor's Effects box). UI thread, no App.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../../core/PluginInfo.h"
#include "../../engine/Effects.h"
#include "../Packs.h"
#include "tracker.h"
#include "tracker_session.h"

namespace gl::packs::trackerpack {

namespace fs = std::filesystem;

namespace {

// A file name made of a sample's name: no separators or characters some systems refuse.
std::string fileSafe(const std::string& s) {
  std::string o;
  for (unsigned char ch : s) {
    const bool bad = ch < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|';
    o += bad ? '_' : static_cast<char>(ch);
  }
  while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
  while (!o.empty() && o.front() == ' ') o.erase(o.begin());
  return o;
}

std::string twoDigits(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02X", v & 0xFF);
  return b;
}

}  // namespace

int exportSampleFiles(const PluginState& st, const std::string& path, std::vector<std::string>* written) {
  if (st.type != trk::kInstrumentType || path.empty()) return 0;
  const std::shared_ptr<const InstrumentAux> inst = instrumentFor(st);
  if (!inst) return 0;
  const Edit e = readEdit(st.data, soundOf(st));
  std::vector<int> slots;
  for (int k = 0; k < trk::kMaxSamples; ++k)
    if (hasSample(e, k) && inst->files[static_cast<size_t>(k)]) slots.push_back(k);
  if (slots.empty()) return 0;
  const fs::path p = fs::u8path(path);
  const fs::path dir = p.parent_path();
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string stem = p.stem().u8string();
  int n = 0;
  for (int k : slots) {
    Wave w;
    if (!waveOf(e, k, *inst, w)) continue;
    // The part the instrument plays (its sample start to its end), the audio as it is stored.
    const Span part{std::max(0, w.cutA), w.cutB > 0 ? w.cutB : w.frames()};
    fs::path to = p;
    if (slots.size() > 1) {  // "<name> 01 <sample>.wav", one file per sample
      const std::string nm = fileSafe(editName(e, k));
      to = dir / fs::u8path(stem + " " + twoDigits(k + 1) + (nm.empty() ? std::string() : " " + nm) + ".wav");
    }
    if (!writeWaveFile(to.u8string(), w, part)) continue;
    if (written) written->push_back(to.u8string());
    ++n;
  }
  return n;
}

bool sampleEffectAllowed(const PluginInfo& info) {
  if (info.kind != PluginKind::Effect || info.hidden || isHostedType(info.type) || info.usesSample || info.midiInput) return false;
  if (!info.outputs.empty() || !info.sendParams.empty() || info.sideInParam >= 0 || info.routeModeParam >= 0) return false;
  // Effects that only route, measure or reach outside the sound they are given.
  static const char* const kOut[] = {"gl.send", "gl.analyzer", "gl.hwinsert", "gl.patcherfx", "gl.receive", "gl.sidechain"};
  for (const char* t : kOut)
    if (info.type == t) return false;
  return true;
}

std::vector<const PluginInfo*> sampleEffects() {
  std::vector<const PluginInfo*> out;
  for (const PluginInfo& p : pluginCatalog())
    if (sampleEffectAllowed(p) && gl::createEffect(p.type)) out.push_back(&p);
  std::stable_sort(out.begin(), out.end(), [](const PluginInfo* a, const PluginInfo* b) {
    return a->category != b->category ? a->category < b->category : a->name < b->name;
  });
  return out;
}

bool waveEffects(Wave& w, Span r0, const std::vector<FxStep>& chain, double tailSeconds, double tempo) {
  if (w.L.empty() || chain.empty()) return false;
  const Span r = r0.empty() ? Span{0, w.frames()} : clampSpan(w, r0);
  if (r.empty()) return false;
  const double rate = w.rate > 0 ? w.rate : 44100.0;
  const int32_t len = r.b - r.a;
  const auto tail = static_cast<int32_t>(std::clamp(std::lround(std::max(0.0, tailSeconds) * rate), 0L, static_cast<long>(rate * kMaxEffectTail)));
  const size_t n = static_cast<size_t>(len) + static_cast<size_t>(tail);
  std::vector<float> L(n, 0.0f), R(n, 0.0f);
  for (int32_t i = 0; i < len; ++i) {
    L[static_cast<size_t>(i)] = w.L[static_cast<size_t>(r.a + i)];
    R[static_cast<size_t>(i)] = w.stereo ? w.R[static_cast<size_t>(r.a + i)] : L[static_cast<size_t>(i)];
  }
  bool ran = false;
  constexpr int kBlock = 256;
  for (const FxStep& s : chain) {
    const PluginInfo* info = findPlugin(s.state.type);
    if (!info || !sampleEffectAllowed(*info)) continue;
    std::unique_ptr<Effect> fx = gl::createEffect(s.state.type);
    if (!fx) continue;
    PluginState st = s.state;
    normalizePluginState(st);
    ChannelAux aux;
    packs::prepareAux(st, aux);
    fx->reset();
    // The effect's own delay (look-ahead, linear phase): its output is taken that much later, so the
    // sound stays where it was in the sample.
    const int lat = std::clamp(fx->latencyFor(st, rate), 0, static_cast<int>(rate * 2));
    std::vector<float> xl(n + static_cast<size_t>(lat), 0.0f), xr(n + static_cast<size_t>(lat), 0.0f);
    std::copy(L.begin(), L.end(), xl.begin());
    std::copy(R.begin(), R.end(), xr.begin());
    ProcessContext ctx;
    ctx.sampleRate = rate;
    ctx.tempo = tempo > 0 ? tempo : 120.0;
    ctx.offline = true;
    ctx.playing = true;
    const double ticksPerFrame = ctx.tempo / 60.0 * ctx.ppq / rate;
    for (size_t i = 0; i < xl.size(); i += kBlock) {
      const int m = static_cast<int>(std::min<size_t>(kBlock, xl.size() - i));
      fx->processAux(st, aux, ctx, xl.data() + i, xr.data() + i, m);
      ctx.tick += m * ticksPerFrame;
      ctx.jumped = false;
    }
    const float mix = static_cast<float>(std::clamp(s.mix, 0.0, 1.0));
    for (size_t i = 0; i < n; ++i) {
      float a = xl[i + static_cast<size_t>(lat)], b = xr[i + static_cast<size_t>(lat)];
      if (!std::isfinite(a)) a = 0.0f;
      if (!std::isfinite(b)) b = 0.0f;
      L[i] = L[i] * (1.0f - mix) + a * mix;
      R[i] = R[i] * (1.0f - mix) + b * mix;
    }
    ran = true;
  }
  if (!ran) return false;
  // A mono sample becomes stereo when the effects made its sides differ.
  bool sides = w.stereo;
  for (size_t i = 0; i < n && !sides; ++i) sides = std::fabs(L[i] - R[i]) > 1e-6f;
  if (sides && !w.stereo) {
    w.R = w.L;
    w.stereo = true;
  }
  for (int32_t i = 0; i < len; ++i) {
    w.L[static_cast<size_t>(r.a + i)] = L[static_cast<size_t>(i)];
    if (w.stereo) w.R[static_cast<size_t>(r.a + i)] = R[static_cast<size_t>(i)];
  }
  // The tail: over what follows the part, then (past the sample's end) as new frames, without the
  // silence at its very end.
  const int32_t before = w.frames();
  size_t keep = n;
  while (keep > static_cast<size_t>(len) && std::fabs(L[keep - 1]) < 1e-5f && std::fabs(R[keep - 1]) < 1e-5f) --keep;
  for (size_t i = static_cast<size_t>(len); i < keep; ++i) {
    const size_t at = static_cast<size_t>(r.b) + (i - static_cast<size_t>(len));
    if (at < w.L.size()) {
      w.L[at] += L[i];
      if (w.stereo) w.R[at] += R[i];
    } else {
      w.L.push_back(L[i]);
      if (w.stereo) w.R.push_back(R[i]);
    }
  }
  if (w.frames() > before && w.cutB >= before) w.cutB = 0;  // (a sample that played to its end still does)
  return true;
}

}  // namespace gl::packs::trackerpack
