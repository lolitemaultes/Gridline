// Gridline - "Arrange by sound..." on the Stage (see StageArrange.h).
#include "StageArrange.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include "I18n.h"

namespace gl::stage {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

const char* roleName(Role r) {
  switch (r) {
    case Role::LeadVocal: return i18n::N_("lead vocal");
    case Role::BackingVocal: return i18n::N_("backing vocal");
    case Role::Kick: return i18n::N_("kick");
    case Role::Bass: return i18n::N_("bass");
    case Role::Snare: return i18n::N_("snare");
    case Role::Clap: return i18n::N_("clap");
    case Role::Hats: return i18n::N_("hats");
    case Role::Perc: return i18n::N_("percussion");
    case Role::Drums: return i18n::N_("drums");
    case Role::Keys: return i18n::N_("keys");
    case Role::Guitar: return i18n::N_("guitar");
    case Role::Lead: return i18n::N_("lead");
    case Role::Pad: return i18n::N_("pad");
    case Role::Strings: return i18n::N_("strings");
    case Role::Brass: return i18n::N_("brass");
    case Role::Fx: return i18n::N_("effects");
    case Role::Loop: return i18n::N_("loop");
    default: return "?";
  }
}

// ---------------------------------------------------------------- the sound
void FeatureAcc::reset(double sampleRate) {
  *this = FeatureAcc{};
  sr_ = sampleRate > 0 ? sampleRate : 48000;
}

void FeatureAcc::add(const float* x, int n) {
  const double aLow = std::exp(-2.0 * kPi * 150.0 / sr_);
  const double aFast = std::exp(-1.0 / (0.005 * sr_)), aSlow = std::exp(-1.0 / (0.15 * sr_));
  for (int i = 0; i < n; ++i) {
    const double v = static_cast<double>(x[i]);
    ++frames_;
    const double a = std::fabs(v);
    peak_ = std::max(peak_, a);
    envFast_ = std::max(a, envFast_ * aFast);
    envSlow_ = envSlow_ * aSlow + (1.0 - aSlow) * envFast_;
    if (envFast_ > 1e-3) {  // only sound counts (rests and silence say nothing)
      ++loud_;
      sumSq_ += v * v;
      sumDiffSq_ += (v - prev_) * (v - prev_);
      low_ = low_ * aLow + (1.0 - aLow) * v;
      sumLowSq_ += low_ * low_;
      envSum_ += envFast_;
      envPeak_ = std::max(envPeak_, envFast_);
    }
    ++since_;
    if (armed_ && envFast_ > 2.0 * envSlow_ + 3e-3 && static_cast<double>(since_) >= 0.1 * sr_) {  // at most ten a second
      ++onsets_;
      armed_ = false;
      since_ = 0;
    } else if (!armed_ && envFast_ < 1.2 * envSlow_) {
      armed_ = true;
    }
    prev_ = v;
  }
}

Features FeatureAcc::result() const {
  Features f;
  f.seconds = static_cast<double>(loud_) / sr_;
  f.peak = peak_;
  if (sumSq_ > 0) {
    const double ratio = std::sqrt(sumDiffSq_ / sumSq_);  // 2 sin(pi f / sr) for a sine
    f.centroid = sr_ / kPi * std::asin(std::min(1.0, ratio * 0.5));
    f.lowShare = std::clamp(sumLowSq_ / sumSq_, 0.0, 1.0);
  }
  f.onsets = f.seconds > 0 ? static_cast<double>(onsets_) / f.seconds : 0;
  f.sustain = envPeak_ > 0 && loud_ > 0 ? std::clamp(envSum_ / static_cast<double>(loud_) / envPeak_, 0.0, 1.0) : 0;
  return f;
}

Role roleFromSound(const Features& f, std::string* why) {
  auto say = [&](const char* w) {
    if (why) *why = w;
  };
  if (!f.enough()) {
    say(i18n::N_("too little sound to tell"));
    return Role::Unknown;
  }
  const double c = f.centroid;
  if (c < 260 && f.onsets >= 0.7 && f.sustain < 0.45) return say(i18n::N_("low and punchy")), Role::Kick;
  if (c < 420 && f.lowShare > 0.3 && f.sustain >= 0.3) return say(i18n::N_("low and steady")), Role::Bass;
  if (c > 5000 && f.sustain < 0.5) return say(i18n::N_("bright and short")), Role::Hats;
  if (c >= 1000 && c <= 5000 && f.onsets >= 0.7 && f.sustain < 0.3) return say(i18n::N_("short with a crack")), Role::Snare;
  if (f.sustain >= 0.55 && f.onsets < 0.7 && c >= 150 && c <= 3000) return say(f.width > 0.3 ? i18n::N_("long, soft and wide") : i18n::N_("long and soft")), Role::Pad;
  if (c > 3000 && f.sustain >= 0.5) return say(i18n::N_("a noisy sweep")), Role::Fx;
  if (c >= 600 && c <= 3500 && f.onsets >= 0.8 && f.sustain >= 0.2 && f.sustain < 0.55) return say(i18n::N_("plucked mid-range")), Role::Guitar;
  if (c >= 300 && c <= 3000 && f.sustain >= 0.3 && f.sustain < 0.75 && f.lowShare < 0.2 && f.width < 0.4)
    return say(i18n::N_("mid-range and voice-like")), Role::LeadVocal;
  say(i18n::N_("mixed or unclear"));
  return Role::Unknown;
}

// ---------------------------------------------------------------- what plays, the name
namespace {
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool word(const std::string& s, const char* w) {  // `w` at a word start in `s` (letters before it end a word)
  for (size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1))
    if (p == 0 || !std::isalpha(static_cast<unsigned char>(s[p - 1]))) return true;
  return false;
}
struct Key {
  const char* w;
  Role r;
};
// Words in names, sample files and presets. Longer and more specific words first.
const Key kWords[] = {
    {"backing", Role::BackingVocal}, {"bvox", Role::BackingVocal}, {"bgv", Role::BackingVocal}, {"choir", Role::BackingVocal},
    {"harmony", Role::BackingVocal}, {"vocal", Role::LeadVocal}, {"vox", Role::LeadVocal}, {"voice", Role::LeadVocal}, {"vocoder", Role::Lead},
    {"kick", Role::Kick}, {"bd", Role::Kick}, {"808", Role::Bass}, {"sub", Role::Bass}, {"bass", Role::Bass},
    {"snare", Role::Snare}, {"snr", Role::Snare}, {"rim", Role::Snare}, {"clap", Role::Clap}, {"snap", Role::Clap},
    {"hat", Role::Hats}, {"hh", Role::Hats}, {"cymbal", Role::Hats}, {"ride", Role::Hats}, {"crash", Role::Hats}, {"shaker", Role::Perc},
    {"perc", Role::Perc}, {"conga", Role::Perc}, {"bongo", Role::Perc}, {"tom", Role::Perc}, {"tamb", Role::Perc},
    {"drum", Role::Drums}, {"kit", Role::Drums}, {"beat", Role::Drums}, {"break", Role::Loop}, {"loop", Role::Loop},
    {"piano", Role::Keys}, {"keys", Role::Keys}, {"rhodes", Role::Keys}, {"organ", Role::Keys}, {"epiano", Role::Keys}, {"ep ", Role::Keys},
    {"guitar", Role::Guitar}, {"gtr", Role::Guitar}, {"pluck", Role::Guitar}, {"harp", Role::Guitar},
    {"pad", Role::Pad}, {"atmo", Role::Pad}, {"drone", Role::Pad}, {"string", Role::Strings}, {"violin", Role::Strings},
    {"cello", Role::Strings}, {"brass", Role::Brass}, {"horn", Role::Brass}, {"trumpet", Role::Brass}, {"sax", Role::Brass},
    {"lead", Role::Lead}, {"synth", Role::Lead}, {"arp", Role::Lead}, {"fx", Role::Fx}, {"riser", Role::Fx}, {"sweep", Role::Fx},
    {"impact", Role::Fx}, {"noise", Role::Fx}, {"foley", Role::Fx}};

Role fromWords(const std::string& text) {
  const std::string s = lower(text);
  for (const Key& k : kWords)
    if (word(s, k.w)) return k.r;
  return Role::Unknown;
}

// Built-in instruments that say what they play (the rest depend on their sound).
const std::map<std::string, Role>& typeRoles() {
  static const std::map<std::string, Role> m = {
      {"gl.kick", Role::Kick}, {"gl.kicksculpt", Role::Kick}, {"gl.sweepkick", Role::Kick}, {"gl.snare", Role::Snare},
      {"gl.hat", Role::Hats}, {"gl.clap", Role::Clap}, {"gl.pads", Role::Drums}, {"gl.drumkit", Role::Drums},
      {"gl.membranekit", Role::Drums}, {"gl.membranepad", Role::Perc}, {"gl.slicer", Role::Loop}, {"gl.keys", Role::Keys},
      {"gl.samplekeys", Role::Keys}, {"gl.acid", Role::Bass}, {"gl.fingerbass", Role::Bass}, {"gl.stepbass", Role::Bass},
      {"gl.pluck", Role::Guitar}, {"gl.plucker", Role::Guitar}, {"gl.twinstring", Role::Strings}, {"gl.string", Role::Strings}};
  return m;
}
}  // namespace

