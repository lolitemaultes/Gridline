// Gridline — J-4 / J-53 tests: FL preset-player (FLEX) channels. Gridline keeps
// the FLEX state (pack, preset, the 45 controls) exactly for the FL export and
// plays a Gridline stand-in ("gl.flex") chosen by category. Checks: the
// classifier, every stand-in template (sound, level, clicks), the drum kit
// map, FLEX controls reaching the stand-in, state preservation through FL
// export (stand-in edits keep the FLEX bytes; FLEX control edits change
// exactly those floats), the project JSON round trip and the migration of
// pre-J-53 states, conversion to a plain instrument, and the shipped corpus.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/FlexStandin.h"
#include "../../src/core/Json.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Presets.h"
#include "../../src/core/Project.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/core/flp/FlpExport.h"
#include "../../src/core/flp/FlpOrigin.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;

namespace {

struct Render {
  std::vector<float> L, R;
  bool finite = true;
  double peak = 0;
};

// Plays `notes` (pitch, start s, length s) on a fresh instance of the state.
Render render(const PluginState& st, const std::vector<std::array<double, 3>>& notes, double seconds, int block = 512) {
  Render r;
  auto g = gl::createGenerator(st.type);
  if (!g) return r;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  ctx.playing = true;
  const int total = static_cast<int>(seconds * ctx.sampleRate);
  r.L.assign(static_cast<size_t>(total), 0.f), r.R.assign(static_cast<size_t>(total), 0.f);
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    for (size_t k = 0; k < notes.size(); ++k) {
      const int on = static_cast<int>(notes[k][1] * ctx.sampleRate), off = static_cast<int>((notes[k][1] + notes[k][2]) * ctx.sampleRate);
      if (on >= pos && on < pos + n) g->noteOn(static_cast<uint32_t>(k + 1), notes[k][0], 0.78f, on - pos);
      if (off >= pos && off < pos + n) g->noteOff(static_cast<uint32_t>(k + 1), off - pos);
    }
    g->process(st, aux, ctx, r.L.data() + pos, r.R.data() + pos, n);
  }
  for (size_t i = 0; i < r.L.size(); ++i) {
    if (!std::isfinite(r.L[i]) || !std::isfinite(r.R[i])) r.finite = false;
    else r.peak = std::max({r.peak, static_cast<double>(std::fabs(r.L[i])), static_cast<double>(std::fabs(r.R[i]))});
  }
  return r;
}

// Loudness (BS.1770 K-weighting, 400 ms blocks, absolute + relative gates).
double lufs(const Render& r, double fs = 44100) {
  auto biquad = [](double b0, double b1, double b2, double a0, double a1, double a2) { return std::array<double, 5>{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0}; };
  const double pi = 3.14159265358979323846;
  double w = 2 * pi * 1681.974450955533 / fs, A = std::pow(10.0, 3.999843853973347 / 40.0), al = std::sin(w) / (2 * 0.7071752369554196);
  const auto shelf = biquad(A * ((A + 1) + (A - 1) * std::cos(w) + 2 * std::sqrt(A) * al), -2 * A * ((A - 1) + (A + 1) * std::cos(w)),
                            A * ((A + 1) + (A - 1) * std::cos(w) - 2 * std::sqrt(A) * al), (A + 1) - (A - 1) * std::cos(w) + 2 * std::sqrt(A) * al,
                            2 * ((A - 1) - (A + 1) * std::cos(w)), (A + 1) - (A - 1) * std::cos(w) - 2 * std::sqrt(A) * al);
  w = 2 * pi * 38.13547087602444 / fs, al = std::sin(w) / (2 * 0.5003270373238773);
  const auto hp = biquad((1 + std::cos(w)) / 2, -(1 + std::cos(w)), (1 + std::cos(w)) / 2, 1 + al, -2 * std::cos(w), 1 - al);
  std::vector<double> power(r.L.size(), 0.0);
  for (const auto* ch : {&r.L, &r.R}) {
    double s1[2] = {}, s2[2] = {};
    for (size_t i = 0; i < ch->size(); ++i) {
      double x = (*ch)[i];
      double y = shelf[0] * x + s1[0];
      s1[0] = shelf[1] * x - shelf[3] * y + s2[0], s2[0] = shelf[2] * x - shelf[4] * y;
      x = y;
      y = hp[0] * x + s1[1];
      s1[1] = hp[1] * x - hp[3] * y + s2[1], s2[1] = hp[2] * x - hp[4] * y;
      power[i] += y * y;
    }
  }
  const size_t blk = static_cast<size_t>(0.4 * fs), hop = static_cast<size_t>(0.1 * fs);
  std::vector<double> z;
  for (size_t s = 0; s + blk <= power.size(); s += hop) {
    double sum = 0;
    for (size_t i = s; i < s + blk; ++i) sum += power[i];
    z.push_back(sum / static_cast<double>(blk));
  }
  auto gated = [&](double thr) {
    double sum = 0;
    int n = 0;
    for (double v : z)
      if (v > 0 && -0.691 + 10 * std::log10(v) > thr) sum += v, ++n;
    return n ? -0.691 + 10 * std::log10(sum / n) : -120.0;
  };
  const double l0 = gated(-70);
  return gated(l0 - 10);
}

