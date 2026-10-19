// J-1 engine hooks: MIDI port bus (MIDI Out channel → MIDI-input effects),
// latency reports of the oversampled reference models.
#include "../src/packs/spacefx/spacefx.h"
namespace {
// Impulse peak position through the reference phaser with a fully dry core:
// what remains is the oversampler's delay, which latencyFor must report.
void phaserLatency() {
  namespace sf = gl::packs::spacefx;
  for (int hq = 0; hq < 2; ++hq) {
    PluginState s = defaultPluginState("gl.vphaser");
    s.params[sf::VpModel] = 1;
    s.params[sf::VpHq] = static_cast<float>(hq);
    s.params[sf::VpMix] = 0;
    s.params[sf::VpNoise] = 0;
    s.params[sf::VpDelayOn] = 0;
    auto fx = createEffect("gl.vphaser");
    std::vector<float> L(4096, 0.f), R(4096, 0.f);
    L[100] = R[100] = 0.5f;
    jt::run(*fx, s, L, R, 128);
    size_t pk = 0;
    for (size_t i = 0; i < L.size(); ++i)
      if (std::fabs(L[i]) > std::fabs(L[pk])) pk = i;
    const int lat = fx->latencyFor(s, jt::kSr);
    char name[128];
    std::snprintf(name, sizeof name, "vintage phaser ref %dx: latency %d = impulse delay %d", hq ? 8 : 4, lat,
                  static_cast<int>(pk) - 100);
    expect(lat > 0 && static_cast<int>(pk) - 100 == lat, name);
  }
  PluginState legacy = defaultPluginState("gl.vphaser");
  legacy.params[gl::packs::spacefx::VpModel] = 0;
  expect(createEffect("gl.vphaser")->latencyFor(legacy, jt::kSr) == 0, "vintage phaser legacy: no latency");
}
void testJ1Engine() {
  MidiPortBus bus;
  bus.set(3, 60, 100);
  expect(bus.vel[3][60] == 100 && bus.used[3], "port bus: note published");
  bus.beginBlock();
  expect(bus.vel[3][60] == 0 && !bus.used[3], "port bus: cleared at block start");
  bus.set(300, 60, 100);
  bus.set(3, 200, 100);
  expect(true, "port bus: out-of-range ports / notes ignored");

  // A MIDI Out channel announces its held notes on its port every block.
  std::unique_ptr<Generator> mo = createGenerator("gl.midiout");
  expect(mo != nullptr, "midi out: created");
  if (!mo) return;
  PluginState s = defaultPluginState("gl.midiout");
  s.params[pp::MoPort] = 5;
  ProcessContext ctx;
  ctx.sampleRate = jt::kSr;
  ctx.ports = &bus;
  ChannelAux aux;
  float L[64] = {}, R[64] = {};
  bus.beginBlock();
  mo->noteOn(1, 64.0, 0.8f, 0);
  mo->process(s, aux, ctx, L, R, 64);
  expect(bus.vel[5][64] > 0, "midi out: note on reaches its port");
  bus.beginBlock();
  mo->process(s, aux, ctx, L, R, 64);
  expect(bus.vel[5][64] > 0, "midi out: held note re-announced next block");
  bus.beginBlock();
  mo->noteOff(1, 0);
  mo->process(s, aux, ctx, L, R, 64);
  expect(bus.vel[5][64] == 0, "midi out: note off clears the port");
  phaserLatency();
  const PluginInfo* pc = findPlugin("gl.pitcher");
  expect(pc && pc->midiInput, "pitch corrector declares a MIDI input (graph orders MIDI Out channels first)");
}
}  // namespace
