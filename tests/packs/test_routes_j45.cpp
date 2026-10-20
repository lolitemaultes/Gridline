// Gridline — J-45: effects exactness tier G. FL route numbering for plugin
// side outputs / inputs (send, band splitter, difference output, sidechain
// source), the controller's LFO tables, the waveshaper's reference engine
// (polyphase oversampling) and the control-panel state writer.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Song.h"
#include "core/flp/Flp.h"
#include "engine/Effects.h"
#include "engine/Program.h"
#include "packs/dynamics/dynamics.h"
#include "packs/modulation/modulation.h"
#include "packs/spacefx/spacefx.h"
#include "packs/spectral/spectral.h"
#include "packs/utility/utility.h"

using namespace gl;
namespace md = gl::packs::modulation;
namespace dy = gl::packs::dynamics;
namespace ut = gl::packs::utility;
namespace sx = gl::packs::spacefx;
namespace sp = gl::packs::spectral;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// Master + 4 inserts: 1 → master and 3; 2 → 3 (sidechain only); 3, 4 → master.
Song routedSong() {
  Song s;
  ensureMixer(s, 4);
  for (auto& t : s.mixer) t.sends.clear();
  auto id = [&](int i) { return s.mixer[static_cast<size_t>(i)].id; };
  s.mixer[1].sends = {{id(3), 1.0f, false, false}, {id(0), 1.0f, false, false}};
  s.mixer[2].sends = {{id(3), 1.0f, true, false}};
  s.mixer[3].sends = {{id(0), 1.0f, false, false}};
  s.mixer[4].sends = {{id(0), 1.0f, false, false}};
  return s;
}

PluginState stateOf(const std::string& type) {
  PluginState p;
  p.type = type;
  normalizePluginState(p);
  return p;
}

}  // namespace