// Clicks: the 1 ms RMS envelope never falls by more than `db` from one
// millisecond to the next while the sound is above -50 dB of its peak (an
// abrupt cut), and the sound ends quietly.
bool clickFree(const Render& r, double db, std::string* why) {
  const size_t ms = 44;
  std::vector<double> env;
  for (size_t s = 0; s + ms <= r.L.size(); s += ms) {
    double sum = 0;
    for (size_t i = s; i < s + ms; ++i) sum += 0.5 * (r.L[i] * r.L[i] + r.R[i] * r.R[i]);
    env.push_back(std::sqrt(sum / ms));
  }
  double top = 0;
  for (double e : env) top = std::max(top, e);
  if (top <= 0) return true;
  for (size_t k = 1; k < env.size(); ++k) {
    if (env[k - 1] < top * 0.003) continue;
    const double drop = 20 * std::log10(std::max(env[k], 1e-12) / env[k - 1]);
    if (drop < -db) {
      if (why) *why = "drop of " + std::to_string(drop) + " dB at " + std::to_string(k) + " ms";
      return false;
    }
  }
  return env.back() < top * 0.05;
}

PluginState flexState(const std::string& pack, const std::string& preset, int cat = flex::CatAuto) {
  float c[flex::kControls];
  for (float& v : c) v = 0.5f;
  for (int k = 0; k < 8; ++k) c[flex::Macro1 + k] = 0;
  c[flex::OutVol] = 1, c[flex::PostCut] = 1, c[flex::DlyOn] = 0, c[flex::RevOn] = 0, c[flex::PostOn] = 0, c[flex::LimOn] = 0;
  PluginState s = flex::makeState(pack, preset, c);
  s.params[flex::PTemplate] = static_cast<float>(cat);
  return s;
}

// A phrase for each kind of template: chords for sustained sounds, a line for the rest.
std::vector<std::array<double, 3>> phrase(int cat) {
  std::vector<std::array<double, 3>> n;
  const bool chords = cat == flex::Pad || cat == flex::Strings || cat == flex::Choir || cat == flex::Organ || cat == flex::Keys ||
                      cat == flex::EPiano || cat == flex::Brass || cat == flex::Stab || cat == flex::Neutral;
  const bool low = cat == flex::Bass || cat == flex::SubBass;
  for (int b = 0; b < 4; ++b) {
    const double t = b * 1.0;
    if (cat == flex::Drums) {
      n.push_back({36, t, 0.1}), n.push_back({42, t + 0.25, 0.1}), n.push_back({38, t + 0.5, 0.1}), n.push_back({42, t + 0.75, 0.1});
    } else if (chords) {
      for (int k : {60, 64, 67}) n.push_back({static_cast<double>(k + (b % 2 ? 5 : 0)), t, 0.9});
    } else {
      for (int s = 0; s < 4; ++s) n.push_back({(low ? 36.0 : 64.0) + (s * 3 + b) % 7, t + s * 0.25, cat == flex::Fx ? 0.9 : 0.2});
    }
  }
  return n;
}

}  // namespace