Role roleFromPlays(const std::vector<std::string>& types, const std::vector<std::string>& presetWords) {
  Role found = Role::Unknown;
  bool mixed = false;
  auto take = [&](Role r) {
    if (r == Role::Unknown) return;
    if (found == Role::Unknown) found = r;
    else if (found != r) mixed = true;
  };
  for (const std::string& t : types)
    if (auto it = typeRoles().find(t); it != typeRoles().end()) take(it->second);
  if (found == Role::Unknown)
    for (const std::string& w : presetWords) take(fromWords(w));
  if (mixed) {  // several kinds of drums on one track: drums; anything else mixed: unsure
    const auto drum = [](Role r) { return r == Role::Kick || r == Role::Snare || r == Role::Clap || r == Role::Hats || r == Role::Perc || r == Role::Drums; };
    bool allDrums = true;
    for (const std::string& t : types)
      if (auto it = typeRoles().find(t); it != typeRoles().end()) allDrums = allDrums && drum(it->second);
    return allDrums && drum(found) ? Role::Drums : Role::Unknown;
  }
  return found;
}

Role roleFromName(const std::string& name) { return fromWords(name); }

Guess guessRole(const std::vector<std::string>& types, const std::vector<std::string>& presetWords, const Features* f, const std::string& name) {
  Guess g;
  if (const Role r = roleFromPlays(types, presetWords); r != Role::Unknown) {
    g.role = r, g.source = 1, g.why = i18n::N_("from the instrument on it");
    return g;
  }
  if (f) {
    std::string why;
    if (const Role r = roleFromSound(*f, &why); r != Role::Unknown) {
      g.role = r, g.source = 2, g.why = why;
      return g;
    }
  }
  if (const Role r = roleFromName(name); r != Role::Unknown) {
    g.role = r, g.source = 3, g.why = i18n::N_("from its name");
    return g;
  }
  g.why = i18n::N_("not sure: place it yourself");
  return g;
}

