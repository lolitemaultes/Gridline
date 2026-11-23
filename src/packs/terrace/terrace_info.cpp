// Gridline — "terrace" pack: the catalogue entry of Terrace (gl.terrace), its factory presets,
// the step bank text and the sources it listens to in the project (see terrace.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "../../core/Document.h"
#include "../../core/MenuOps.h"
#include "../../core/Params.h"
#include "../Packs.h"
#include "../tideline/tideline.h"
#include "../tideline/tideline_source.h"
#include "terrace.h"

namespace gl::packs::terrace {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  const double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

const char* const kLaneWords[kLanes] = {"volume", "cutoff", "pan"};
const char* const kSizeNames[pp::kShaperRates] = {"Auto", "1/32", "1/32T", "1/16", "1/16T", "1/8", "1/8T", "1/4", "1/4T", "1/2", "1/2T", "1 bar"};


// Allocation-free reading: one signed integer, skipping spaces and tabs (not line ends).
bool readInt(const char*& s, long& out) {
  while (*s == ' ' || *s == '\t') ++s;
  bool neg = false;
  if (*s == '-' || *s == '+') neg = *s++ == '-';
  if (*s < '0' || *s > '9') return false;
  long v = 0;
  while (*s >= '0' && *s <= '9') {
    if (v < 100000000) v = v * 10 + (*s - '0');
    ++s;
  }
  out = neg ? -v : v;
  return true;
}

// ---- factory presets: steps drawn in code (original patterns)

struct PresetBuilder {
  std::unique_ptr<Bank> b = std::make_unique<Bank>();
  std::vector<std::pair<int, float>> v;
  PresetBuilder() { clear(*b); }
  void set(int k, float x) { v.push_back({k, x}); }
  void lane(int band, int ln, int steps, int size, std::initializer_list<float> levels, int pattern = 0) {
    set(laneParam(band, ln, LpSteps), static_cast<float>(steps - 1));
    set(laneParam(band, ln, LpSize), static_cast<float>(size));
    int k = 0;
    for (float x : levels) b->val[pattern][band][ln][k++] = x;
  }
  // The same levels into a lane of another pattern (the lane's length and size are shared).
  void steps(int band, int ln, std::initializer_list<float> levels, int pattern) {
    int k = 0;
    for (float x : levels) b->val[pattern][band][ln][k++] = x;
  }
  void depth(int band, int ln, float d) { set(laneParam(band, ln, LpDepth), d); }
  void curve(int band, int ln, float c) { set(laneParam(band, ln, LpCurve), c); }
  void swing(int band, int ln, float s) { set(laneParam(band, ln, LpSwing), s); }
  void hits(int band, int ln, int step, int n, int pattern = 0) { b->hits[pattern][band][ln][step] = static_cast<uint8_t>(n); }
  void chance(int band, int ln, int step, int pc, int pattern = 0) { b->chance[pattern][band][ln][step] = static_cast<uint8_t>(pc); }
  void route(int r, int src, int tgt, float amt) {
    set(routeParam(r, RtSource), static_cast<float>(src)), set(routeParam(r, RtTarget), static_cast<float>(tgt)), set(routeParam(r, RtAmount), amt);
  }
  void lfo(int i, int rate, int shape, float amount) {
    set(lfoParam(i, LfRate), static_cast<float>(rate)), set(lfoParam(i, LfShape), static_cast<float>(shape)), set(lfoParam(i, LfAmount), amount);
  }
  void smooth(float ms) { set(TrSmooth, ms); }
  void out(float db) { set(TrOutput, db); }
  void gain(int band, float db) { set(bandParam(band, BpGain), db); }
  PluginInfo::Preset done(const char* name) {
    finish(*b);
    const std::string w = write(*b);
    // Even with every step at rest the preset carries its (empty) step text, so loading it puts
    // the steps back to rest instead of keeping the ones already there.
    return PluginInfo::Preset{name, v, w.empty() ? std::string("TR1\n") : w};
  }
};

// Size indices of the step size list.
enum { Sz32 = 1, Sz32T, Sz16, Sz16T, Sz8, Sz8T, Sz4, Sz4T, Sz2, Sz2T, SzBar };
// LFO rate indices (kLfoRateNames).
enum { Rt8th = 4, RtBeat = 7, RtHalfBar = 9, RtBar = 11, Rt2Bars = 12, Rt4Bars = 13 };

// The preset groups, in list order, with the presets in each (the editor shows them as headings).
const PresetGroupEntry kGroupOf[] = {
    {"Clean slate", "Start"},
    {"Four floors", "Chops and stutters"},     {"Sixteenth chop", "Chops and stutters"},   {"Bar end stutter", "Chops and stutters"},
    {"Ratchet highs", "Chops and stutters"},   {"Broken sixteenths", "Chops and stutters"},
    {"Low pump", "Pumping and ducking"},       {"Breathing quarters", "Pumping and ducking"}, {"Room for the kick", "Pumping and ducking"},
    {"Ducked lows", "Pumping and ducking"},    {"Duck from notes", "Pumping and ducking"},
    {"Triplet tops", "Triplets and odd grids"}, {"Three across the bar", "Triplets and odd grids"}, {"Shuffle triplets", "Triplets and odd grids"},
    {"Three against four", "Triplets and odd grids"}, {"Fives and sevens", "Triplets and odd grids"}, {"Dotted eighths", "Triplets and odd grids"},
    {"Ping pong highs", "Pan and width"},      {"Slow orbit", "Pan and width"},            {"Left centre right", "Pan and width"},
    {"Rolling mids", "Filter rhythms"},        {"Muffled offbeats", "Filter rhythms"},     {"Talking mids", "Filter rhythms"},
    {"Slow swells", "Slow textures"},          {"Drifting air", "Slow textures"},          {"Soft breathing", "Slow textures"},
    {"Slow filter sweep", "Slow textures"},
    {"Fourth bar fill", "Fills and chains"},   {"Question and answer", "Fills and chains"},
    {"Gentle groove", "Mix glue"},             {"Offbeat air", "Mix glue"},                {"Tight lows", "Mix glue"},
};

std::vector<PluginInfo::Preset> factoryPresets() {
  std::vector<PluginInfo::Preset> out;
  out.push_back(PresetBuilder().done("Clean slate"));

  // ---------------------------------------------------------------- chops and stutters
  {  // the four bands each at their own length: 16 against 5, 12 triplets and 20 thirty-seconds
    PresetBuilder p;
    p.lane(0, LnVol, 16, Sz16, {1, 0, .6f, 0, 1, 0, 0, .8f, 1, 0, .6f, 0, 1, .3f, 0, .9f});
    p.lane(1, LnVol, 5, Sz16, {1, .2f, 1, .2f, .5f});
    p.swing(1, LnVol, 0.12f);
    p.lane(2, LnVol, 12, Sz8T, {1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1});
    p.hits(2, LnVol, 6, 3);
    p.lane(3, LnVol, 20, Sz32, {.9f, .4f, .7f, .2f, 1, .4f, .6f, .2f, .9f, .3f, .9f, .4f, .7f, .2f, 1, .4f, .6f, .2f, .9f, .3f});
    p.swing(3, LnVol, 0.3f);
    p.chance(3, LnVol, 1, 40), p.chance(3, LnVol, 13, 70);
    p.hits(3, LnVol, 4, 2), p.hits(3, LnVol, 14, 4);
    p.route(0, SrcLfo1, bandTarget(1, TbVolDepth), -0.4f);
    p.route(1, SrcLfo2, bandTarget(3, TbSize), 0.25f);
    p.route(2, SrcFollower, TgSplit2, -0.3f);
    p.route(3, SrcLfo1, bandTarget(3, TbPan), 0.45f);
    p.set(lfoParam(0, LfAmount), 0.6f), p.set(lfoParam(1, LfAmount), 0.6f);
    p.set(TrFolAmount, 0.4f), p.set(TrFolGain, 6);
    p.out(3.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Four floors"));
  }
  {  // the whole sound gated in sixteenths, the lows kept half open so the groove keeps its weight
    PresetBuilder p;
    for (int b = 0; b < kBands; ++b) p.lane(b, LnVol, 16, Sz16, {1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, .6f});
    p.depth(0, LnVol, 0.5f), p.depth(1, LnVol, 0.8f);
    p.smooth(2.5f);
    p.out(1.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Sixteenth chop"));
  }
  {  // plays straight, then the last beat of every second bar stutters in faster and faster repeats
    PresetBuilder p;
    for (int b = 0; b < kBands; ++b) {
      p.lane(b, LnVol, 32, Sz16, {});
      p.hits(b, LnVol, 28, 2), p.hits(b, LnVol, 29, 2), p.hits(b, LnVol, 30, 3), p.hits(b, LnVol, 31, 4);
    }
    p.b->val[0][0][LnVol][30] = 0, p.b->val[0][0][LnVol][31] = 0;  // the lows drop out under the fastest hits
    p.smooth(1.5f);
    out.push_back(p.done("Bar end stutter"));
  }
  {  // the highs in eighths with ratchets of two, three and four hits; the high mids follow lightly
    PresetBuilder p;
    p.lane(3, LnVol, 8, Sz8, {1, .7f, 1, .8f, 1, .7f, 1, 1});
    p.hits(3, LnVol, 2, 2), p.hits(3, LnVol, 5, 3), p.hits(3, LnVol, 7, 4);
    p.lane(2, LnVol, 8, Sz8, {1, .7f, 1, .8f, 1, .7f, 1, 1});
    p.hits(2, LnVol, 7, 4);
    p.depth(2, LnVol, 0.5f);
    p.smooth(1.5f);
    out.push_back(p.done("Ratchet highs"));
  }
  {  // a sixteenth gate on the mids and highs where some steps only sometimes play: never the same bar twice
    PresetBuilder p;
    const std::initializer_list<float> lv = {1, .5f, 1, .8f, 1, .5f, 1, .8f, 1, .5f, 1, .8f, 1, .5f, 1, .8f};
    for (int b = 1; b < kBands; ++b) p.lane(b, LnVol, 16, Sz16, lv);
    for (int b = 1; b < kBands; ++b)
      for (int k : {1, 3, 6, 9, 11, 14, 15}) p.chance(b, LnVol, k, b == 3 ? 55 : 70);
    p.depth(1, LnVol, 0.6f);
    p.smooth(2);
    p.out(0.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Broken sixteenths"));
  }

  // ---------------------------------------------------------------- pumping and ducking
  {  // the lows pump on the beat, the rest stays put
    PresetBuilder p;
    p.lane(0, LnVol, 4, Sz16, {0, .45f, .8f, 1});
    p.curve(0, LnVol, 0.45f);
    p.lane(1, LnVol, 4, Sz16, {.5f, .75f, .95f, 1});
    p.curve(1, LnVol, 0.45f);
    p.set(TrBands, 0), p.set(TrSplit1, 150), p.set(TrSplit2, 2500);
    p.out(3);  // level matched to the bypassed sound
    out.push_back(p.done("Low pump"));
  }
  {  // every band breathes on the beat, the lows deepest and the highs barely
    PresetBuilder p;
    p.lane(0, LnVol, 4, Sz16, {.05f, .5f, .85f, 1});
    p.lane(1, LnVol, 4, Sz16, {.25f, .65f, .9f, 1});
    p.lane(2, LnVol, 4, Sz16, {.5f, .8f, .95f, 1});
    p.lane(3, LnVol, 4, Sz16, {.7f, .9f, 1, 1});
    for (int b = 0; b < kBands; ++b) p.curve(b, LnVol, 0.4f);
    p.out(3);  // level matched to the bypassed sound
    out.push_back(p.done("Breathing quarters"));
  }
  {  // a short dip in the lows at each beat: room for a kick on a bass or a full loop
    PresetBuilder p;
    p.lane(0, LnVol, 8, Sz32, {.15f, .5f, .85f, 1, 1, 1, 1, 1});
    p.curve(0, LnVol, 0.3f);
    p.lane(1, LnVol, 8, Sz32, {.6f, .85f, 1, 1, 1, 1, 1, 1});
    p.curve(1, LnVol, 0.3f);
    p.set(TrSplit1, 140);
    p.out(1);  // level matched to the bypassed sound
    out.push_back(p.done("Room for the kick"));
  }
  {  // another track's level ducks the lows (pick it under Sidechain), the highs chop in eighths
    PresetBuilder p;
    p.lane(3, LnVol, 8, Sz16, {1, .2f, 1, .2f, 1, .2f, 1, 1});
    p.route(0, SrcSidechain, bandTarget(0, TbGain), -0.75f);
    p.set(TrScThreshold, -30);
    out.push_back(p.done("Ducked lows"));
  }
  {  // a channel's notes duck the lows and low mids (pick the channel under Sidechain)
    PresetBuilder p;
    p.set(TrScListen, LsNotes);
    p.set(TrScRelease, 180);
    p.route(0, SrcSidechain, bandTarget(0, TbGain), -0.6f);
    p.route(1, SrcSidechain, bandTarget(1, TbGain), -0.35f);
    out.push_back(p.done("Duck from notes"));
  }

  // ---------------------------------------------------------------- triplets and odd grids
  {  // triplet gates on the tops with a few skipped hits, the high mids bouncing left and right
    PresetBuilder p;
    p.lane(3, LnVol, 12, Sz16T, {1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1});
    for (int k = 0; k < 12; k += 3) p.chance(3, LnVol, k + 2, 65);
    p.lane(2, LnPan, 8, Sz8, {-.7f, .7f, -.4f, .4f, -.7f, .7f, -.2f, .2f});
    out.push_back(p.done("Triplet tops"));
  }
  {  // three long steps across each bar (Steps 3, Size 1/2T) on the mids, the highs panning with them
    PresetBuilder p;
    p.lane(1, LnVol, 3, Sz2T, {1, .3f, .6f});
    p.lane(2, LnVol, 3, Sz2T, {1, .3f, .6f});
    p.lane(3, LnPan, 3, Sz2T, {-.6f, 0, .6f});
    p.smooth(8);
    p.out(1.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Three across the bar"));
  }
  {  // eighth note triplets: long, short, medium, so a straight loop takes a shuffle
    PresetBuilder p;
    p.lane(0, LnVol, 3, Sz8T, {1, .6f, .8f});
    p.depth(0, LnVol, 0.5f);
    for (int b = 1; b < kBands; ++b) p.lane(b, LnVol, 3, Sz8T, {1, .15f, .6f});
    p.depth(1, LnVol, 0.7f);
    p.smooth(2.5f);
    p.out(1.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Shuffle triplets"));
  }
  {  // the low mids loop every 3 sixteenths, the highs every 4: the accents walk across the bar
    PresetBuilder p;
    p.lane(1, LnVol, 3, Sz16, {1, .25f, .5f});
    p.lane(3, LnVol, 4, Sz16, {1, .3f, .7f, .3f});
    p.depth(1, LnVol, 0.8f);
    p.smooth(2);
    p.out(1.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Three against four"));
  }
  {  // loops of 5 and 7 sixteenths on the mids and highs: they meet again only every 35 steps
    PresetBuilder p;
    p.lane(1, LnVol, 5, Sz16, {1, .3f, .8f, .3f, .5f});
    p.lane(2, LnVol, 7, Sz16, {1, .2f, .7f, .2f, 1, .2f, .5f});
    p.lane(3, LnPan, 7, Sz16, {-.5f, .5f, -.3f, .3f, -.5f, .5f, 0});
    p.depth(1, LnVol, 0.7f);
    p.smooth(2);
    p.out(1.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Fives and sevens"));
  }
  {  // a hit every three sixteenths (a dotted eighth) on the high mids and highs, the classic echo rhythm
    PresetBuilder p;
    p.lane(2, LnVol, 3, Sz16, {1, .15f, .35f});
    p.lane(3, LnVol, 3, Sz16, {1, .15f, .35f});
    p.lane(3, LnPan, 6, Sz16, {-.4f, -.4f, -.4f, .4f, .4f, .4f});
    p.smooth(2);
    out.push_back(p.done("Dotted eighths"));
  }

  // ---------------------------------------------------------------- pan and width
  {  // the highs jump left and right in eighths, the high mids answer the other way
    PresetBuilder p;
    p.lane(3, LnPan, 2, Sz8, {-.8f, .8f});
    p.lane(2, LnPan, 2, Sz8, {.45f, -.45f});
    p.smooth(4);
    out.push_back(p.done("Ping pong highs"));
  }
  {  // two slow LFOs turn the upper bands around the room in opposite directions, the lows stay put
    PresetBuilder p;
    p.lfo(0, Rt2Bars, ShSine, 1);
    p.lfo(1, Rt4Bars, ShTriangle, 1);
    p.route(0, SrcLfo1, bandTarget(2, TbPan), 0.6f);
    p.route(1, SrcLfo1, bandTarget(3, TbPan), -0.75f);
    p.route(2, SrcLfo2, bandTarget(1, TbPan), 0.3f);
    out.push_back(p.done("Slow orbit"));
  }
  {  // eighth note triplets walk the high mids and highs left, centre and right
    PresetBuilder p;
    p.lane(2, LnPan, 3, Sz8T, {-.75f, 0, .75f});
    p.lane(3, LnPan, 3, Sz8T, {.85f, 0, -.85f});
    p.smooth(4);
    out.push_back(p.done("Left centre right"));
  }

  // ---------------------------------------------------------------- filter rhythms
  {  // the low mids' filter rolls open and shut, an LFO pans them, the highs ratchet
    PresetBuilder p;
    p.lane(1, LnCut, 8, Sz8, {.15f, .35f, .6f, .9f, .25f, .55f, .8f, 1});
    p.curve(1, LnCut, 0.6f);
    p.lane(3, LnVol, 8, Sz8, {1, 0, 1, 1, 0, 1, 1, 1});
    p.hits(3, LnVol, 3, 3), p.hits(3, LnVol, 7, 4);
    p.route(0, SrcLfo2, bandTarget(1, TbPan), 0.5f);
    p.set(lfoParam(1, LfAmount), 1.0f);
    p.out(0.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Rolling mids"));
  }
  {  // on every offbeat eighth the upper bands close their filters: the beat stays bright, the offbeats dull
    PresetBuilder p;
    for (int b = 1; b < kBands; ++b) p.lane(b, LnCut, 4, Sz16, {1, 1, .3f, .55f});
    p.depth(1, LnCut, 0.6f);
    p.smooth(4);
    out.push_back(p.done("Muffled offbeats"));
  }
  {  // the mids' filters open and close in a gliding sixteenth line, like a voice saying the rhythm
    PresetBuilder p;
    p.lane(1, LnCut, 8, Sz16, {.25f, .6f, 1, .5f, .3f, .8f, .45f, 1});
    p.lane(2, LnCut, 8, Sz16, {.35f, .7f, 1, .55f, .3f, .9f, .5f, 1});
    p.curve(1, LnCut, 0.5f), p.curve(2, LnCut, 0.5f);
    p.smooth(6);
    out.push_back(p.done("Talking mids"));
  }

  // ---------------------------------------------------------------- slow textures
  {  // over four bars each band swells and settles at its own time, softly (for pads and long sounds)
    PresetBuilder p;
    p.set(TrLength, 3);
    p.lane(0, LnVol, 4, SzBar, {1, .7f, .85f, .6f});
    p.lane(1, LnVol, 4, SzBar, {.6f, 1, .7f, .9f});
    p.lane(2, LnVol, 4, SzBar, {.45f, .8f, 1, .6f});
    p.lane(3, LnVol, 4, SzBar, {.8f, .5f, .7f, 1});
    for (int b = 0; b < kBands; ++b) p.curve(b, LnVol, 0.3f), p.depth(b, LnVol, 0.8f);
    p.smooth(40);
    p.out(1.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Slow swells"));
  }
  {  // slow random drifts in the highs' level and place, and the high mids' place
    PresetBuilder p;
    p.lfo(0, RtBar, ShSmoothRandom, 1);
    p.lfo(1, Rt2Bars, ShSmoothRandom, 1);
    p.route(0, SrcLfo1, bandTarget(3, TbGain), 0.15f);
    p.route(1, SrcLfo1, bandTarget(3, TbPan), 0.5f);
    p.route(2, SrcLfo2, bandTarget(2, TbPan), -0.4f);
    p.route(3, SrcLfo2, bandTarget(2, TbGain), 0.08f);
    p.smooth(20);
    out.push_back(p.done("Drifting air"));
  }
  {  // the mids rise and fall every half bar like slow breath
    PresetBuilder p;
    p.lane(1, LnVol, 2, Sz2, {1, .45f});
    p.lane(2, LnVol, 2, Sz2, {.5f, 1});
    p.curve(1, LnVol, 0.6f), p.curve(2, LnVol, 0.6f);
    p.depth(1, LnVol, 0.7f), p.depth(2, LnVol, 0.7f);
    p.smooth(20);
    p.out(1);  // level matched to the bypassed sound
    out.push_back(p.done("Soft breathing"));
  }
  {  // the upper bands' filters close over one bar and open again over the next
    PresetBuilder p;
    for (int b = 1; b < kBands; ++b) p.lane(b, LnCut, 2, SzBar, {1, .2f});
    p.depth(1, LnCut, 0.5f);
    for (int b = 1; b < kBands; ++b) p.curve(b, LnCut, 0.1f);
    p.smooth(30);
    out.push_back(p.done("Slow filter sweep"));
  }

  // ---------------------------------------------------------------- fills and chains
  {  // three bars of a light groove, then pattern 2 breaks the fourth bar up (Chain on)
    PresetBuilder p;
    p.set(TrChain, 1);
    p.b->chain[0] = 0, p.b->chain[1] = 0, p.b->chain[2] = 0, p.b->chain[3] = 1;
    for (int b = 1; b < kBands; ++b) p.lane(b, LnVol, 16, Sz16, {1, .7f, 1, .85f, 1, .7f, 1, .85f, 1, .7f, 1, .85f, 1, .7f, 1, .85f});
    for (int b = 1; b < kBands; ++b) p.steps(b, LnVol, {1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1}, 1);
    p.steps(0, LnVol, {1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0}, 1);
    for (int b = 0; b < kBands; ++b) p.hits(b, LnVol, 8, 2, 1), p.hits(b, LnVol, 9, 2, 1);
    for (int b = 1; b < kBands; ++b) p.hits(b, LnVol, 12, 3, 1), p.hits(b, LnVol, 13, 3, 1), p.hits(b, LnVol, 14, 4, 1), p.hits(b, LnVol, 15, 4, 1);
    p.smooth(2);
    p.out(1);  // level matched to the bypassed sound
    out.push_back(p.done("Fourth bar fill"));
  }
  {  // one bar the lows and low mids lead with the tops gated back, the next bar the other way round
    PresetBuilder p;
    p.set(TrChain, 1);
    p.b->chain[0] = 0, p.b->chain[1] = 1;
    p.lane(0, LnVol, 16, Sz16, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, 0);
    p.steps(0, LnVol, {1, .5f, .5f, .5f, .8f, .5f, .5f, .5f, 1, .5f, .5f, .5f, .8f, .5f, .5f, .5f}, 1);
    p.lane(1, LnVol, 16, Sz16, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, 0);
    p.steps(1, LnVol, {1, .5f, .5f, .5f, .8f, .5f, .5f, .5f, 1, .5f, .5f, .5f, .8f, .5f, .5f, .5f}, 1);
    for (int b = 2; b < kBands; ++b) {
      p.lane(b, LnVol, 16, Sz16, {1, .4f, .4f, .4f, .7f, .4f, .4f, .4f, 1, .4f, .4f, .4f, .7f, .4f, .4f, .4f}, 0);
      p.steps(b, LnVol, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, 1);
    }
    p.smooth(4);
    p.out(1.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Question and answer"));
  }

  // ---------------------------------------------------------------- mix glue
  {  // small sixteenth accents with a little swing on every band: a loop leans into the beat
    PresetBuilder p;
    for (int b = 0; b < kBands; ++b) {
      p.lane(b, LnVol, 16, Sz16, {1, .6f, .85f, .7f, 1, .6f, .85f, .7f, 1, .6f, .85f, .7f, 1, .6f, .9f, .75f});
      p.depth(b, LnVol, b == 0 ? 0.15f : 0.3f);
      p.swing(b, LnVol, 0.15f);
    }
    p.smooth(5);
    out.push_back(p.done("Gentle groove"));
  }
  {  // the highs lift a little on the offbeat eighths and sit back on the beat
    PresetBuilder p;
    p.lane(3, LnVol, 4, Sz16, {.75f, .8f, 1, .8f});
    p.lane(2, LnVol, 4, Sz16, {.9f, .9f, 1, .9f});
    p.curve(3, LnVol, 0.2f);
    p.smooth(6);
    out.push_back(p.done("Offbeat air"));
  }
  {  // the lows sit back a little after each beat, so a busy bass line reads clearer
    PresetBuilder p;
    p.lane(0, LnVol, 4, Sz16, {1, .85f, .7f, .8f});
    p.curve(0, LnVol, 0.2f);
    p.set(TrSplit1, 160);
    p.smooth(8);
    p.out(0.5f);  // level matched to the bypassed sound
    out.push_back(p.done("Tight lows"));
  }
  return out;
}

bool isTerrace(const Song& s, ObjectId owner, int slot) {
  const MixerTrack* t = s.mixerTrack(owner);
  return t && slot >= 0 && static_cast<size_t>(slot) < t->slots.size() && t->slots[static_cast<size_t>(slot)].plugin.type == kType;
}

bool hasSend(const Song& s, ObjectId from, ObjectId to, bool* sidechain) {
  const MixerTrack* t = s.mixerTrack(from);
  if (!t) return false;
  for (const Send& sd : t->sends)
    if (sd.to == to) {
      if (sidechain) *sidechain = sd.sidechain;
      return true;
    }
  return false;
}

void setP(Document& d, ObjectId owner, int slot, int k, double v) { d.setParam(slotParam(owner, slot, k), v, false); }
double getP(const Song& s, ObjectId owner, int slot, int k) {
  const MixerTrack* t = s.mixerTrack(owner);
  if (!t || slot < 0 || static_cast<size_t>(slot) >= t->slots.size()) return 0;
  const PluginState& p = t->slots[static_cast<size_t>(slot)].plugin;
  const PluginInfo* in = findPlugin(kType);
  if (static_cast<size_t>(k) < p.params.size()) return p.params[static_cast<size_t>(k)];
  return in ? in->params[static_cast<size_t>(k)].def : 0.0;
}

}  // namespace

// ------------------------------------------------------------------ names

const char* presetGroup(const std::string& name) {
  for (const PresetGroupEntry& e : kGroupOf)
    if (name == e.name) return e.group;
  return "";
}

const char* bandName(int band, int bands) {
  static const char* const four[kBands] = {"Low", "Low mid", "High mid", "High"};
  static const char* const three[3] = {"Low", "Mid", "High"};
  if (bands == 3) return band >= 0 && band < 3 ? three[band] : "";
  return band >= 0 && band < kBands ? four[band] : "";
}
const char* bandCaps(int band, int bands) {
  static const char* const four[kBands] = {"LOW", "LOW MID", "HIGH MID", "HIGH"};
  static const char* const three[3] = {"LOW", "MID", "HIGH"};
  if (bands == 3) return band >= 0 && band < 3 ? three[band] : "";
  return band >= 0 && band < kBands ? four[band] : "";
}

std::string targetName(int target, int bands) {
  if (target <= TgNone || target >= TgCount) return "None";
  if (target >= TgSplit1) {
    static const char* const g[] = {"Split 1", "Split 2", "Split 3", "Mix", "Output"};
    return g[target - TgSplit1];
  }
  const int b = (target - TgBand0) / TbCount, k = (target - TgBand0) % TbCount;
  static const char* const w[TbCount] = {"depth", "cutoff depth", "pan depth", "step size", "swing", "gain", "cutoff", "pan"};
  const std::string bn = bands == 3 && b == 3 ? "Band 4" : bandName(b, bands);
  return bn + " " + w[k];
}

// ------------------------------------------------------------------ the step bank

void clear(Bank& b) {
  for (int p = 0; p < kPatterns; ++p)
    for (int d = 0; d < kBands; ++d)
      for (int l = 0; l < kLanes; ++l) {
        for (int k = 0; k < kSteps; ++k) b.val[p][d][l][k] = laneDefault(l), b.chance[p][d][l][k] = 100, b.hits[p][d][l][k] = 1;
        b.anyChance[p][d][l] = b.anyHits[p][d][l] = false;
      }
  for (auto& c : b.chain) c = -1;
  b.chainLen = 0;
}

void finish(Bank& b) {
  for (int p = 0; p < kPatterns; ++p)
    for (int d = 0; d < kBands; ++d)
      for (int l = 0; l < kLanes; ++l) {
        bool c = false, h = false;
        for (int k = 0; k < kSteps; ++k) c = c || b.chance[p][d][l][k] < 100, h = h || b.hits[p][d][l][k] > 1;
        b.anyChance[p][d][l] = c, b.anyHits[p][d][l] = h;
      }
  b.chainLen = 0;
  for (int k = 0; k < kChain; ++k)
    if (b.chain[k] >= 0) b.chainLen = k + 1;
}

void parse(const char* text, Bank& b) {
  clear(b);
  if (!text) return;
  const char* s = text;
  while (*s) {
    const char kind = *s;
    const char* line = s + 1;
    long p = 0, d = 0, l = 0, at = 0, v = 0;
    if ((kind == 'v' || kind == 'q' || kind == 'r') && (*line == ' ' || *line == '\t') && readInt(line, p) && readInt(line, d) && readInt(line, l) &&
        readInt(line, at) && p >= 0 && p < kPatterns && d >= 0 && d < kBands && l >= 0 && l < kLanes) {
      for (long k = at; readInt(line, v); ++k) {
        if (k < 0 || k >= kSteps) continue;
        if (kind == 'v') b.val[p][d][l][k] = std::clamp(static_cast<float>(v) / 10000.f, l == LnPan ? -1.f : 0.f, 1.f);
        else if (kind == 'q') b.chance[p][d][l][k] = static_cast<uint8_t>(std::clamp(v, 0L, 100L));
        else b.hits[p][d][l][k] = static_cast<uint8_t>(std::clamp(v, 1L, 4L));
      }
    } else if (kind == 'n' && (*line == ' ' || *line == '\t')) {
      for (int k = 0; readInt(line, v); ++k)
        if (k < kChain) b.chain[k] = static_cast<int8_t>(v >= 1 && v <= kPatterns ? v - 1 : -1);
    }
    while (*s && *s != '\n') ++s;
    if (*s == '\n') ++s;
  }
  finish(b);
}

std::string write(const Bank& b) {
  std::string out;
  char buf[32];
  auto row = [&](char kind, int p, int d, int l, auto get, auto isDefault) {
    int last = -1;
    for (int k = 0; k < kSteps; ++k)
      if (!isDefault(k)) last = k;
    if (last < 0) return;
    int first = 0;
    while (first < last && isDefault(first)) ++first;
    std::snprintf(buf, sizeof buf, "%c %d %d %d %d", kind, p, d, l, first);
    out += buf;
    for (int k = first; k <= last; ++k) std::snprintf(buf, sizeof buf, " %ld", get(k)), out += buf;
    out += '\n';
  };
  for (int p = 0; p < kPatterns; ++p)
    for (int d = 0; d < kBands; ++d)
      for (int l = 0; l < kLanes; ++l) {
        const long def = std::lround(laneDefault(l) * 10000.0);
        row('v', p, d, l, [&](int k) { return std::lround(b.val[p][d][l][k] * 10000.0); }, [&](int k) { return std::lround(b.val[p][d][l][k] * 10000.0) == def; });
        row('q', p, d, l, [&](int k) { return static_cast<long>(b.chance[p][d][l][k]); }, [&](int k) { return b.chance[p][d][l][k] >= 100; });
        row('r', p, d, l, [&](int k) { return static_cast<long>(b.hits[p][d][l][k]); }, [&](int k) { return b.hits[p][d][l][k] <= 1; });
      }
  int len = 0;
  for (int k = 0; k < kChain; ++k)
    if (b.chain[k] >= 0) len = k + 1;
  if (len > 0) {
    out += 'n';
    for (int k = 0; k < len; ++k) std::snprintf(buf, sizeof buf, " %d", b.chain[k] + 1), out += buf;
    out += '\n';
  }
  return out.empty() ? out : "TR1\n" + out;
}

uint32_t noteChannel(const std::string& data) { return tideline::noteSourceChannel(data); }
std::string withNoteChannel(const std::string& data, uint32_t channel) { return tideline::withNoteSourceChannel(data, channel); }

std::string withBank(const std::string& data, const Bank& b) {
  const uint32_t c = noteChannel(data);
  return withNoteChannel(write(b), c);
}

// ------------------------------------------------------------------ sources

bool linkLevelTrack(Document& d, ObjectId owner, int slot, ObjectId from) {
  const Song& s = d.song();
  if (!isTerrace(s, owner, slot) || from == owner || !s.mixerTrack(from)) return false;
  bool sc = false;
  const bool exists = hasSend(s, from, owner, &sc);
  if (!exists && !canRoute(s, from, owner)) return false;
  d.beginGroup("Terrace sidechain from " + mixerTrackName(s, static_cast<size_t>(s.mixerIndex(from))));
  int scIn = -1;
  if (!exists) {
    if (!d.setSend(from, owner, 1.0f) || !d.setSendSidechain(from, owner, true)) {
      d.cancelGroup();
      return false;
    }
  } else if (!sc) {  // already mixed into this track: read that route (its input route number)
    const std::vector<int> ins = flRouteInputs(d.song(), d.song().mixerIndex(owner));
    const auto it = std::find(ins.begin(), ins.end(), d.song().mixerIndex(from));
    scIn = it == ins.end() ? -1 : static_cast<int>(it - ins.begin());
  }
  setP(d, owner, slot, TrScIn, scIn);
  if (std::lround(getP(d.song(), owner, slot, TrScListen)) == LsNotes) setP(d, owner, slot, TrScListen, LsLevel);
  d.endGroup();
  return true;
}

bool linkChannel(Document& d, ObjectId owner, int slot, ObjectId channel) {
  const Song& s = d.song();
  const Channel* c = s.channel(channel);
  if (!c || !isTerrace(s, owner, slot)) return false;
  const int listen = static_cast<int>(std::lround(getP(s, owner, slot, TrScListen)));
  d.beginGroup("Terrace sidechain from " + c->name);
  if (listen != LsNotes) {  // the level: the channel's track routed in (moved to a free track of its own when needed)
    ObjectId from = tideline::channelTrack(d.song(), channel);
    if (!tideline::channelKeysDirectly(d.song(), channel, owner)) {
      const std::vector<ObjectId> used = ops::routeToFreeMixerTracks(d, {channel});
      from = used.empty() ? kNoId : used.front();
    }
    const int keep = listen;
    if (from == kNoId || !linkLevelTrack(d, owner, slot, from)) {
      d.cancelGroup();
      return false;
    }
    if (keep == LsBoth) setP(d, owner, slot, TrScListen, LsBoth);
  }
  if (listen != LsLevel) {  // the notes
    d.editMixerTrack(owner, "Terrace notes from " + c->name, [&](MixerTrack& t) {
      PluginState& p = t.slots[static_cast<size_t>(slot)].plugin;
      normalizePluginState(p);
      p.data = withNoteChannel(p.data, channel);
      if (p.params.size() >= static_cast<size_t>(TrCount)) p.params[TrNoteSrc] = NsChannel;
    });
  }
  d.endGroup();
  return true;
}

bool setNoteSource(Document& d, ObjectId owner, int slot, int src, int port) {
  if (!isTerrace(d.song(), owner, slot) || src == NsChannel) return false;
  return d.editMixerTrack(owner, src == NsInput ? "Terrace notes from the MIDI input" : "Terrace notes from MIDI Out ports", [&](MixerTrack& t) {
    PluginState& p = t.slots[static_cast<size_t>(slot)].plugin;
    normalizePluginState(p);
    p.data = withNoteChannel(p.data, 0);
    if (p.params.size() >= static_cast<size_t>(TrCount)) {
      p.params[TrNoteSrc] = static_cast<float>(src == NsInput ? NsInput : NsPorts);
      if (src != NsInput) p.params[TrNotePort] = static_cast<float>(std::clamp(port, -1, 255));
      if (std::lround(p.params[TrScListen]) == LsLevel) p.params[TrScListen] = LsNotes;
    }
  });
}

bool clearLevelSource(Document& d, ObjectId owner, int slot) {
  if (!isTerrace(d.song(), owner, slot)) return false;
  setP(d, owner, slot, TrScIn, 0);
  return true;
}

// ------------------------------------------------------------------ catalogue

void appendInfos(std::vector<PluginInfo>& v) {
  PluginInfo p{kType, "Terrace", "Dynamics", PluginKind::Effect, {}, false, {}};
  p.params.resize(TrCount);
  p.params[TrBands] = ch("Bands", {"3", "4"}, 1);
  p.params[TrSplit1] = lg("Split 1", "Hz", 20, 20000, 120);
  p.params[TrSplit2] = lg("Split 2", "Hz", 20, 20000, 900);
  p.params[TrSplit3] = lg("Split 3", "Hz", 20, 20000, 5000);
  p.params[TrSlope] = ch("Slope", {"12 dB", "24 dB", "48 dB"}, Slope24);
  p.params[TrLength] = ch("Length", {"1/2 bar", "1 bar", "2 bars", "4 bars"}, 1);
  p.params[TrSmooth] = lg("Smoothing", "ms", 0.5, 50, 3);
  p.params[TrOutput] = lin("Output", "dB", -24, 12, 0);
  p.params[TrMix] = lin("Mix", "%", 0, 1, 1);
  p.params[TrPattern] = ch("Pattern", {"1", "2", "3", "4", "5", "6", "7", "8"}, 0);
  p.params[TrChain] = tog("Pattern chain", 0);
  std::vector<std::string> steps, sizes(kSizeNames, kSizeNames + pp::kShaperRates);
  for (int k = 1; k <= kSteps; ++k) steps.push_back(std::to_string(k));
  for (int b = 0; b < kBands; ++b) {
    const std::string bn = "Band " + std::to_string(b + 1) + " ";
    for (int l = 0; l < kLanes; ++l) {
      const std::string ln = bn + kLaneWords[l];
      p.params[static_cast<size_t>(laneParam(b, l, LpSteps))] = ch(ln + " steps", steps, 15);
      p.params[static_cast<size_t>(laneParam(b, l, LpSize))] = ch(ln + " step size", sizes, 3);
      p.params[static_cast<size_t>(laneParam(b, l, LpSwing))] = lin(ln + " swing", "%", 0, 1, 0);
      p.params[static_cast<size_t>(laneParam(b, l, LpDepth))] = lin(ln + " depth", "%", 0, 1, 1);
      p.params[static_cast<size_t>(laneParam(b, l, LpCurve))] = lin(ln + " curve", "%", -1, 1, 0);
    }
    p.params[static_cast<size_t>(bandParam(b, BpSolo))] = tog(bn + "solo", 0);
    p.params[static_cast<size_t>(bandParam(b, BpMute))] = tog(bn + "mute", 0);
    p.params[static_cast<size_t>(bandParam(b, BpGain))] = lin(bn + "gain", "dB", -24, 12, 0);
  }
  const std::vector<std::string> rates(kLfoRateNames, kLfoRateNames + kLfoRates);
  for (int i = 0; i < kLfos; ++i) {
    const std::string n = "LFO " + std::to_string(i + 1) + " ";
    p.params[static_cast<size_t>(lfoParam(i, LfSync))] = tog(n + "sync", 1);
    p.params[static_cast<size_t>(lfoParam(i, LfRate))] = ch(n + "rate", rates, i == 0 ? 9 : 5);
    p.params[static_cast<size_t>(lfoParam(i, LfHz))] = lg(n + "speed", "Hz", 0.01, 40, 1);
    p.params[static_cast<size_t>(lfoParam(i, LfShape))] = ch(n + "shape", {"Sine", "Triangle", "Saw up", "Saw down", "Square", "Random", "Smooth random"}, i == 0 ? 0 : 1);
    p.params[static_cast<size_t>(lfoParam(i, LfPhase))] = lin(n + "phase", "deg", 0, 360, 0);
    p.params[static_cast<size_t>(lfoParam(i, LfAmount))] = lin(n + "amount", "%", -1, 1, 1);
    p.params[static_cast<size_t>(lfoParam(i, LfRetrig))] = ch(n + "restart", {"Free", "Pattern"}, 0);
  }
  p.params[TrFolSource] = ch("Follower source", {"Input", "Band 1", "Band 2", "Band 3", "Band 4"}, 0);
  p.params[TrFolAttack] = lg("Follower attack", "ms", 0.1, 500, 10);
  p.params[TrFolRelease] = lg("Follower release", "ms", 1, 2000, 150);
  p.params[TrFolGain] = lin("Follower gain", "dB", -12, 24, 0);
  p.params[TrFolAmount] = lin("Follower amount", "%", -1, 1, 1);
  p.params[TrScIn] = lin("Sidechain input", "", -1, 500, -1);
  p.params[TrScListen] = ch("Sidechain listens to", {"Level", "Notes", "Both"}, LsLevel);
  p.params[TrScThreshold] = lin("Sidechain threshold", "dB", -60, 0, -24);
  p.params[TrScRelease] = lg("Sidechain release", "ms", 5, 2000, 80);
  p.params[TrScAmount] = lin("Sidechain amount", "%", -1, 1, 1);
  p.params[TrScRetrig] = tog("Notes restart the lanes", 0);
  p.params[TrNoteSrc] = ch("Note source", {"MIDI Out ports", "Channel", "MIDI input"}, NsChannel);
  p.params[TrNotePort] = lin("Note port", "", -1, 255, -1);
  p.params[TrNotePatterns] = tog("Notes pick patterns", 0);
  p.params[TrNoteBase] = lin("Pattern notes from", "note", 0, 120, 48);
  std::vector<std::string> srcs = {"Off", "LFO 1", "LFO 2", "Follower", "Sidechain"}, tgts;
  for (int k = 0; k < TgCount; ++k) {
    if (k > TgNone && k < TgSplit1) {  // the catalogue names the bands by number (their names follow the band count)
      static const char* const w[TbCount] = {"volume depth", "cutoff depth", "pan depth", "step size", "swing", "gain", "cutoff", "pan"};
      tgts.push_back("Band " + std::to_string((k - TgBand0) / TbCount + 1) + " " + w[(k - TgBand0) % TbCount]);
    } else {
      tgts.push_back(targetName(k, 4));
    }
  }
  for (int r = 0; r < kRoutes; ++r) {
    const std::string n = "Route " + std::to_string(r + 1) + " ";
    p.params[static_cast<size_t>(routeParam(r, RtSource))] = ch(n + "source", srcs, SrcOff);
    p.params[static_cast<size_t>(routeParam(r, RtTarget))] = ch(n + "target", tgts, TgNone);
    p.params[static_cast<size_t>(routeParam(r, RtAmount))] = lin(n + "amount", "%", -1, 1, 0);
  }
  p.sideInParam = TrScIn;
  p.editorWidth = 1240;
  p.editorFullSize = true;  // opens at 100 % (its type sizes are set for it), smaller only on a small screen
  p.ownPresetBar = true;
  p.presets = factoryPresets();
  v.push_back(std::move(p));
}

}  // namespace gl::packs::terrace