int main() {
  // ---------------------------------------------------------------- classifier
  {
    struct C {
      const char* pack;
      const char* preset;
      int cat;
    };
    const C table[] = {
        {"", "Deep Sub Bass", flex::SubBass},      {"", "808 Clean", flex::SubBass},          {"Some 808s", "Astro", flex::SubBass},
        {"", "BAS Rumble", flex::Bass},            {"", "Finger Bass", flex::Bass},           {"", "Basse Funk", flex::Bass},
        {"", "Bajo Electrico", flex::Bass},        {"", "Acid Line", flex::Bass},             {"", "LED Shine", flex::Lead},
        {"", "Dystopian Lead", flex::Lead},        {"", "Solo Flute", flex::Lead},            {"", "Bassoon Section", flex::Lead},
        {"", "PAD Warmth", flex::Pad},             {"", "Evolving Drone", flex::Pad},         {"", "Scatter Clouds", flex::Pad},
        {"", "Piano Dark", flex::Keys},            {"", "Grand Piano", flex::Keys},           {"", "Klavier Hell", flex::Keys},
        {"", "KEY Morning", flex::Keys},           {"", "Electric Piano 1", flex::EPiano},    {"", "Rhodes Mk1", flex::EPiano},
        {"", "E Piano Soft", flex::EPiano},        {"", "Piano Electrique", flex::EPiano},    {"Mobile Synth Pluck", "Synpluck Dual", flex::Pluck},
        {"", "PLK Navid", flex::Pluck},            {"", "Harp", flex::Pluck},                 {"", "Pizzicato", flex::Pluck},
        {"", "Tinke Bell", flex::Bell},            {"", "Glockenspiel", flex::Bell},          {"", "Marimba Wood", flex::Bell},
        {"", "Cloche Douce", flex::Bell},          {"", "String Ensemble", flex::Strings},    {"", "Cello Solo", flex::Strings},
        {"", "Streicher", flex::Strings},          {"", "Cuerdas Lentas", flex::Strings},     {"", "Brass Section 1", flex::Brass},
        {"", "Trumpet Fall", flex::Brass},         {"", "Alto Sax", flex::Brass},             {"", "Blechblaeser", flex::Brass},
        {"", "Choir Aahs", flex::Choir},           {"", "VOX Chop", flex::Choir},             {"", "Coro Angelico", flex::Choir},
        {"", "Chor Gross", flex::Choir},           {"", "STB House", flex::Stab},             {"", "Chord Memory", flex::Stab},
        {"", "Orchestra Hit", flex::Stab},         {"", "Arp Bubbles", flex::Arp},            {"", "Sequence 3", flex::Arp},
        {"", "Nylon Guitar", flex::Guitar},        {"", "Gitarre Clean", flex::Guitar},       {"", "Chitarra", flex::Guitar},
        {"", "Organ Drawbar", flex::Organ},        {"", "Orgue Eglise", flex::Organ},         {"", "Hammond B3", flex::Organ},
        {"", "SFX Crasha", flex::Fx},              {"", "Riser Long", flex::Fx},              {"", "White Noise Sweep", flex::Fx},
        {"", "Drum Kit 909", flex::Drums},         {"", "Cowbell", flex::Drums},              {"", "Agogo", flex::Drums},
        {"", "Batterie Rock", flex::Drums},        {"", "Percussion Loop", flex::Drums},      {"", "Snare Tight", flex::Drums},
        {"", "Just Strollin", flex::Neutral},      {"", "", flex::Neutral},                   {"Library X", "Capacitation", flex::Neutral},
    };
    int ok = 0, n = 0;
    for (const C& c : table) {
      ++n;
      std::string why;
      const int got = flex::classify(c.pack, c.preset, &why);
      if (got == c.cat) ++ok;
      else std::printf("# classify \"%s\" / \"%s\": %s (want %s, word \"%s\")\n", c.pack, c.preset, flex::catName(got), flex::catName(c.cat), why.c_str());
    }
    std::printf("# classifier: %d / %d names\n", ok, n);
    expect(ok == n, "classifier: every name in the table gets its category");
    expect(flex::drumPiece("Cowbell") == 10 && flex::drumPiece("Drum Kit") == -1 && flex::drumPiece("Agogo") == 10, "single percussion pieces");
  }

  // ------------------------------------------------------------- the type
  {
    const PluginInfo* info = findPlugin(flex::kType);
    expect(info && info->params.size() == static_cast<size_t>(flex::PCount) && info->presets.size() >= 2 && info->presets.size() <= 6,
           "gl.flex: catalogue entry (49 parameters, factory presets)");
    PluginState shortSt;
    shortSt.type = flex::kType;
    shortSt.params = {0.2f, 0.3f};
    normalizePluginState(shortSt);
    expect(shortSt.params.size() == static_cast<size_t>(flex::PCount) && shortSt.params[0] == 0.2f && shortSt.params[flex::PTemplate] == 0,
           "gl.flex: short parameter vectors normalise");
  }

  // ------------------------------------------------- every template renders
  {
    int good = 0;
    for (int cat = 1; cat < flex::CatCount; ++cat) {
      const PluginState st = flexState("Pack", "Test", cat);
      const Render r = render(st, phrase(cat), 6.0);
      const double l = lufs(r);
      std::string why;
      const bool clicks = clickFree(r, cat == flex::Drums || cat == flex::Guitar ? 60 : 40, &why);
      const bool ok = r.finite && r.peak > 0.01 && r.peak < 1.2 && l > -20 && l < -10 && clicks;
      good += ok;
      std::printf("# template %-15s %-10s peak %.3f  %.1f LUFS%s%s\n", flex::catName(cat), flex::engineName(flex::engineOf(cat)), r.peak, l,
                  clicks ? "" : "  click: ", why.c_str());
      expect(ok, (std::string("template renders cleanly at an instrument level: ") + flex::catName(cat)).c_str());
    }
    std::printf("# templates: %d / %d clean\n", good, flex::CatCount - 1);
    // Block-size invariance (events and chunks inside the wrapper).
    const PluginState st = flexState("Pack", "Pad", flex::Lead);
    const Render a = render(st, phrase(flex::Lead), 2.0, 64), b = render(st, phrase(flex::Lead), 2.0, 2048 + 512);
    double diff = 0;
    for (size_t i = 0; i < a.L.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a.L[i] - b.L[i])));
    expect(diff < 1e-3, "stand-in output does not depend on the block size");
  }

  // ------------------------------------------------------ FLEX controls apply
  {
    PluginState dark = flexState("Pack", "Lead"), bright = dark, quiet = dark, up = dark;
    dark.params[flex::Cutoff] = 0.2f, bright.params[flex::Cutoff] = 0.8f;
    auto centroid = [](const Render& r) {
      double num = 0, den = 0;
      for (size_t i = 1; i < r.L.size(); ++i) {
        const double d = r.L[i] - r.L[i - 1];
        num += d * d, den += static_cast<double>(r.L[i]) * r.L[i];  // high-frequency share (~ f squared)
      }
      return den > 0 ? num / den : 0.0;
    };
    const auto notes = phrase(flex::Lead);
    const double cd = centroid(render(dark, notes, 2)), cb = centroid(render(bright, notes, 2));
    std::printf("# brightness proxy: cutoff 0.2 %.4f, 0.8 %.4f\n", cd, cb);
    expect(cb > cd * 1.5, "FLEX cutoff control moves the stand-in's brightness");
    quiet.params[flex::OutVol] = 0.25f;
    expect(render(quiet, notes, 2).peak < render(bright, notes, 2).peak, "FLEX output volume lowers the stand-in");
    PluginState mac = dark;
    mac.params[flex::Macro1] = 1;  // macro 1: tone
    expect(centroid(render(mac, notes, 2)) > cd * 1.3, "macro 1 (tone) brightens the stand-in");
    up.params[flex::Pitch] = 0.5f + 12.0f / 48.0f;  // +12 st
    const std::vector<std::array<double, 3>> one = {{{60, 0, 1}}};
    auto zeroCross = [](const Render& r) {
      int z = 0;
      for (size_t i = 22050; i < 44100; ++i) z += (r.L[i - 1] < 0) != (r.L[i] < 0);
      return z;
    };
    PluginState organ = flexState("Pack", "x", flex::Organ), organUp = organ;
    organUp.params[flex::Pitch] = up.params[flex::Pitch];
    const int z0 = zeroCross(render(organ, one, 1.2)), z1 = zeroCross(render(organUp, one, 1.2));
    expect(z1 > z0 * 1.6, "FLEX pitch transposes the stand-in");
    PluginState wet = flexState("Pack", "x", flex::Pluck);
    wet.params[flex::RevOn] = 1, wet.params[flex::RevMix] = 1, wet.params[flex::RevDecay] = 0.8f;
    const std::vector<std::array<double, 3>> stab = {{{60, 0, 0.1}}};
    const Render dry = render(flexState("Pack", "x", flex::Pluck), stab, 3), rv = render(wet, stab, 3);
    double tailDry = 0, tailWet = 0;
    for (size_t i = 88200; i < 110250; ++i) tailDry += std::fabs(dry.L[i]), tailWet += std::fabs(rv.L[i]);
    expect(tailWet > tailDry * 10 + 1e-3, "FLEX reverb switch adds the stand-in reverb");
  }

  // -------------------------------------------------------------- drum kit
  {
    expect(flex::drumNote(36) != flex::drumNote(38) && flex::drumNote(38) != flex::drumNote(42) && flex::drumNote(42) != flex::drumNote(46) &&
               flex::drumNote(36) == flex::drumNote(48 - 12),
           "kit map: kick, snare, closed and open hat on their own pads; octaves wrap");
    const PluginState kit = flexState("Pack", "Drum Kit");
    expect(flex::categoryOf(kit) == flex::Drums, "drum kit preset classifies as drums");
    auto bright = [](const Render& r) {
      double num = 0, den = 0;
      for (size_t i = 1; i < r.L.size(); ++i) num += std::fabs(r.L[i] - r.L[i - 1]), den += std::fabs(r.L[i]);
      return den > 0 ? num / den : 0.0;
    };
    const Render kick = render(kit, {{{36, 0, 0.1}}}, 0.5), hat = render(kit, {{{42, 0, 0.1}}}, 0.5);
    expect(kick.peak > 0.01 && hat.peak > 0.01 && bright(hat) > bright(kick) * 3, "kit: key 36 plays a kick, key 42 a hat");
  }

  // ------------------------------------------------- convert to an instrument
  {
    for (int cat : {flex::Pad, flex::Keys, flex::Bell, flex::Guitar, flex::Drums}) {
      const PluginState st = flexState("Pack", "x", cat);
      const PluginState conv = flex::standInState(st);
      expect(conv.type == flex::engineType(flex::engineOf(cat)) && conv.data.empty() && render(conv, phrase(cat), 3).peak > 0.01,
             (std::string("convert to Gridline instrument: ") + flex::catName(cat)).c_str());
    }
  }

  // -------------------------------------------- project JSON and migration
  {
    Song s;
    ensureMixer(s);
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "Flex";
    ch.plugin = flexState("My Pack", "Warm Pad", flex::Strings);
    ch.plugin.params[flex::Macro4] = 0.3125f;
    ch.plugin.params[flex::PTone] = -0.25f;
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
    Json j;
    Song back;
    const bool ok = jsonParse(projectToText(s), j) && songFromJson(j, back);
    expect(ok && back.channels.size() == 1 && back.channels[0].plugin == ch.plugin, "project JSON keeps the FLEX state and the stand-in settings");

    // A project saved before J-53: FLEX played on Analog with "flex 1" data.
    Json old;
    jsonParse(projectToText(s), old);
    Json& pj = old.object["channels"].array[0].object["plugin"];
    std::string v = "v";
    for (int k = 0; k < flex::kControls; ++k) v += " " + std::to_string(k == flex::Macro2 ? 0.75 : k == flex::Pitch ? 0.25 : 0.5);
    pj.object["type"] = Json::makeString("gl.analog");
    pj.object["data"] = Json::makeString("flex 1\npack Old Pack\npreset Old Bass\n" + v + "\n");
    pj.object["params"] = Json::makeArray();
    Song mig;
    expect(songFromJson(old, mig) && mig.channels.size() == 1 && mig.channels[0].plugin.type == flex::kType &&
               std::fabs(mig.channels[0].plugin.params[flex::Macro2] - 0.75f) < 1e-6 && std::fabs(mig.channels[0].plugin.params[flex::Pitch] - 0.25f) < 1e-6 &&
               flex::identity(mig.channels[0].plugin.data).preset == "Old Bass" && flex::categoryOf(mig.channels[0].plugin) == flex::Bass,
           "migration: a pre-stand-in FLEX channel loads as gl.flex with its kept controls");
    // Its FL origin was recorded under the old Gridline type.
    flp::EventList ev;
    ev.push_back(flp::RawEvent{201, flp::utf16z("FLEX")});
    ev.push_back(flp::RawEvent{flp::kGlType, flp::Bytes{'g', 'l', '.', 'a', 'n', 'a', 'l', 'o', 'g'}});
    expect(flp::originTypeMatches(ev, flex::kType) && !flp::originTypeMatches(ev, "gl.keys"), "migration: the old FL origin stays usable");
  }

  // ------------------------------------------- shipped corpus (FL installed)
  if (!testfl::flInstalled()) {
    std::printf("# FL Studio not installed: FLEX corpus checks skipped\n");
    return finish();
  }
  {
    auto all = testfl::decodeProjects("FLEX");
    for (const char* rel : {"/Data/System/FLEX.fst", "/Data/Patches/Plugin database/Generators/Synth classic/FLEX.fst"}) {
      testfl::Decoded d;
      d.where = rel;
      flp::PresetResult r;
      d.ok = flp::importFlPreset(testfl::flData() + rel, r, testfl::flData()) && r.kind == flp::PresetResult::Plugin;
      d.state = r.state;
      all.push_back(d);
    }
    int total = 0, decoded = 0, kept = 0, sounding = 0;
    int perCat[flex::CatCount] = {};
    for (const auto& d : all) {
      ++total;
      if (!d.ok) {
        std::printf("# not decoded: %s\n", d.where.c_str());
        continue;
      }
      ++decoded;
      testfl::maybeDump(d);
      kept += d.state.type == flex::kType && flex::identity(d.state.data).ok;
      const int cat = flex::categoryOf(d.state);
      perCat[cat]++;
      if (std::getenv("GL_FLEX_LIST")) {  // debug: one line per shipped state
        const flex::Identity fid = flex::identity(d.state.data);
        std::string why;
        flex::classify(fid.pack, fid.preset, &why);
        std::printf("#   %-48.48s %-24s %-22s -> %s (%s)\n", d.where.c_str(), fid.pack.c_str(), fid.preset.c_str(), flex::catName(cat), why.c_str());
      }
      const Render r = render(d.state, phrase(cat), 2.5);
      sounding += r.finite && r.peak > 1e-3;
      if (!(r.finite && r.peak > 1e-3)) std::printf("# silent: %s (%g)\n", d.where.c_str(), r.peak);
    }
    std::printf("# FLEX: %d states, %d decoded, %d gl.flex with pack / preset, %d sounding\n#", total, decoded, kept, sounding);
    for (int c = 1; c < flex::CatCount; ++c)
      if (perCat[c]) std::printf(" %s %d,", flex::catName(c), perCat[c]);
    std::printf("\n");
    expect(total >= 50 && decoded == total, "every shipped FLEX state decodes");
    expect(kept == decoded, "the FLEX selection and controls are kept (gl.flex)");
    expect(sounding == decoded, "every FLEX stand-in sounds");
  }

  // ---------------- FL preset library import: FLEX presets as stand-in presets
  {
    namespace fs = std::filesystem;
    // A private "FL data" folder holding one shipped FLEX preset, converted
    // into a private library (never the user's real one).
    const fs::path tmp = fs::temp_directory_path() / ("gl_flex_presets_" + std::to_string(std::rand()));
    const fs::path fl = tmp / "fl", lib = tmp / "lib";
    std::error_code ec;
    fs::create_directories(fl / "Data/Patches/Plugin presets/Generators/FLEX", ec);
    fs::copy_file(fs::path(testfl::flData()) / "Data/System/FLEX.fst", fl / "Data/Patches/Plugin presets/Generators/FLEX/Lead test.fst", ec);
    const flp::PresetLibraryReport rep = flp::importPresetLibrary(fl.string(), lib.string());
    std::vector<PresetEntry> got = listPresets(lib.string(), flex::kType);
    std::string text;
    if (!got.empty()) {
      FILE* fp = std::fopen(got[0].file.c_str(), "rb");
      if (fp) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
        std::fclose(fp);
      }
    }
    PluginState loaded;
    const bool ok = !got.empty() && loadPreset(got[0].file, flex::kType, loaded);
    expect(rep.standins == 1 && got.size() == 1 && ok && flex::identity(loaded.data).ok && text.find("stand-in") != std::string::npos &&
               text.find("\"approximate\"") != std::string::npos,
           "import-fl-presets: a FLEX preset becomes a flagged stand-in preset");
    fs::remove_all(tmp, ec);
  }

  // --------------------------- state preservation through the FL export
  {
    namespace fs = std::filesystem;
    const fs::path src = fs::path(testfl::flData()) / "Data/Demo projects/Demo songs/Mehran Abbasi - With You.flp";
    Song song;
    flp::Report rep;
    std::string err;
    if (!fs::exists(src) || !importFlp(src.string(), song, rep, &err, testfl::flData())) {
      std::printf("# FLEX demo project not found: export checks skipped\n");
      return finish();
    }
    auto flexStates = [](const flp::Bytes& flpBytes) {
      std::vector<flp::Bytes> out;
      flp::File f;
      if (!flp::readBytes(flpBytes, f, nullptr)) return out;
      bool inFlex = false;
      for (const auto& e : f.events) {
        if (e.id == 201) inFlex = e.text() == "FLEX";
        if (e.id == 213 && inFlex) out.emplace_back(e.data, e.data + e.size), inFlex = false;
      }
      return out;
    };
    std::vector<uint8_t> orig;
    {
      FILE* fp = std::fopen(src.string().c_str(), "rb");
      if (fp) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) orig.insert(orig.end(), buf, buf + n);
        std::fclose(fp);
      }
    }
    const std::vector<flp::Bytes> before = flexStates(orig);
    std::vector<size_t> flexChannels;
    for (size_t i = 0; i < song.channels.size(); ++i)
      if (song.channels[i].plugin.type == flex::kType) flexChannels.push_back(i);
    expect(before.size() == flexChannels.size() && before.size() >= 10, "demo project: every FLEX channel imports as gl.flex");
    // Stand-in-only edits on every FLEX channel: the FLEX states export byte-identical.
    Song edited = song;
    for (size_t i : flexChannels) {
      auto& p = edited.channels[i].plugin.params;
      p[flex::PTemplate] = flex::Organ, p[flex::PTone] = 0.7f, p[flex::PLevel] = -6, p[flex::POctave] = 1;
    }
    flp::ExportOptions eo;
    eo.factoryData = testfl::flData();
    flp::Bytes out;
    flp::ExportReport xr;
    const bool exp1 = flp::exportFlp(edited, out, xr, eo);
    const std::vector<flp::Bytes> after = flexStates(out);
    bool same = exp1 && after.size() == before.size();
    for (size_t k = 0; same && k < after.size(); ++k) same = after[k] == before[k];
    expect(same, "stand-in-only edits: every FLEX state exports byte-identical");
    // Encoder mode (no origin): the FLEX controls and names are rebuilt exactly.
    flp::ExportOptions en = eo;
    en.useOrigin = false;
    flp::Bytes out2;
    flp::ExportReport xr2;
    bool nativeOk = flp::exportFlp(song, out2, xr2, en);
    for (const auto& p : xr2.plugins)
      if (p.glType == flex::kType) nativeOk = nativeOk && p.quality == "exact";
    expect(nativeOk, "encoder mode: every FLEX state is written exactly");
    // A macro edit: exactly that float changes in exactly that state.
    Song mac = song;
    const size_t c0 = flexChannels.front();
    mac.channels[c0].plugin.params[flex::Macro3] = 0.4375f;
    mac.channels[c0].plugin.params[flex::Cutoff] = 0.625f;
    flp::Bytes out3;
    flp::ExportReport xr3;
    const bool exp3 = flp::exportFlp(mac, out3, xr3, eo);
    const std::vector<flp::Bytes> after3 = flexStates(out3);
    bool onlyThat = exp3 && after3.size() == before.size();
    int changedStates = 0;
    for (size_t k = 0; onlyThat && k < after3.size(); ++k) {
      if (after3[k] == before[k]) continue;
      ++changedStates;
      if (after3[k].size() != before[k].size()) {
        onlyThat = false;
        break;
      }
      std::vector<size_t> diff;
      for (size_t i = 0; i < after3[k].size(); ++i)
        if (after3[k][i] != before[k][i]) diff.push_back(i);
      // The two floats: find control 0 by the macro value written.
      float m3 = 0, cut = 0;
      for (size_t i = 0; i + 4 <= after3[k].size(); ++i) {
        float x;
        std::memcpy(&x, after3[k].data() + i, 4);
        if (x == 0.4375f) m3 = x;
        if (x == 0.625f) cut = x;
      }
      onlyThat = !diff.empty() && diff.back() - diff.front() < 4 * (flex::Cutoff - flex::Macro3 + 1) && m3 == 0.4375f && cut == 0.625f;
    }
    expect(onlyThat && changedStates == 1, "FLEX control edits change exactly those floats of that state");
  }
  return finish();
}
