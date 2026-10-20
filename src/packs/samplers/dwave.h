// Gridline — "samplers" pack, J-33 group "dwave": Wave Program
// (gl.waveprogram), a zone sampler that follows the reference zone sampler's
// engine as analysed: a program of zones (sample, key / velocity range,
// trigger and mute groups, loop modes incl. sustained / bounce, amp + 2 mod
// envelopes in a square-root domain, 2 filters of 2 cascaded biquads each, 2
// zone LFOs, a 16-slot matrix on normalised values, 4 insert effects, sends)
// and a program section (master volume, glide, play mode, 2 program LFOs,
// 4 Mod Val knobs, delay / chorus / reverb buses, 2 drives).
// Parameters 0..31 = the program controls (reference tags, raw 0..1 values);
// zones and the remaining program fields live in PluginState::data (text).
#pragma once
#include <array>
#include <string>
#include <vector>

namespace gl::packs::samplers::dw {

enum DwProgramTag {
  DwMaster = 0, DwGlide, DwLfo1Rate, DwLfo1Phase, DwLfo1Attack, DwLfo2Rate, DwLfo2Phase, DwLfo2Attack,
  DwDelayTime, DwDelayFb, DwDelayLowCut, DwDelayHighCut, DwRevRoom, DwRevDamp, DwRevDiffusion, DwRevDecay,
  DwChoDelay, DwChoDepth, DwChoRate, DwChoFb, DwSpare20, DwSpare21, DwSpare22, DwSpare23,
  DwModVal1, DwModVal2, DwModVal3, DwModVal4, DwDriveA, DwDriveB, DwReserved30, DwReserved31,
  DwCount
};

struct DwLfo {
  int wave = 0;
  bool sync = false;
  float rate = 0.1f, phase = 1.f, attack = 0.f;
};
struct DwSlot {
  int src = 0, dst = 0;
  float amt = 0.5f;
};
struct DwFilter {
  int type = 0;
  float cut = 0.4725f, res = 0.5f, shape = 0.f;
};

struct DwZone {
  int root = 60, lo = 0, hi = 127, vlo = 0, vhi = 127, muteGroup = 0, trigGroup = 0;
  bool parallel = false;
  float gain = 1.f, pan = 0.5f;
  std::string name, path;
  int ticks = 16;
  float rate = 44100.f;
  int loopMode = 0;  // 0 disabled, 1 one-shot, 2 forward, 3 sustained, 4 bounce
  int loopStart = 0, loopEnd = 0, sampleStart = 0;
  float tune = 0.5f;
  int transpose = 0, fine = 0, keytrack = 100;
  bool ts = false, tsSync = false;
  float tsTime = 0.5f, tsGrain = 0.5f, tsSmooth = 1.f;
  float send[4] = {0.5f, 0.5f, 0.5f, 1.f};  // delay, chorus, reverb, dry
  DwFilter f[2];
  bool beatSync = false, waveSlice = false;
  float amp[4] = {0.f, 0.5f, 1.f, 0.25f};
  bool fxOn[4] = {false, false, false, false};  // ring mod, decimator, bit reducer, phaser
  float fxA[4] = {0.f, 0.f, 0.f, 0.5f}, fxB[4] = {1.f, 1.f, 1.f, 1.f};
  float menv[2][4] = {{0.f, 0.5f, 0.5f, 0.25f}, {0.f, 0.5f, 0.5f, 0.25f}};
  DwLfo lfo[2] = {DwLfo{}, DwLfo{0, false, 0.55f, 1.f, 0.f}};
  DwSlot slot[16];
  DwZone();
};

struct DwTrigger {
  int type = 0;
  float prob = 1.f, overlap = 0.f;
};

struct DwProgram {
  std::string name;
  int playMode = 0;  // 0 poly, 1 mono, 2 legato
  bool glideAuto = false;
  bool driveOn[2] = {false, false};
  bool delayOn = false, pingPong = false, revOn = false, choOn = false;
  DwLfo plfo[2];
  std::array<DwTrigger, 100> trig{};
  // Instance options.
  bool clipVolume = false, compatVelocity = false, sinc = false, hq = false;
  // MIDI channel state (channel 0).
  int bendRange = 200;
  float cc7 = 100.f / 127.f, cc10 = 0.5f, cc11 = 1.f, cc1 = 0.f;
  std::vector<DwZone> zones;
};

std::string formatProgram(const DwProgram& p);
bool parseProgram(const std::string& text, DwProgram& p);

}  // namespace gl::packs::samplers::dw