int main() {
  // ------------------------------------------------ route lists and resolution
  {
    const Song s = routedSong();
    expect(flRouteOutputs(s, 1) == std::vector<int>({1, 0, 3}), "outputs: the track, then its destinations ascending");
    expect(flRouteInputs(s, 3) == std::vector<int>({3, 1, 2}), "inputs: the track, then its sources ascending (sidechain-only included)");
    expect(flRouteOutput(s, 1, 0) == -1 && flRouteOutput(s, 1, -1) == -1, "output 0 / -1: none");
    expect(flRouteOutput(s, 1, 1) == 0 && flRouteOutput(s, 1, 2) == 3, "output n: the n-th destination");
    expect(flRouteOutput(s, 1, 7) == 3, "output past the end: the last destination");
    expect(flRouteOutput(s, 0, 2) == -1, "a track without routes: none (itself)");
    expect(flRouteOutput(s, 4, 2) == 0, "a track routed only to the master: every n is the master");
    expect(flRouteInput(s, 3, 1) == 1 && flRouteInput(s, 3, 2) == 2 && flRouteInput(s, 3, 3) == -1, "input n: the n-th source, none past the end");
  }

  // ------------------------------------------------ engine: route-indexed sends resolve per build
  {
    Song s = routedSong();
    PluginState send = stateOf("gl.send");
    send.params[md::SnTarget] = 2;
    send.params[md::SnRoute] = 1;
    s.mixer[1].slots[0].plugin = send;
    PluginState shaper = stateOf("gl.stereomatrix");
    shaper.params[ut::SmDiffTo] = 1;
    s.mixer[1].slots[1].plugin = shaper;
    PluginState split = stateOf("gl.splitter");
    split.params[static_cast<size_t>(sp::SpRoute)] = 1;
    split.params[static_cast<size_t>(sp::splitParam2(0, sp::SxSendTo))] = 2;
    split.params[static_cast<size_t>(sp::splitParam2(1, sp::SxSendTo))] = 0;
    s.mixer[1].slots[2].plugin = split;
    PluginState hall = stateOf("gl.hall");
    hall.params[sx::HvScIn] = 1;
    s.mixer[3].slots[0].plugin = hall;
    PluginState hall2 = stateOf("gl.hall");
    s.mixer[3].slots[1].plugin = hall2;  // default: the track's own sidechain input
    ProgramBuilder b;
    auto prog = b.build(s);
    const ProgMixer& m1 = prog->mixer[1];
    expect(m1.slots[0].sendTo == 3, "send: route 2 of insert 1 is insert 3");
    expect(m1.slots[1].multiSend[0] == 0, "difference output: route 1 of insert 1 is the master");
    expect(m1.slots[2].multiSend[0] == 3 && m1.slots[2].multiSend[1] == -1, "band sends: route index per band, 0 = none");
    expect(prog->mixer[3].slots[0].sideOwn && prog->mixer[3].slots[0].sideSrc == 1 && prog->mixer[3].slots[0].sideIn.size() == 1,
           "sidechain source: input 1 of insert 3 is insert 1, read by the slot");
    expect(!prog->mixer[3].slots[1].sideOwn, "sidechain source -1: the track's own sidechain input");
    // Re-route insert 1: the same stored index now names another track.
    s.mixer[1].sends = {{s.mixer[0].id, 1.0f, false, false}, {s.mixer[4].id, 1.0f, false, false}};
    auto prog2 = b.build(s);
    expect(prog2->mixer[1].slots[0].sendTo == 4, "send: resolved again when the routing changes");
    PluginState plain = stateOf("gl.send");
    plain.params[md::SnTarget] = 2;
    s.mixer[1].slots[0].plugin = plain;
    auto prog3 = b.build(s);
    expect(prog3->mixer[1].slots[0].sendTo == 2, "send: mixer-track numbering unchanged by default");
  }

  // ------------------------------------------------ difference output = input - output
  {
    auto fx = createEffect("gl.stereomatrix");
    PluginState st = stateOf("gl.stereomatrix");
    st.params[ut::SmLL] = 0.5f;  // not unity: something to subtract
    std::vector<float> L(256), R(256), dL(256, 0.f), dR(256, 0.f), inL(256), inR(256);
    for (int i = 0; i < 256; ++i) L[i] = inL[i] = std::sin(0.05f * i) * 0.5f, R[i] = inR[i] = std::cos(0.03f * i) * 0.4f;
    SideIo io;
    io.multiL[0] = dL.data();
    io.multiR[0] = dR.data();
    fx->setSideIo(io);
    ProcessContext ctx;
    ctx.sampleRate = 48000;
    fx->process(st, ctx, L.data(), R.data(), 256);
    bool ok = true;
    for (int i = 0; i < 256; ++i) ok = ok && dL[i] == inL[i] - L[i] && dR[i] == inR[i] - R[i];
    expect(ok, "stereo matrix: the difference output carries input minus output");
  }

  // ------------------------------------------------ fruity send law (route mode): far channel folds in at half weight
  {
    auto fx = createEffect("gl.send");
    PluginState st = stateOf("gl.send");
    st.params[md::SnRoute] = 1;
    st.params[md::SnPan] = 0.5f;
    st.params[md::SnLevel] = 1.0f;
    std::vector<float> L(4, 1.f), R(4, 0.f), sL(4, 0.f), sR(4, 0.f);
    SideIo io;
    io.sendL = sL.data(), io.sendR = sR.data();
    fx->setSideIo(io);
    ProcessContext ctx;
    fx->process(st, ctx, L.data(), R.data(), 4);
    expect(near(sL[0], 0.5, 1e-6) && near(sR[0], 0.25, 1e-6), "send: pan +0.5 keeps 1-p of the left and moves p/2 to the right");
  }

  // ------------------------------------------------ peak controller: reference LFO tables
  {
    auto fx = createEffect("gl.peakctl");
    auto lfoFirst = [&](int shape) {
      auto f = createEffect("gl.peakctl");
      PluginState st = stateOf("gl.peakctl");
      st.params[md::PkModel] = 1;
      st.params[md::LfShape] = static_cast<float>(shape);
      st.params[md::LfRAmount] = 256;  // full LFO amount (×1.0)
      st.params[md::LfRBase] = 0;
      st.params[md::LfRTension] = 0;
      st.params[md::PkRAmount] = 0;
      ProcessContext ctx;
      ctx.sampleRate = 48000, ctx.tempo = 120, ctx.ppq = 96, ctx.playing = false;
      std::vector<float> L(600, 0.f), R(600, 0.f);  // ~2 ticks
      f->process(st, ctx, L.data(), R.data(), 300);
      float out[3] = {0, 0, 0};
      f->controlOutputs(out, 3);
      return static_cast<double>(out[2]);
    };
    // First tick reads entry 0: noise table entry 0 = 0.0829383582 → (v + 1) / 2.
    expect(near(lfoFirst(4), (0.08293835818767548 + 1.0) * 0.5, 1e-6), "peak controller: random reads the reference noise table");
    expect(near(lfoFirst(1), 0.5, 1e-6), "peak controller: triangle starts at the centre, rising");
    expect(near(lfoFirst(2), 1.0, 1e-6), "peak controller: square starts high");
    expect(near(lfoFirst(3), 0.0, 1e-6), "peak controller: saw starts at the bottom");
    (void)fx;
  }

  // ------------------------------------------------ waveshaper reference engine
  {
    PluginState st = stateOf("gl.waveshaper");
    st.data = "WS1|0,0,0,0 1,1,0,0";  // unipolar identity
    st.params[pp::WsClassic] = 1;
    st.params[pp::WsBipolar] = 1;
    st.params[pp::WsPreAmp] = 1, st.params[pp::WsMix] = 1, st.params[pp::WsPost] = 1;
    st.params[pp::WsHq] = 1;
    bool finiteAll = true;
    for (int os = 0; os <= 4; ++os) {
      st.params[pp::WsOversample] = static_cast<float>(os);
      auto fx = createEffect("gl.waveshaper");
      expect(fx->latencyFor(st, 48000) == 0, ("waveshaper: no reported latency at " + std::to_string(1 << os) + "x").c_str());
      std::vector<float> L(4096), R(4096);
      for (int i = 0; i < 4096; ++i) L[i] = R[i] = 0.5f * std::sin(2 * 3.14159265f * 200.f * i / 48000.f);
      ProcessContext ctx;
      ctx.sampleRate = 48000;
      for (int o = 0; o < 4096; o += 256) fx->process(st, ctx, L.data() + o, R.data() + o, 256);
      double pk = 0;
      for (int i = 2048; i < 4096; ++i) {
        finiteAll = finiteAll && std::isfinite(L[i]);
        pk = std::max(pk, static_cast<double>(std::fabs(L[i])));
      }
      expect(near(pk, 0.5, 0.01), ("waveshaper: identity curve passes a low tone at unity, " + std::to_string(1 << os) + "x").c_str());
      std::vector<float> Z(512, 0.f), Z2(512, 0.f);
      auto fz = createEffect("gl.waveshaper");
      fz->process(st, ctx, Z.data(), Z2.data(), 512);
      expect(*std::max_element(Z.begin(), Z.end()) == 0.f, "waveshaper: silence in, silence out");
    }
    expect(finiteAll, "waveshaper: finite output");
  }

  // ------------------------------------------------ transient split: reference engine
  {
    PluginState st = stateOf("gl.transplit");
    st.params[dy::TsModel] = 1;  // stored-control defaults: saturation / diffusion slots
    for (int bs : {64, 512}) {
      auto fx = createEffect("gl.transplit");
      ProcessContext ctx;
      ctx.sampleRate = 48000, ctx.tempo = 120;
      std::vector<float> L(24576), R(24576), tL(24576, 0.f), tR(24576, 0.f), sL(24576, 0.f), sR(24576, 0.f);
      uint32_t seed = 3;
      for (size_t i = 0; i < L.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        L[i] = R[i] = (i % 6000 < 300 ? 0.8f : 0.05f) * ((seed >> 8) / 8388608.0f - 1.0f);
      }
      bool finiteAll = true;
      for (size_t o = 0; o < L.size(); o += static_cast<size_t>(bs)) {
        SideIo io;
        io.multiL[0] = tL.data() + o, io.multiR[0] = tR.data() + o, io.multiL[1] = sL.data() + o, io.multiR[1] = sR.data() + o;
        fx->setSideIo(io);
        fx->process(st, ctx, L.data() + o, R.data() + o, bs);
      }
      double e = 0, et = 0, es = 0;
      for (size_t i = 0; i < L.size(); ++i) {
        finiteAll = finiteAll && std::isfinite(L[i]) && std::isfinite(R[i]);
        e += L[i] * L[i], et += tL[i] * tL[i], es += sL[i] * sL[i];
      }
      expect(finiteAll && e > 0 && et > 0 && es > 0, ("transient split reference: output and both sends, block " + std::to_string(bs)).c_str());
      expect(fx->latencyFor(st, 48000) == 96, "transient split reference: 2 ms look-ahead reported");
    }
  }

  // ------------------------------------------------ hit looper: loops follow hits; block-size invariant
  {
    auto render = [&](int bs) {
      PluginState st = stateOf("gl.hitloop");
      st.params[dy::HlReverse] = 10000;  // every slice reversed
      st.params[dy::HlFixed] = 1;
      auto fx = createEffect("gl.hitloop");
      ProcessContext ctx;
      ctx.sampleRate = 48000, ctx.tempo = 120;
      std::vector<float> L(98304, 0.f), R(98304, 0.f), sA(98304, 0.f), sB(98304, 0.f);
      for (size_t i = 0; i < L.size(); ++i)
        if (i % 12000 < 600) L[i] = R[i] = 0.7f * std::sin(0.03f * static_cast<float>(i)) * std::exp(-static_cast<float>(i % 12000) / 150.f);
      for (size_t o = 0; o < L.size(); o += static_cast<size_t>(bs)) {
        SideIo io;
        io.multiL[0] = sA.data() + o, io.multiR[0] = sB.data() + o;
        fx->setSideIo(io);
        fx->process(st, ctx, L.data() + o, R.data() + o, bs);
      }
      L.insert(L.end(), sA.begin(), sA.end());
      return L;
    };
    const std::vector<float> a = render(512), b = render(64);
    bool finiteAll = true;
    double tail = 0;
    for (size_t i = 0; i < 98304; ++i) finiteAll = finiteAll && std::isfinite(a[i]);
    for (size_t i = 13000; i < 24000; ++i) tail += a[i] * a[i];  // between hits: the loops play
    expect(finiteAll && tail > 0, "hit looper: loops sound between the hits");
    expect(a == b, "hit looper: identical for any block size (64 / 512)");
    auto fx = createEffect("gl.hitloop");
    expect(fx->latencyFor(stateOf("gl.hitloop"), 48000) == 240, "hit looper: 5 ms look-ahead reported");
  }

  // ------------------------------------------------ control panel state: built without origin bytes
  {
    std::vector<std::pair<int, std::string>> ctl;
    std::vector<double> v;
    for (int i = 0; i < 10; ++i) ctl.push_back({1, "Slider" + std::to_string(i + 1)}), v.push_back(0.5);
    const std::vector<uint8_t> b = flp::buildDashboard(ctl, v);
    const flp::DashPanel d = flp::parseDashboard(b.data(), b.size());
    expect(d.ok && d.links.size() == 10 && d.kinds.size() == 10 && d.kinds[0] == 1, "panel: the built state parses back (10 sliders)");
    const std::string fst = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Patches/Plugin presets/Generators/Dashboard/Sliders.fst";
    if (fs::exists(fst)) {
      flp::File f;
      std::string err;
      const flp::Event* e = flp::readFile(fst, f, &err) ? flp::pluginState(f.events) : nullptr;
      const bool same = e && std::vector<uint8_t>(e->data, e->data + e->size) == b;
      expect(same, "panel: ten centred sliders build the shipped ten-slider state byte for byte");
    }
  }
  return finish();
}