// ---------------------------------------------------------------- where each role stands
namespace {
struct Slot {
  double pan, depth;
};
// Each role's places, the first one first; more tracks of a role take the next.
std::vector<Slot> slotsOf(Role r) {
  switch (r) {
    case Role::LeadVocal: return {{0, 0}, {-0.15, 0}, {0.15, 0}};
    case Role::Kick: return {{-0.1, 0.25}, {0.1, 0.25}};
    case Role::Bass: return {{0.12, 0.25}, {-0.12, 0.25}};
    case Role::Drums: return {{0, 0.25}, {-0.2, 0.25}, {0.2, 0.25}};
    case Role::Snare: return {{-0.3, 0.5}, {0.3, 0.5}};
    case Role::Clap: return {{0, 0.5}, {-0.15, 0.5}};
    case Role::Hats: return {{0.3, 0.5}, {-0.45, 0.5}};
    case Role::Perc: return {{0.55, 0.5}, {-0.55, 0.5}, {0.7, 0.5}};
    case Role::Loop: return {{0, 0.5}, {-0.2, 0.5}};
    case Role::Keys: return {{-0.6, 0.25}, {0.6, 0.25}};
    case Role::Guitar: return {{0.6, 0.25}, {-0.6, 0.25}, {0.8, 0.25}};
    case Role::Lead: return {{0.25, 0.25}, {-0.25, 0.25}};
    case Role::Pad: return {{0, 0.75}, {-0.4, 0.75}, {0.4, 0.75}};
    case Role::Strings: return {{-0.7, 0.75}, {0.7, 0.75}};
    case Role::BackingVocal: return {{0.6, 0.75}, {-0.6, 0.75}};
    case Role::Brass: return {{0.45, 0.75}, {-0.45, 0.75}};
    case Role::Fx: return {{0.4, 1.0}, {-0.4, 1.0}, {0, 1.0}};
    default: return {{0, 0.5}, {-0.15, 0.5}, {0.15, 0.5}};
  }
}
}  // namespace

