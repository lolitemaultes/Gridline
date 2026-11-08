// Gridline: `gridline plugin-xformat <from type> <to type>...` (K-78): does a plugin's state move between
// its installed formats? Runs both through the engine path a project uses:
//   1. the source format with test values on its parameters, its state captured and reloaded in a
//      second instance (what the project would hold);
//   2. that state converted to each target format (CrossFormat.h) and loaded there, the load-time read
//      back check included; its parameters compared by name with the source's;
//   3. the same with parameters only (the fallback), and both outputs compared with the source's on
//      the same input (a 220 Hz tone, or a held note for instruments).
// Prints one block per target; exit code 0 when every target took the state or the parameters.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../core/Json.h"
#include "../core/Params.h"
#include "../core/Project.h"
#include "../core/Song.h"
#include "../engine/Effects.h"
#include "../engine/Generators.h"
#include "CrossFormat.h"
#include "HostedPlugins.h"
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace gl::host {

namespace {

constexpr double kRate = 48000;
constexpr int kBlock = 512;

struct Instance {
  std::unique_ptr<Generator> g;
  std::unique_ptr<Effect> fx;
  HostedInstance* hi = nullptr;
};

bool make(const HostedClass& c, const PluginState& st, Instance& out) {
  if (c.instrument) {
    out.g = createGenerator(c.type);
    if (!out.g) return false;
    out.g->prepareHost(st, kRate);
    out.hi = hostedInstance(out.g.get());
  } else {
    out.fx = createEffect(c.type);
    if (!out.fx) return false;
    out.fx->prepareHost(st, kRate);
    out.hi = hostedInstance(out.fx.get());
  }
  return out.hi != nullptr;
}

// Renders `blocks` blocks (a tone through an effect; a held C4 on an instrument) and returns the output.
std::vector<float> render(Instance& in, const PluginState& st, int blocks) {
  ProcessContext ctx;
  ctx.sampleRate = kRate;
  ctx.tempo = 120;
  ctx.playing = true;
  std::vector<float> L(kBlock), R(kBlock), out;
  double ph = 0;
  if (in.g) in.g->noteOn(1, 60, 0.8f, 0);
  for (int b = 0; b < blocks; ++b) {
    for (int i = 0; i < kBlock; ++i) {
      const float x = in.g ? 0.0f : static_cast<float>(0.25 * std::sin(ph));
      L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = x;
      ph += 2 * M_PI * 220 / kRate;
    }
    if (in.g) in.g->process(st, ChannelAux{}, ctx, L.data(), R.data(), kBlock);
    else in.fx->processAux(st, ChannelAux{}, ctx, L.data(), R.data(), kBlock);
    for (int i = 0; i < kBlock; ++i) out.push_back(L[static_cast<size_t>(i)]), out.push_back(R[static_cast<size_t>(i)]);
  }
  return out;
}

// Gives the plugin its idle time (and, on macOS, its run loop) for a moment, as the app does between
// frames: some plugins move parameter changes into the settings they save only there.
void settle(Instance& in, int rounds) {
  for (int r = 0; r < rounds; ++r) {
    if (Backend* be = in.hi->backend()) be->idle();
#ifdef __APPLE__
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
#endif
  }
}

// The plugin's parameter values now (plugin units, HostedClass order).
std::vector<float> readBack(Instance& in) {
  UiUpdate u;
  in.hi->pumpUi(u);
  return u.values;
}

double rms(const std::vector<float>& v, size_t from) {
  double s = 0;
  for (size_t i = from; i < v.size(); ++i) s += static_cast<double>(v[i]) * v[i];
  return v.size() > from ? std::sqrt(s / static_cast<double>(v.size() - from)) : 0.0;
}
// Difference of two outputs relative to the source's level (dB; -inf for identical).
double diffDb(const std::vector<float>& a, const std::vector<float>& b) {
  const size_t n = std::min(a.size(), b.size()), from = n / 2;  // the settled second half
  double d = 0;
  for (size_t i = from; i < n; ++i) d += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
  d = std::sqrt(d / static_cast<double>(std::max<size_t>(1, n - from)));
  const double ref = rms(a, from);
  if (d <= 0) return -999;
  return 20 * std::log10(d / std::max(ref, 1e-9));
}

// Saved parameter values compared with the target's by name: {compared, equal}.
std::pair<int, int> compare(const HostedClass& src, const std::vector<float>& sv, const HostedClass& dst, const std::vector<float>& dv,
                            const std::vector<int>& map) {
  int n = 0, eq = 0;
  for (size_t k = 0; k < map.size() && k < sv.size(); ++k) {
    const int t = map[k];
    if (t < 0 || static_cast<size_t>(t) >= dv.size()) continue;
    ++n;
    const double a = src.toNormalized(k, sv[k]), b = dst.toNormalized(static_cast<size_t>(t), dv[static_cast<size_t>(t)]);
    if (std::fabs(a - b) <= 0.01) ++eq;
  }
  return {n, eq};
}

}  // namespace

int crossFormatCheckMain(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: gridline plugin-xformat <from type> <to type>...\n");
    return 2;
  }
  // --project <file>: also writes a project holding the source with its state (insert 1, first slot, or a
  // channel for instruments) and an automation clip on its second parameter.
  std::string projectOut;
  std::vector<std::string> targets;
  for (int i = 3; i < argc; ++i) {
    if (std::string(argv[i]) == "--project" && i + 1 < argc) projectOut = argv[++i];
    else targets.push_back(argv[i]);
  }
  auto src = hostedClass(argv[2]);
  if (!src) {
    std::fprintf(stderr, "not registered: %s (run scan-plugins)\n", argv[2]);
    return 2;
  }
  // 1. The source with test values: a spread over every parameter (choices on a middle step).
  PluginState st = defaultPluginState(src->type);
  for (size_t k = 0; k < src->params.size() && k < st.params.size(); ++k) {
    const double n = 0.2 + 0.6 * std::fmod(static_cast<double>(k) * 0.381966, 1.0);
    st.params[k] = static_cast<float>(src->fromNormalized(k, n));
  }
  Instance a;
  if (!make(*src, st, a)) {
    std::fprintf(stderr, "cannot load %s: %s\n", src->type.c_str(), lastError().c_str());
    return 2;
  }
  render(a, st, 8);  // the values reach the processor
  readBack(a);
  // Also through the plugin's controller, as its own editor would set them (some plugins only keep
  // what arrives there in their state).
  if (Backend* be = a.hi->backend()) {
    for (size_t k = 0; k < src->params.size() && k < st.params.size(); ++k) be->setParamNormalized(src->params[k].id, src->toNormalized(k, st.params[k]));
    render(a, st, 8);
    int live = 0;
    for (size_t k = 0; k < src->params.size() && k < st.params.size(); ++k)
      live += std::fabs(be->paramNormalized(src->params[k].id) - src->toNormalized(k, st.params[k])) <= 0.01;
    std::printf("        test values: %d of %zu set in the source before its state was captured\n", live, src->params.size());
  }
  settle(a, 25);
  render(a, st, 2);
  const std::vector<float> live = readBack(a);
  std::string data;
  if (!a.hi->captureState(data)) std::printf("note: the source gave no state\n");
  PluginState saved = st;
  saved.data = data;
  Instance a2;  // what the project holds, reloaded
  if (!make(*src, saved, a2)) return 2;
  render(a2, saved, 2);  // the saved state is applied when the plugin first runs
  std::vector<float> truth = readBack(a2);
  const PluginState def = defaultPluginState(src->type);
  auto awayFromDefaults = [&](const std::vector<float>& v) {
    int n = 0;
    for (size_t k = 0; k < v.size() && k < def.params.size(); ++k) n += std::fabs(v[k] - def.params[k]) > 1e-3f;
    return n;
  };
  // A plugin whose own state does not keep values set by the host reloads at its defaults; the parameter
  // path is then compared against the live source instead, so it is still tested with real values.
  const bool useLive = awayFromDefaults(truth) == 0 && awayFromDefaults(live) > 0;
  Instance& ref = useLive ? a : a2;
  if (useLive) truth = live;
  saved.params = truth;
  const std::vector<float> srcOut = render(ref, saved, 40);
  std::printf("source  %s (%s) \"%s\" by \"%s\": %zu parameters, state %zu bytes (%s)\n", src->type.c_str(), src->formatLabel(), src->name.c_str(),
              src->vendor.c_str(), src->params.size(), data.size(), data.substr(0, data.find(':')).c_str());
  {
    const int changed = awayFromDefaults(useLive ? readBack(a2) : truth);
    if (useLive) std::printf("        note: the source's saved state does not keep host-set values; comparing with the live source\n");
    std::printf("        test values: %d of %zu parameters away from their defaults after the reload (", changed, truth.size());
    for (size_t k = 0; k < truth.size() && k < 4; ++k) std::printf("%s%s %.3f", k ? ", " : "", src->params[k].title.c_str(), truth[k]);
    std::printf("%s)\n", truth.size() > 4 ? ", ..." : "");
  }
  if (!projectOut.empty()) {
    Song song;
    ensureMixer(song);
    ObjectId owner = kNoId;
    uint32_t base = 0;
    if (src->instrument) {
      Channel c;
      c.id = song.nextId++;
      c.name = src->name;
      c.plugin = saved;
      song.channels.push_back(c);
      owner = c.id;
      base = kChannelPluginBase;
    } else {
      song.mixer[1].slots[0].plugin = saved;
      owner = song.mixer[1].id;
      base = kSlotBase;
    }
    if (saved.params.size() > 1) {
      AutomationClip a;
      a.id = song.nextId++;
      a.name = src->params[1].title;
      a.owner = owner;
      a.index = base + 1;
      AutoPoint p0, p1;
      p0.value = 0.2f;
      p1.pos = 960, p1.value = 0.8f;
      a.points = {p0, p1};
      song.automations.push_back(a);
    }
    std::string err;
    std::printf("project %s: %s\n", projectOut.c_str(), saveProject(projectOut, song, &err) ? "written" : err.c_str());
  }
  int failed = 0;
  for (const std::string& tt : targets) {
    const char* tname = tt.c_str();
    auto dst = hostedClass(tname);
    if (!dst) {
      std::printf("\ntarget  %s: not registered\n", tname);
      ++failed;
      continue;
    }
    const ident::Identity id = ident::identityOf(*src);
    std::vector<ident::Candidate> ranked = ident::rank(id, {dst}, {}, nullptr);
    const ident::Confidence conf = ranked.empty() ? ident::Confidence::None : ranked[0].confidence;
    std::printf("\ntarget  %s (%s) \"%s\" by \"%s\": %zu parameters; match %s", dst->type.c_str(), dst->formatLabel(), dst->name.c_str(),
                dst->vendor.c_str(), dst->params.size(), ident::confidenceName(conf));
    if (!ranked.empty())
      for (const auto& r : ranked[0].reasons) std::printf("; %s", r.c_str());
    std::printf("\n");
    xformat::Conversion cv;
    if (!xformat::convert(saved, id, *dst, conf, cv)) {
      std::printf("        conversion FAILED\n");
      ++failed;
      continue;
    }
    std::printf("        parameters by name: %d of %zu; state: %s%s%s\n", cv.note.matched, saved.params.size(), cv.note.via.c_str(),
                cv.note.how.empty() ? "" : ", ", cv.note.how.c_str());
    bool anyOk = false;
    for (int pass = 0; pass < 2; ++pass) {
      PluginState ts = cv.state;
      if (pass == 1) ts.data.clear();  // parameters only
      else if (ts.data.empty()) continue;
      Json ij;
      if (jsonParse(ts.ident, ij) && ij.is(Json::Type::Object)) {
        ij.object["load"] = xformat::toJson(cv.note);
        ts.ident = jsonCompact(ij);
      }
      xformat::takeLoadChecks();
      Instance b;
      if (!make(*dst, ts, b)) {
        std::printf("        cannot load: %s\n", lastError().c_str());
        break;
      }
      for (const auto& l : xformat::takeLoadChecks()) std::printf("        load check: %s\n", l.c_str());
      render(b, ts, 2);
      const std::vector<float> vals = readBack(b);
      const auto [n, eq] = compare(*src, truth, *dst, vals, cv.indexMap);
      ts.params = vals;
      const std::vector<float> out = render(b, ts, 40);
      const double d = diffDb(srcOut, out);
      std::printf("        %s: %d of %d parameters equal the source's; output difference %s\n", pass == 0 ? "with the moved state" : "parameters only",
                  eq, n, d <= -998 ? "none (identical)" : (std::to_string(static_cast<int>(std::lround(d))) + " dB below the signal").c_str());
      anyOk = anyOk || (n > 0 && eq * 10 >= n * 9);
    }
    if (!anyOk) ++failed;
  }
  std::fflush(stdout);
  std::_Exit(failed ? 1 : 0);  // skip plugin unload / static destructors
}

}  // namespace gl::host
