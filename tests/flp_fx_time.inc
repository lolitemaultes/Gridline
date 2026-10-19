// Effect decoder tests for FlpFxTime.cpp (included by test_flp_mixer.cpp).
#include "../src/core/PluginParams.h"
#include "../src/packs/spacefx/spacefx.h"

static void testFxTime() {
  namespace pp = gl::pp;
  namespace sf = gl::packs::spacefx;
  flp::PluginMapping m;

  // Reverb: version word, 15 knobs, two flag bytes.
  {
    Blob b;
    b.i32(10001);
    for (int v : {150, 40, 24, 50, 100, 25, 60, 150, 400, 0, 128, 64, 96, 684, 0}) b.i32(v);
    b.u8(1).u8(1);
    expect(decodeFx("Fruity Reeverb 2", b.b, m) && m.glType == "gl.reverb", "reverb2: decodes");
    expect(near(param(m, pp::RvbDecayTime), 2.5, 1e-5) && near(param(m, pp::RvbSize), 0.5, 1e-6), "reverb2: decay seconds, size");
    expect(near(param(m, pp::RvbBassMult), 1.5, 1e-6) && near(param(m, pp::RvbCrossover), 400, 1e-3), "reverb2: bass multiplier, crossover");
    expect(near(param(m, pp::RvbLowCut), 150, 1e-3) && near(param(m, pp::RvbHighCut), 4000, 1e-2), "reverb2: cuts");
    expect(near(param(m, pp::RvbWet), (std::pow(11.0, 96 / 256.0) - 1) / (std::pow(11.0, 0.5) - 1), 1e-5) &&
               near(param(m, pp::RvbEarly), (std::pow(11.0, 0.25) - 1) / (std::pow(11.0, 0.5) - 1), 1e-5) && near(param(m, pp::RvbDry), 1.0, 1e-6),
           "reverb2: levels (11^x curve)");
    expect(param(m, pp::RvbPreSync) == 1 && param(m, pp::RvbInputMode) == 1 && near(param(m, pp::RvbPreDelay), 62.5, 1e-4), "reverb2: flags, pre-delay (v / 384 s)");
    expect(m.param && m.param(5) == pp::RvbDecayTime && m.value && near(m.value(5, 1.0), 20.0, 1e-6), "reverb2: automation mapping");
  }
  // Delay 3: synced quarter note, ping-pong, feedback.
  {
    Blob b;
    b.i32(1);
    int v[26] = {6000, 2, 1, 0, 192, 3000, 500, 6000, 3000, 0, 0, 0, 0, 1, 3000, 0, 6000, 0, 0, 0, 0, 60000, 6000, 3000, 0, 6000};
    for (int x : v) b.i32(x);
    expect(decodeFx("Fruity Delay 3", b.b, m) && m.glType == "gl.delay", "delay3: decodes");
    expect(param(m, pp::DlyModel) == 1 && param(m, pp::DlySync) == 1 && param(m, pp::DlyTicks) == 192, "delay3: reference model, 192 ticks = 1 beat");
    expect(param(m, pp::DlyType3) == 2 && near(param(m, pp::DlyFeedback), 0.625, 1e-6) && near(param(m, pp::DlyWet), 0.5, 1e-6),
           "delay3: ping-pong, feedback (×1.25), wet");
    expect(param(m, pp::DlyDistType) == 2 && param(m, pp::DlyRate3) >= 47999 && near(param(m, pp::DlyCutoff3), 21985, 0.5), "delay3: saturate, rate off, cutoff");
  }
  // Delay 2: 3 steps (0.75 beat), invert mode.
  {
    Blob b;
    for (int x : {0, 128, 128, 64, 144, 0, 1, 128}) b.i32(x);
    expect(decodeFx("Fruity Delay 2", b.b, m) && m.glType == "gl.delay", "delay2: decodes");
    expect(param(m, pp::DlySync) == 1 && near(pp::kDelayBeats[static_cast<int>(param(m, pp::DlyTime))], 0.75, 1e-9) && param(m, pp::DlyMode) == 2,
           "delay2: time, mode");
  }
  // Delay Bank: unit 0 enabled with 4 steps.
  {
    Blob b;
    b.i32(16);
    for (int x : {128, 99, 128, 128}) b.i32(x);
    for (int k = 0; k < 8; ++k)
      for (int x : {k == 0 ? -64 : 0, 128, 33145, 17500, 0, 0, 64, 0, k == 0 ? 1920 : 1440, 0, 33145, 17500, 0, 0, 128, 0}) b.i32(x);
    std::vector<uint8_t> tail(4 + 8 * 32, 0);
    tail[4 + 29] = 1;
    tail[4 + 31] = 1;  // tempo-based time
    b.b.insert(b.b.end(), tail.begin(), tail.end());
    expect(decodeFx("Fruity Delay Bank", b.b, m) && m.glType == "gl.multidelay", "delaybank: decodes");
    expect(param(m, sf::mdParam(0, sf::MdOn)) == 1 && param(m, sf::mdParam(1, sf::MdOn)) == 0, "delaybank: unit enables");
    expect(near(param(m, sf::mdParam(0, sf::MdSteps)), 4, 1e-6) && near(param(m, sf::mdParam(0, sf::MdInPan)), -0.5, 1e-6), "delaybank: time, pan");
    expect(near(param(m, sf::MdWet), (std::pow(11.0, 99.0 / 128) - 1) / 10, 1e-6) && param(m, sf::MdModel) == 1, "delaybank: wet (11^x − 1)/10, reference model");
  }
  // Chorus, Hyper Chorus, Flanger, Flangus: layouts and a few values.
  {
    Blob b;
    for (int x : {1, 512, 2250, 341, 1500, 2500, 3500, 23, 0, 0, 0, 2500, 0}) b.i32(x);
    expect(decodeFx("Fruity Chorus", b.b, m) && m.glType == "gl.chorus" && near(param(m, pp::ChoDepthMs), 2.25, 1e-6) &&
               param(m, pp::ChoVoices) == 2 && near(param(m, pp::ChoStereo), 341.0 / 1024, 1e-6) && near(param(m, pp::ChoRate), 0.45, 1e-6) &&
               near(param(m, pp::ChoCrossMode), 1, 1e-9) && near(param(m, pp::ChoCrossFreq), 320.24, 0.05) && m.exact,
           "chorus: decodes");
  }
  {
    Blob b;
    for (int x : {2, 500, 500, 581, 783, 819, 805, 1, 20000, 5724, 259, 1000, 0}) b.i32(x);
    expect(decodeFx("Hyper Chorus", b.b, m) && m.glType == "gl.ensemble" && near(param(m, sf::EnHighPass), 5724, 1e-2) &&
               near(param(m, sf::EnLowPass), 20000, 1e-2) && param(m, sf::EnLines) == 1 && near(param(m, sf::EnMix), 0.259, 1e-6),
           "hyper chorus: decodes (filters in Hz)");
  }
  {
    Blob b;
    for (int x : {1, 0, 2469, 1167, 61, 0, 0, 61, 1024, 0, 3500, 3500, 3229}) b.i32(x);
    expect(decodeFx("Fruity Flanger", b.b, m) && m.glType == "gl.flanger" && near(param(m, pp::FlgPhase), 61, 1e-6) &&
               near(param(m, pp::FlgFeedback), -0.61 * 0.95, 1e-5) && near(param(m, pp::FlgMix), 0.5, 1e-6),
           "flanger: decodes (inverted feedback, mix)");
  }
  {
    Blob b;
    for (int x : {4, 512, 768, 8, 128, -32, 96, 96}) b.i32(x);
    expect(decodeFx("Fruity Flangus", b.b, m) && m.glType == "gl.swirl" && param(m, sf::SwOrder) == 4 &&
               near(param(m, sf::SwDepthK), 0.5, 1e-6) && near(param(m, sf::SwWetK), 0.75, 1e-6) && param(m, sf::SwModel) == 1,
           "flangus: decodes");
  }
  // Truncated states are rejected.
  expect(!decodeFx("Fruity Delay 3", {1, 0, 0, 0}, m), "delay3: short state rejected");
}