std::vector<Pos> arrangeByRole(const std::vector<Role>& roles) {
  std::vector<Pos> out(roles.size());
  std::map<int, int> used;
  std::vector<Pos> taken;
  for (size_t i = 0; i < roles.size(); ++i) {
    const std::vector<Slot> slots = slotsOf(roles[i]);
    const int k = used[static_cast<int>(roles[i])]++;
    Slot s = slots[static_cast<size_t>(k) % slots.size()];
    if (k >= static_cast<int>(slots.size()))  // more of a role than it has places: further out, alternating sides
      s.pan = std::clamp(s.pan + (k % 2 ? -1.0 : 1.0) * 0.15 * (k / static_cast<int>(slots.size())), -1.0, 1.0);
    // Never two tracks on one point unless they share a spot on purpose: step aside.
    for (int guard = 0; guard < 40; ++guard) {
      bool clash = false;
      for (const Pos& p : taken) clash = clash || (std::fabs(p.pan - s.pan) < 0.06 && std::fabs(p.depth - s.depth) < 0.05);
      if (!clash) break;
      s.pan = std::clamp(s.pan + (s.pan >= 0 ? 0.12 : -0.12), -1.0, 1.0);
      if (std::fabs(s.pan) >= 1.0) s.pan = -s.pan * 0.5, s.depth = std::min(1.0, s.depth + 0.12);
    }
    out[i].pan = s.pan, out[i].depth = s.depth;
    taken.push_back(out[i]);
  }
  return out;
}

int roleIcon(Role r) {
  switch (r) {
    case Role::Kick: return 1;
    case Role::Snare: return 2;
    case Role::Hats: return 3;
    case Role::Perc: return 4;
    case Role::Bass: return 5;
    case Role::Keys: return 6;
    case Role::Lead: return 7;
    case Role::Pad: return 8;
    case Role::Guitar: return 9;
    case Role::LeadVocal: return 10;
    case Role::Fx: return 11;
    case Role::Clap: return 18;
    case Role::Drums: return 20;
    case Role::Strings: return 43;
    case Role::Brass: return 45;
    case Role::BackingVocal: return 49;
    case Role::Loop: return 54;
    default: return 0;
  }
}

const Preset& preset(int k) {
  static const Preset kPresets[kPresetCount] = {
      {i18n::N_("Band"), i18n::N_("The singer in front, the band behind"), 2, 0.5},
      {i18n::N_("Electronic"), i18n::N_("Kick and bass up front, pads wide at the back"), 3, 0.3},
      {i18n::N_("Wide pop"), i18n::N_("Doubled parts mirrored left and right"), 8, 0.5},
      {i18n::N_("Podcast"), i18n::N_("Voices close, music bed further back"), 1, 0.5},
      {i18n::N_("Chamber"), i18n::N_("Strings left, brass right, in a hall"), 5, 0.65},
      {i18n::N_("Lo-fi room"), i18n::N_("Everything close, narrow and small"), 1, 1.0},
  };
  return kPresets[std::clamp(k, 0, kPresetCount - 1)];
}

std::vector<Pos> arrangePreset(int k, const std::vector<Role>& roles) {
  std::vector<Pos> out = arrangeByRole(roles);
  auto drum = [](Role r) { return r == Role::Kick || r == Role::Snare || r == Role::Clap || r == Role::Hats || r == Role::Perc || r == Role::Drums; };
  auto voice = [](Role r) { return r == Role::LeadVocal || r == Role::BackingVocal; };
  std::map<int, int> seen;
  for (size_t i = 0; i < out.size(); ++i) {
    Pos& p = out[i];
    const Role r = roles[i];
    const int n = seen[static_cast<int>(r)]++;
    switch (k) {
      case 1:  // Electronic: kick and bass dry up front, the rest spread, pads and effects wide at the back
        if (r == Role::Kick || r == Role::Bass) p.pan = (n % 2 ? -0.08 : 0.08) * n, p.depth = r == Role::Kick ? 0.0 : 0.25;
        else if (r == Role::Lead || r == Role::LeadVocal) p.depth = 0.25;
        else if (r == Role::Hats || r == Role::Perc) p.pan *= 1.4;
        else if (r == Role::Pad || r == Role::Fx) p.pan = (p.pan == 0 ? (n % 2 ? -0.5 : 0.5) : p.pan * 1.5), p.depth = 1.0;
        break;
      case 2:  // Wide pop: the singer centre front, the rest wider, doubles mirrored
        if (r != Role::LeadVocal && r != Role::Kick && r != Role::Bass) p.pan *= 1.35;
        break;
      case 3:  // Podcast: voices close either side of the middle, everything else a bed at the back
        if (voice(r)) p.pan = (n % 2 ? 0.25 : -0.25) * (1 + n / 2), p.depth = 0.0;
        else p.pan *= 1.2, p.depth = 0.75;
        break;
      case 4:  // Chamber: strings left, brass right at the back, pads centre back, keys centre near
        if (r == Role::Strings) p.pan = -0.6 - 0.15 * n, p.depth = 0.5;
        else if (r == Role::Brass) p.pan = 0.55 + 0.15 * n, p.depth = 0.75;
        else if (r == Role::Keys) p.pan = (n % 2 ? 0.2 : -0.2) * n, p.depth = 0.25;
        else if (r == Role::Pad || r == Role::Lead) p.pan *= 0.5, p.depth = 0.75;
        else if (drum(r)) p.depth = std::max(p.depth, 0.75);
        break;
      case 5:  // Lo-fi room: narrow and close
        p.pan *= 0.55, p.depth *= 0.5;
        break;
      default: break;  // Band: the role places as they are
    }
    p.pan = std::clamp(p.pan, -1.0, 1.0);
    p.depth = std::clamp(snapDepth(p.depth), 0.0, 1.0);
  }
  // Never two tracks on one point: step aside along the row.
  for (size_t i = 0; i < out.size(); ++i)
    for (int guard = 0; guard < 40; ++guard) {
      bool clash = false;
      for (size_t j = 0; j < i; ++j) clash = clash || (std::fabs(out[j].pan - out[i].pan) < 0.06 && std::fabs(out[j].depth - out[i].depth) < 0.05);
      if (!clash) break;
      out[i].pan = std::clamp(out[i].pan + (out[i].pan >= 0 ? 0.12 : -0.12), -1.0, 1.0);
      if (std::fabs(out[i].pan) >= 1.0) out[i].pan = -out[i].pan * 0.5, out[i].depth = std::min(1.0, out[i].depth + 0.25);
    }
  return out;
}

}  // namespace gl::stage
