// Gridline — J-53: FLEX stand-ins (see FlexStandin.h). Everything here is
// Gridline's own: the keyword table, the templates and the control laws.
// Nothing is derived from FLEX's pack content (which Gridline never reads).
#include "FlexStandin.h"
#include "Platform.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../packs/models/models.h"
#include "../packs/models/strings.h"
#include "../packs/synths/synths.h"
#include "PluginBrand.h"
#include "PluginInfo.h"
#include "PluginParams.h"

namespace gl::flex {

namespace sy = gl::packs::synths;
namespace md = gl::packs::models;

// ------------------------------------------------------------------ names

const char* catName(int cat) {
  static const char* const n[CatCount] = {"Auto",  "Bass",   "Sub bass", "Lead",   "Pad",   "Keys",   "Electric piano",
                                          "Pluck", "Bell",   "Strings",  "Brass",  "Choir", "Stab",   "Arp",
                                          "Guitar", "Organ", "FX",       "Drums",  "Neutral"};
  return cat >= 0 && cat < CatCount ? n[cat] : "Neutral";
}

const std::vector<std::string>& catChoices() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> r;
    for (int c = 0; c < CatCount; ++c) r.push_back(catName(c));
    return r;
  }();
  return v;
}

const char* engineType(int e) {
  static const char* const t[EngineCount] = {"gl.analog", "gl.keys", "gl.fm", "gl.drumkit", "gl.plucker"};
  return e >= 0 && e < EngineCount ? t[e] : "gl.analog";
}
const char* engineName(int e) {
  // J-94: the catalogue's display name (stable pointer: the catalogue never changes).
  const PluginInfo* info = findPlugin(engineType(e));
  return info ? info->name.c_str() : engineType(e);
}

int engineOf(int cat) {
  switch (cat) {
    case Keys: case EPiano: return EKeys;
    case Bell: return EFm;
    case Drums: return EDrums;
    case Guitar: return EPlucker;
    default: return EAnalog;
  }
}

// ------------------------------------------------------------ classifier

namespace {

// Words of a name: lower case, split at non-alphanumerics, at letter / digit
// boundaries and at lower → upper case steps ("SynPluck" → syn, pluck).
std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  std::string w;
  auto flush = [&] {
    if (!w.empty()) out.push_back(w), w.clear();
  };
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(s[i]);
    if (!std::isalnum(ch)) {
      flush();
      continue;
    }
    if (!w.empty()) {
      const unsigned char prev = static_cast<unsigned char>(s[i - 1]);
      const bool digitStep = std::isdigit(ch) != std::isdigit(prev);
      const bool caseStep = std::isupper(ch) && std::islower(prev);
      if (digitStep || caseStep) flush();
    }
    w += static_cast<char>(std::tolower(ch));
  }
  flush();
  return out;
}

enum Match : uint8_t { Whole, Prefix, Inside };
struct Rule {
  int cat;
  Match m;
  const char* w;
};

// Tier 0: producer shorthand tags (whole words only). They name the category
// outright, so they win over instrument words ("SFX Crash" is an effect).
const Rule kTags[] = {
    {Bass, Whole, "bas"},   {Bass, Whole, "bs"},     {Bass, Whole, "bss"},    {SubBass, Whole, "sub"},  {SubBass, Whole, "808"},
    {SubBass, Whole, "808s"}, {Lead, Whole, "ld"},   {Lead, Whole, "led"},    {Lead, Whole, "lds"},     {Lead, Whole, "ldr"},
    {Pad, Whole, "pd"},     {Pad, Whole, "pad"},     {Pad, Whole, "pads"},    {Keys, Whole, "key"},     {Keys, Whole, "keys"},
    {Keys, Whole, "kys"},   {Keys, Whole, "kbd"},    {EPiano, Whole, "ep"},   {Pluck, Whole, "plk"},    {Pluck, Whole, "plck"},
    {Bell, Whole, "bel"},   {Bell, Whole, "bll"},    {Strings, Whole, "str"}, {Strings, Whole, "strs"}, {Strings, Whole, "stg"},
    {Brass, Whole, "brs"},  {Choir, Whole, "vox"},   {Choir, Whole, "vcl"},   {Choir, Whole, "vo"},     {Stab, Whole, "stb"},
    {Stab, Whole, "chd"},   {Stab, Whole, "chrd"},   {Arp, Whole, "arp"},     {Arp, Whole, "arps"},     {Arp, Whole, "seq"},
    {Arp, Whole, "sq"},     {Guitar, Whole, "gtr"},  {Guitar, Whole, "gtrs"}, {Organ, Whole, "org"},    {Organ, Whole, "orgn"},
    {Fx, Whole, "sfx"},     {Fx, Whole, "fx"},       {Drums, Whole, "drm"},   {Drums, Whole, "drms"},   {Drums, Whole, "prc"},
    {Drums, Whole, "perc"}, {Drums, Whole, "kit"},   {Drums, Whole, "kik"},   {Drums, Whole, "snr"},    {Drums, Whole, "hh"},
};

// Tier 1: instrument words (EN, DE, FR, ES, IT, PT, NL), most specific first.
const Rule kWords[] = {
    // percussion: kits and single pieces
    {Drums, Inside, "drum"},       {Drums, Prefix, "kits"},      {Drums, Prefix, "percu"},     {Drums, Prefix, "kick"},
    {Drums, Inside, "snare"},      {Drums, Whole, "hat"},        {Drums, Whole, "hats"},       {Drums, Inside, "hihat"},
    {Drums, Prefix, "clap"},       {Drums, Prefix, "cymbal"},    {Drums, Prefix, "crash"},     {Drums, Whole, "ride"},
    {Drums, Whole, "tom"},         {Drums, Whole, "toms"},       {Drums, Prefix, "conga"},     {Drums, Prefix, "bongo"},
    {Drums, Prefix, "cowbell"},    {Drums, Prefix, "agogo"},     {Drums, Prefix, "clave"},     {Drums, Prefix, "shaker"},
    {Drums, Prefix, "tambo"},      {Drums, Prefix, "tamb"},      {Drums, Prefix, "timbal"},    {Drums, Prefix, "timpan"},
    {Drums, Prefix, "tabla"},      {Drums, Prefix, "djemb"},     {Drums, Prefix, "cajon"},     {Drums, Prefix, "woodblock"},
    {Drums, Prefix, "triangle"},   {Drums, Prefix, "rimshot"},   {Drums, Prefix, "breakbeat"}, {Drums, Prefix, "schlag"},
    {Drums, Prefix, "batteri"},    {Drums, Prefix, "bateria"},   {Drums, Prefix, "trommel"},   {Drums, Prefix, "tambor"},
    {Drums, Prefix, "caisse"},     {Drums, Prefix, "tamburo"},   {Drums, Prefix, "trom"},      {Drums, Whole, "caja"},
    // sub / 808 basses
    {SubBass, Inside, "subbass"},  {SubBass, Prefix, "subbas"},  {SubBass, Inside, "808"},     {SubBass, Prefix, "sublow"},
    // electric pianos (also "electric piano", "e piano" pairs, see classifyWords)
    {EPiano, Prefix, "rhodes"},    {EPiano, Prefix, "wurli"},    {EPiano, Inside, "epiano"},   {EPiano, Prefix, "tine"},
    {EPiano, Prefix, "suitcase"},  {EPiano, Whole, "dx7"},       {EPiano, Prefix, "elpiano"},
    // pianos and keyboards
    {Keys, Inside, "piano"},       {Keys, Prefix, "klavier"},    {Keys, Prefix, "keyboard"},   {Keys, Whole, "grand"},
    {Keys, Prefix, "upright"},     {Keys, Prefix, "clav"},       {Keys, Prefix, "harpsi"},     {Keys, Prefix, "cembal"},
    {Keys, Prefix, "clavecin"},    {Keys, Prefix, "fluegel"},    {Keys, Prefix, "klavecimb"},  {Keys, Prefix, "spinet"},
    // organs
    {Organ, Inside, "organ"},      {Organ, Prefix, "orgel"},     {Organ, Prefix, "orgue"},     {Organ, Prefix, "organo"},
    {Organ, Prefix, "hammond"},    {Organ, Prefix, "tonewheel"}, {Organ, Whole, "b3"},         {Organ, Prefix, "drawbar"},
    {Organ, Prefix, "harmonium"},  {Organ, Prefix, "accordi"},   {Organ, Prefix, "akkordeon"}, {Organ, Prefix, "acordeon"},
    {Organ, Prefix, "fisarmonica"}, {Organ, Prefix, "melodica"}, {Organ, Prefix, "orgao"},
    // bells and mallets
    {Bell, Inside, "bell"},        {Bell, Prefix, "glock"},      {Bell, Prefix, "celest"},     {Bell, Prefix, "marimba"},
    {Bell, Prefix, "vibra"},       {Bell, Prefix, "xylo"},       {Bell, Prefix, "kalimba"},    {Bell, Prefix, "mallet"},
    {Bell, Prefix, "chime"},       {Bell, Prefix, "tink"},       {Bell, Prefix, "musicbox"},   {Bell, Prefix, "carillon"},
    {Bell, Prefix, "glocke"},      {Bell, Prefix, "cloche"},     {Bell, Prefix, "campan"},     {Bell, Prefix, "tubular"},
    {Bell, Prefix, "steelpan"},    {Bell, Prefix, "steeldrum"},  {Bell, Prefix, "gamelan"},    {Bell, Prefix, "sino"},
    {Bell, Prefix, "klokk"},       {Bell, Prefix, "bowl"},
    // guitars and plucked folk instruments
    {Guitar, Prefix, "guitar"},    {Guitar, Prefix, "gitar"},    {Guitar, Prefix, "guitare"},  {Guitar, Prefix, "chitarr"},
    {Guitar, Prefix, "strum"},     {Guitar, Prefix, "banjo"},    {Guitar, Prefix, "mandolin"}, {Guitar, Prefix, "ukulele"},
    {Guitar, Whole, "uke"},        {Guitar, Prefix, "sitar"},    {Guitar, Prefix, "koto"},     {Guitar, Prefix, "shamisen"},
    {Guitar, Prefix, "bouzouk"},   {Guitar, Prefix, "nylon"},    {Guitar, Prefix, "violao"},   {Guitar, Prefix, "oud"},
    // ensemble hits ("Orchestra Hit", "String Hit") are stabs
    {Stab, Whole, "hit"},          {Stab, Whole, "hits"},
    // strings
    {Strings, Prefix, "string"},   {Strings, Prefix, "violin"},  {Strings, Prefix, "viola"},   {Strings, Prefix, "cello"},
    {Strings, Prefix, "violoncel"}, {Strings, Prefix, "contrabass"}, {Strings, Prefix, "orchestr"}, {Strings, Prefix, "ensemble"},
    {Strings, Prefix, "streich"},  {Strings, Prefix, "cordes"},  {Strings, Prefix, "cuerda"},  {Strings, Prefix, "archi"},
    {Strings, Prefix, "violon"},   {Strings, Prefix, "spiccato"}, {Strings, Prefix, "strijk"}, {Strings, Prefix, "cordas"},
    {Strings, Prefix, "orquest"},
    // brass and reeds
    {Brass, Prefix, "brass"},      {Brass, Prefix, "horn"},      {Brass, Prefix, "trumpet"},   {Brass, Prefix, "trombon"},
    {Brass, Prefix, "tuba"},       {Brass, Prefix, "sax"},       {Brass, Prefix, "flugel"},    {Brass, Prefix, "cornet"},
    {Brass, Prefix, "blech"},      {Brass, Prefix, "blaeser"},   {Brass, Prefix, "blaser"},    {Brass, Prefix, "cuivre"},
    {Brass, Prefix, "metales"},    {Brass, Prefix, "ottoni"},    {Brass, Prefix, "tromba"},    {Brass, Prefix, "trompet"},
    {Brass, Prefix, "posaune"},    {Brass, Prefix, "bugle"},     {Brass, Prefix, "fanfar"},    {Brass, Prefix, "metais"},
    {Brass, Prefix, "koper"},
    // choirs and voices
    {Choir, Prefix, "choir"},      {Choir, Whole, "chor"},       {Choir, Prefix, "choral"},    {Choir, Prefix, "chorale"},
    {Choir, Prefix, "vocal"},      {Choir, Prefix, "voice"},     {Choir, Prefix, "voix"},      {Choir, Prefix, "voce"},
    {Choir, Whole, "voz"},         {Choir, Prefix, "vozes"},     {Choir, Prefix, "stimm"},     {Choir, Whole, "coro"},
    {Choir, Prefix, "choeur"},     {Choir, Prefix, "koor"},      {Choir, Prefix, "aah"},       {Choir, Prefix, "ooh"},
    {Choir, Prefix, "vowel"},      {Choir, Prefix, "formant"},   {Choir, Prefix, "singer"},    {Choir, Prefix, "chant"},
    {Choir, Prefix, "gregor"},
    // plucks
    {Pluck, Inside, "pluck"},      {Pluck, Prefix, "pizz"},      {Pluck, Prefix, "harp"},      {Pluck, Prefix, "harfe"},
    {Pluck, Prefix, "harpe"},      {Pluck, Whole, "arpa"},       {Pluck, Prefix, "zupf"},      {Pluck, Prefix, "dulcimer"},
    {Pluck, Prefix, "pizzicato"},  {Pluck, Prefix, "pulsad"},    {Pluck, Prefix, "pizzic"},
    // arpeggios and sequences
    {Arp, Prefix, "arpeg"},        {Arp, Prefix, "arpegg"},      {Arp, Prefix, "sequen"},      {Arp, Prefix, "secuenc"},
    {Arp, Prefix, "ostinat"},      {Arp, Prefix, "trancegate"},  {Arp, Prefix, "gated"},       {Arp, Prefix, "arped"},
    {Arp, Prefix, "rhythmic"},     {Arp, Prefix, "pulsing"},
    // stabs and chords
    {Stab, Prefix, "stab"},        {Stab, Prefix, "chord"},
    {Stab, Prefix, "akkord"},      {Stab, Prefix, "accord"},     {Stab, Prefix, "acorde"},     {Stab, Prefix, "hoover"},
    {Stab, Prefix, "supersaw"},    {Stab, Prefix, "rave"},       {Stab, Prefix, "accordo"},
    // pads
    {Pad, Prefix, "pad"},          {Pad, Prefix, "atmo"},        {Pad, Prefix, "ambien"},      {Pad, Prefix, "drone"},
    {Pad, Prefix, "texture"},      {Pad, Prefix, "swell"},       {Pad, Prefix, "flaech"},      {Pad, Prefix, "flach"},
    {Pad, Prefix, "nappe"},        {Pad, Prefix, "tappeto"},     {Pad, Prefix, "soundscape"},  {Pad, Prefix, "evolv"},
    // leads and woodwinds (before "bass": bassoon)
    {Lead, Prefix, "lead"},        {Lead, Prefix, "solo"},       {Lead, Prefix, "bassoon"},    {Lead, Prefix, "fagott"},
    {Lead, Prefix, "flute"},       {Lead, Prefix, "floete"},     {Lead, Prefix, "flaut"},      {Lead, Prefix, "piccolo"},
    {Lead, Prefix, "recorder"},    {Lead, Prefix, "clarinet"},   {Lead, Prefix, "klarinet"},   {Lead, Prefix, "oboe"},
    {Lead, Prefix, "whistle"},     {Lead, Prefix, "ocarina"},    {Lead, Prefix, "fife"},       {Lead, Prefix, "flaute"},
    {Lead, Prefix, "panflute"},    {Lead, Prefix, "scream"},     {Lead, Prefix, "screech"},
    // basses
    {Bass, Inside, "bass"},        {Bass, Prefix, "basse"},      {Bass, Prefix, "bajo"},       {Bass, Prefix, "baixo"},
    {Bass, Prefix, "basso"},       {Bass, Prefix, "reese"},      {Bass, Prefix, "wobble"},     {Bass, Whole, "wob"},
    {Bass, Prefix, "acid"},        {Bass, Whole, "303"},         {Bass, Prefix, "growl"},      {Bass, Prefix, "fretless"},
    {Bass, Prefix, "slap"},        {Bass, Prefix, "lowend"},
    // effects
    {Fx, Prefix, "effect"},        {Fx, Prefix, "riser"},        {Fx, Prefix, "sweep"},        {Fx, Prefix, "uplift"},
    {Fx, Prefix, "downlift"},      {Fx, Prefix, "impact"},       {Fx, Prefix, "noise"},        {Fx, Prefix, "whoosh"},
    {Fx, Prefix, "zap"},           {Fx, Prefix, "laser"},        {Fx, Prefix, "siren"},        {Fx, Prefix, "glitch"},
    {Fx, Prefix, "transition"},    {Fx, Prefix, "explos"},       {Fx, Prefix, "efect"},        {Fx, Prefix, "effet"},
    {Fx, Prefix, "effett"},        {Fx, Prefix, "geraeusch"},    {Fx, Prefix, "ruido"},        {Fx, Prefix, "bruit"},
    {Fx, Prefix, "rumore"},        {Fx, Prefix, "foley"},
};

// Tier 2: mood words — used only when no instrument word decides.
const Rule kMoods[] = {
    {Pad, Prefix, "sphere"},   {Pad, Prefix, "cloud"},    {Pad, Prefix, "space"},     {Pad, Prefix, "dream"},   {Pad, Prefix, "haze"},
    {Pad, Prefix, "mist"},     {Pad, Prefix, "lush"},     {Pad, Prefix, "ether"},     {Pad, Prefix, "aura"},    {Pad, Prefix, "nebul"},
    {Pad, Prefix, "galax"},    {Pad, Prefix, "cosmi"},    {Pad, Prefix, "horizon"},   {Pad, Prefix, "landscape"}, {Pad, Prefix, "air"},
    {Pad, Prefix, "float"},    {Pad, Prefix, "glow"},     {Pad, Prefix, "shimmer"},   {Pad, Prefix, "heaven"},  {Pad, Prefix, "ocean"},
    {Pad, Prefix, "wind"},     {Pad, Prefix, "warm"},     {Pad, Prefix, "serene"},    {Pad, Prefix, "calm"},
    {Lead, Prefix, "driv"},    {Lead, Prefix, "crush"},   {Lead, Prefix, "crunch"},   {Lead, Prefix, "distort"}, {Lead, Prefix, "dirty"},
    {Lead, Prefix, "grit"},    {Lead, Prefix, "fuzz"},    {Lead, Prefix, "squeal"},   {Lead, Prefix, "sharp"},  {Lead, Prefix, "saw"},
    {Lead, Prefix, "square"},  {Lead, Prefix, "anthem"},
    {Fx, Prefix, "anomal"},    {Fx, Prefix, "chaos"},      {Fx, Prefix, "alien"},    {Fx, Prefix, "robot"},
    {Fx, Prefix, "machine"},   {Fx, Prefix, "static"},    {Fx, Prefix, "tension"},    {Fx, Prefix, "horror"},
    {Pluck, Prefix, "pick"},   {Pluck, Prefix, "pop"},
    {Bass, Prefix, "low"},     {Bass, Prefix, "deep"},
};

bool matches(const Rule& r, const std::string& w) {
  switch (r.m) {
    case Whole: return w == r.w;
    case Prefix: return w.rfind(r.w, 0) == 0;
    case Inside: return w.find(r.w) != std::string::npos;
  }
  return false;
}

template <size_t N>
int firstMatch(const Rule (&rules)[N], const std::vector<std::string>& ws, std::string* why) {
  for (const Rule& r : rules)
    for (const auto& w : ws)
      if (matches(r, w)) {
        if (why) *why = w;
        return r.cat;
      }
  return CatAuto;
}

bool has(const std::vector<std::string>& ws, std::initializer_list<const char*> any) {
  for (const auto& w : ws)
    for (const char* a : any)
      if (w == a) return true;
  return false;
}

// Tier 1 with the two-word electric piano names first.
int instrumentWord(const std::vector<std::string>& ws, std::string* why) {
  bool piano = false;
  for (const auto& w : ws) piano = piano || w.find("piano") != std::string::npos;
  if (piano && has(ws, {"e", "el", "elec", "electric", "electro", "electrique", "electrico", "elettrico", "elektrisch", "elektrische",
                        "eletrico", "stage", "digital"})) {
    if (why) *why = "electric piano";
    return EPiano;
  }
  return firstMatch(kWords, ws, why);
}

}  // namespace

int classify(const std::string& pack, const std::string& preset, std::string* why) {
  const std::vector<std::string> pw = words(preset), kw = words(pack);
  for (int tier = 0; tier < 2; ++tier) {
    for (const auto* ws : {&pw, &kw}) {
      const int c = tier == 0 ? firstMatch(kTags, *ws, why) : instrumentWord(*ws, why);
      if (c != CatAuto) return c;
    }
  }
  for (const auto* ws : {&pw, &kw})
    if (int c = firstMatch(kMoods, *ws, why); c != CatAuto) return c;
  if (why) why->clear();
  return Neutral;
}

int drumPiece(const std::string& preset) {
  const std::vector<std::string> ws = words(preset);
  for (const auto& w : ws)
    if (w.find("drum") != std::string::npos || w == "kit" || w == "kits" || w.rfind("percu", 0) == 0 || w == "perc" ||
        w == "prc" || w == "drm" || w.rfind("batter", 0) == 0 || w.rfind("bateria", 0) == 0 || w.rfind("schlag", 0) == 0)
      return -1;  // a kit
  struct P {
    const char* w;
    int pad;
  };
  // Pads of the Drum Model studio kit: 0 kick, 1 snare, 2 rim, 3 clap,
  // 4 closed hat, 5 open hat, 6..8 toms, 9 crash, 10 cowbell, 11 clave,
  // 12 conga, 13 bongo, 14 tabla, 15 timpani.
  static const P pieces[] = {{"kick", 0},     {"kik", 0},      {"snare", 1},   {"snr", 1},       {"rim", 2},      {"clap", 3},
                             {"openhat", 5},  {"hihat", 4},    {"hat", 4},     {"shaker", 4},    {"tamb", 4},     {"tom", 7},
                             {"crash", 9},    {"cymbal", 9},   {"ride", 9},    {"cowbell", 10},  {"agogo", 10},   {"triangle", 10},
                             {"clave", 11},   {"woodblock", 11}, {"conga", 12}, {"bongo", 13},   {"tabla", 14},   {"djemb", 14},
                             {"cajon", 12},   {"timpan", 15},  {"timbal", 7}};
  for (const auto& w : ws)
    for (const P& p : pieces)
      if (w.rfind(p.w, 0) == 0) return p.pad;
  return -1;
}

// ----------------------------------------------------------- identity

Identity identity(const std::string& data) {
  Identity id;
  size_t pos = 0;
  while (pos < data.size()) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) nl = data.size();
    const std::string line = data.substr(pos, nl - pos);
    if (line == "flex 2" || line == "flex 1") id.ok = true;
    else if (line.rfind("pack ", 0) == 0) id.pack = line.substr(5);
    else if (line.rfind("preset ", 0) == 0) id.preset = line.substr(7);
    pos = nl + 1;
  }
  return id;
}

std::string makeData(const std::string& pack, const std::string& preset) { return "flex 2\npack " + pack + "\npreset " + preset + "\n"; }

int categoryOf(const PluginState& s) {
  const int t = s.params.size() > static_cast<size_t>(PTemplate) ? static_cast<int>(std::lround(s.params[PTemplate])) : 0;
  if (t > CatAuto && t < CatCount) return t;
  const Identity id = identity(s.data);
  return classify(id.pack, id.preset);
}

// ------------------------------------------------------------ templates

namespace {

void setp(PluginState& s, int i, double v) {
  if (i >= 0 && static_cast<size_t>(i) < s.params.size()) s.params[static_cast<size_t>(i)] = static_cast<float>(v);
}
constexpr int osc(int k, int f) { return sy::anOsc(k, f); }

PluginState analog(std::initializer_list<std::pair<int, double>> v) {
  PluginState s = defaultPluginState("gl.analog");
  setp(s, sy::AnVoices, 12);
  setp(s, sy::AnVelAmp, 0.5);
  for (const auto& [k, x] : v) setp(s, k, x);
  return s;
}

}  // namespace

PluginState templateState(int cat, int piece) {
  using namespace sy;
  switch (cat) {
    case Bass:
      return analog({{osc(0, OscWave), WaveSaw}, {osc(0, OscLevel), 0.75}, {osc(1, OscWave), WavePulse}, {osc(1, OscCoarse), -12},
                     {osc(1, OscFine), 0}, {osc(1, OscPw), 0.4}, {osc(1, OscLevel), 0.5}, {AnSubLevel, 0.45}, {AnSubWave, 0},
                     {AnFltMode, FltLp24}, {AnCutoff, 650}, {AnReso, 0.2}, {AnFltEnv, 0.45}, {AnFenA, 0.002}, {AnFenD, 0.25},
                     {AnFenS, 0.15}, {AnFenR, 0.15}, {AnAmpA, 0.002}, {AnAmpD, 0.6}, {AnAmpS, 0.8}, {AnAmpR, 0.1},
                     {AnVelFilter, 0.4}, {AnKeyTrack, 0.4}, {AnVoices, 6}});
    case SubBass:
      return analog({{osc(0, OscWave), WaveSine}, {osc(0, OscLevel), 0.95}, {osc(1, OscWave), WaveTriangle}, {osc(1, OscFine), 0},
                     {osc(1, OscLevel), 0.22}, {AnFltMode, FltLp12}, {AnCutoff, 900}, {AnReso, 0.05}, {AnFltEnv, 0.1},
                     {AnFenD, 0.2}, {AnFenS, 0.2}, {AnAmpA, 0.002}, {AnAmpD, 1.8}, {AnAmpS, 0.85}, {AnAmpR, 0.18},
                     {AnModDest, AdPitch}, {AnModAmt, 0.06}, {AnModA, 0.001}, {AnModD, 0.06}, {AnModS, 0}, {AnFxDrive, 0.22},
                     {AnPlayMode, PlayLegato}, {AnGlide, 0.03}, {AnGlideMode, 1}, {AnVelAmp, 0.35}, {AnVoices, 4}});
    case Lead:
      return analog({{osc(0, OscWave), WaveSaw}, {osc(1, OscWave), WaveSaw}, {osc(1, OscFine), 9}, {osc(1, OscLevel), 0.65},
                     {AnUniVoices, 2}, {AnUniDetune, 0.18}, {AnUniSpread, 0.6}, {AnFltMode, FltLp24}, {AnCutoff, 3400},
                     {AnReso, 0.22}, {AnFltEnv, 0.25}, {AnFenD, 0.35}, {AnFenS, 0.4}, {AnAmpA, 0.004}, {AnAmpD, 0.4},
                     {AnAmpS, 0.9}, {AnAmpR, 0.22}, {anLfo(0, LfoDest), AdPitch}, {anLfo(0, LfoAmt), 0.012},
                     {anLfo(0, LfoRate), 5.5}, {anLfo(0, LfoDelay), 0.35}, {AnVoices, 8}});
    case Pad:
      return analog({{AnUniVoices, 4}, {AnUniDetune, 0.32}, {AnUniSpread, 1}, {osc(1, OscFine), 8}, {osc(1, OscLevel), 0.5},
                     {AnFltMode, FltLp12}, {AnCutoff, 2000}, {AnReso, 0.1}, {AnFltEnv, 0.18}, {AnFenA, 1.2}, {AnFenD, 2.0},
                     {AnFenS, 0.5}, {AnFenR, 1.5}, {AnAmpA, 0.8}, {AnAmpD, 1.5}, {AnAmpS, 0.85}, {AnAmpR, 1.8},
                     {anLfo(1, LfoDest), AdCutoff}, {anLfo(1, LfoAmt), 0.15}, {anLfo(1, LfoRate), 0.22},
                     {AnFxDrive + FxChoOn, 1}, {AnFxDrive + FxChoMix, 0.4}, {AnVoices, 16}});
    case Keys: {
      PluginState s = defaultPluginState("gl.keys");
      setp(s, pp::KyModel, 4), setp(s, pp::KyTremDepth, 0), setp(s, pp::KyHardness, 0.5), setp(s, pp::KyRelease, 0.35);
      setp(s, pp::KyDrive, 0), setp(s, pp::KyDecay, 4.5), setp(s, pp::KyVolume, 0.7), setp(s, pp::KyMuffle, 12000);
      return s;
    }
    case EPiano: {
      PluginState s = defaultPluginState("gl.keys");
      setp(s, pp::KyModel, 0), setp(s, pp::KyTremDepth, 0.18), setp(s, pp::KyTremRate, 4.2), setp(s, pp::KyRelease, 0.3);
      setp(s, pp::KyVolume, 0.7);
      return s;
    }
    case Pluck:
      return analog({{osc(0, OscWave), WaveSaw}, {osc(1, OscWave), WaveTriangle}, {osc(1, OscCoarse), 12}, {osc(1, OscFine), 4},
                     {osc(1, OscLevel), 0.45}, {AnFltMode, FltLp12}, {AnCutoff, 750}, {AnReso, 0.15}, {AnFltEnv, 0.7},
                     {AnFenA, 0.001}, {AnFenD, 0.28}, {AnFenS, 0}, {AnFenR, 0.25}, {AnAmpA, 0.001}, {AnAmpD, 0.55},
                     {AnAmpS, 0}, {AnAmpR, 0.35}, {AnVelFilter, 0.4}, {AnUniVoices, 2}, {AnUniDetune, 0.1}});
    case Bell: {
      PluginState s = defaultPluginState("gl.fm");
      if (const PluginInfo* info = findPlugin("gl.fm"))
        for (size_t i = 0; i < info->presets.size(); ++i)
          if (info->presets[i].name == "Bell") s = presetState(*info, i);
      setp(s, pp::fmParam(0, pp::FmRelease), 1.6), setp(s, pp::fmParam(1, pp::FmRelease), 1.2);
      setp(s, pp::FmVolume, 0.55);
      return s;
    }
    case Strings:
      return analog({{osc(0, OscWave), WavePulse}, {osc(0, OscPw), 0.35}, {osc(1, OscWave), WaveSaw}, {osc(1, OscFine), -6},
                     {osc(1, OscLevel), 0.45}, {AnUniVoices, 3}, {AnUniDetune, 0.2}, {AnUniSpread, 0.9},
                     {anLfo(0, LfoDest), AdPulseWidth}, {anLfo(0, LfoAmt), 0.3}, {anLfo(0, LfoRate), 0.6}, {anLfo(0, LfoRetrig), 0},
                     {AnFltMode, FltLp12}, {AnCutoff, 3000}, {AnFltEnv, 0.08}, {AnAmpA, 0.3}, {AnAmpD, 1}, {AnAmpS, 0.9},
                     {AnAmpR, 0.8}, {AnFxDrive + FxChoOn, 1}, {AnFxDrive + FxChoMix, 0.5}, {AnVoices, 16}});
    case Brass:
      return analog({{osc(1, OscFine), -8}, {osc(1, OscLevel), 0.7}, {osc(2, OscOn), 1}, {osc(2, OscWave), WaveSaw},
                     {osc(2, OscCoarse), 0}, {osc(2, OscFine), 6}, {osc(2, OscLevel), 0.5}, {AnFltMode, FltLp12}, {AnCutoff, 950},
                     {AnFltEnv, 0.55}, {AnFenA, 0.08}, {AnFenD, 0.5}, {AnFenS, 0.55}, {AnAmpA, 0.04}, {AnAmpS, 0.9},
                     {AnAmpR, 0.3}, {AnVelFilter, 0.5}});
    case Choir:
      return analog({{osc(0, OscWave), WaveSaw}, {osc(1, OscWave), WaveSaw}, {osc(1, OscFine), 7}, {osc(1, OscLevel), 0.6},
                     {AnUniVoices, 3}, {AnUniDetune, 0.2}, {AnUniSpread, 0.8}, {AnNoise, 0.03}, {AnFltMode, FltBp12},
                     {AnCutoff, 950}, {AnReso, 0.35}, {AnKeyTrack, 0.5}, {AnFltEnv, 0.05}, {AnAmpA, 0.35}, {AnAmpD, 1},
                     {AnAmpS, 0.9}, {AnAmpR, 0.9}, {anLfo(0, LfoDest), AdPitch}, {anLfo(0, LfoAmt), 0.01},
                     {anLfo(0, LfoRate), 5}, {anLfo(0, LfoDelay), 0.3}, {AnFxDrive + FxChoOn, 1}, {AnFxDrive + FxChoMix, 0.5},
                     {AnVoices, 16}});
    case Stab:
      return analog({{AnUniVoices, 5}, {AnUniDetune, 0.3}, {AnUniSpread, 1}, {osc(1, OscFine), 0}, {osc(1, OscCoarse), 12},
                     {osc(1, OscLevel), 0.35}, {AnFltMode, FltLp12}, {AnCutoff, 4200}, {AnReso, 0.1}, {AnFltEnv, 0.35},
                     {AnFenD, 0.25}, {AnFenS, 0.3}, {AnAmpA, 0.003}, {AnAmpD, 0.5}, {AnAmpS, 0.45}, {AnAmpR, 0.2},
                     {AnVoices, 16}});
    case Arp:
      return analog({{osc(0, OscWave), WavePulse}, {osc(0, OscPw), 0.3}, {osc(1, OscWave), WaveSaw}, {osc(1, OscFine), 5},
                     {osc(1, OscLevel), 0.35}, {AnFltMode, FltLp24}, {AnCutoff, 950}, {AnReso, 0.45}, {AnFltEnv, 0.55},
                     {AnFenD, 0.16}, {AnFenS, 0}, {AnAmpA, 0.001}, {AnAmpD, 0.22}, {AnAmpS, 0.25}, {AnAmpR, 0.12}});
    case Guitar: {
      PluginState s = defaultPluginState("gl.plucker");
      setp(s, md::strs::PkDecay, 150), setp(s, md::strs::PkColor, 72);
      return s;
    }
    case Organ:
      return analog({{osc(0, OscWave), WaveSine}, {osc(0, OscLevel), 0.7}, {osc(1, OscWave), WaveSine}, {osc(1, OscCoarse), 12},
                     {osc(1, OscFine), 0}, {osc(1, OscLevel), 0.5}, {osc(2, OscOn), 1}, {osc(2, OscWave), WaveSine},
                     {osc(2, OscCoarse), 19}, {osc(2, OscLevel), 0.3}, {AnNoise, 0.004}, {AnFltMode, FltLp12}, {AnCutoff, 9000},
                     {AnFltEnv, 0}, {AnKeyTrack, 0}, {AnAmpA, 0.006}, {AnAmpD, 0.1}, {AnAmpS, 1}, {AnAmpR, 0.08},
                     {AnVelAmp, 0.1}, {AnFxDrive + FxChoOn, 1}, {AnFxDrive + FxChoRate, 5.5}, {AnFxDrive + FxChoDepth, 0.3},
                     {AnFxDrive + FxChoMix, 0.45}, {AnVoices, 16}});
    case Fx:
      return analog({{osc(0, OscWave), WaveSaw}, {osc(1, OscWave), WaveNoise}, {osc(1, OscLevel), 0.35}, {AnFltMode, FltBp12},
                     {AnCutoff, 1400}, {AnReso, 0.45}, {AnFltEnv, 0.6}, {AnFenA, 1.5}, {AnFenD, 2}, {AnFenS, 0.6},
                     {AnModDest, AdPitch}, {AnModAmt, 0.35}, {AnModA, 1.5}, {AnModD, 1.5}, {AnModS, 1}, {AnAmpA, 0.25},
                     {AnAmpD, 1}, {AnAmpS, 1}, {AnAmpR, 1.2}, {anLfo(1, LfoDest), AdCutoff}, {anLfo(1, LfoAmt), 0.25},
                     {anLfo(1, LfoRate), 0.3}});
    case Drums: {
      PluginState s = defaultPluginState("gl.drumkit");
      setp(s, md::DmBaseNote, 60), setp(s, md::DmLimiter, 1), setp(s, md::DmVolume, 0.8);
      if (piece > 0 && piece < md::kDrumPads) {  // one pitched piece: its pad's sound on pad 1
        for (int k = 0; k < md::DpCount; ++k) setp(s, md::drumPad(0, k), s.params[static_cast<size_t>(md::drumPad(piece, k))]);
        setp(s, md::drumPad(0, md::DpPan), 0), setp(s, md::drumPad(0, md::DpChoke), 0);
      }
      if (piece >= 0) setp(s, md::DmKeyMode, 1);
      return s;
    }
    default:  // neutral
      return analog({{osc(0, OscWave), WaveSaw}, {osc(1, OscWave), WaveTriangle}, {osc(1, OscFine), 5}, {osc(1, OscLevel), 0.5},
                     {AnFltMode, FltLp12}, {AnCutoff, 3800}, {AnReso, 0.1}, {AnFltEnv, 0.2}, {AnFenD, 0.5}, {AnFenS, 0.4},
                     {AnAmpA, 0.01}, {AnAmpD, 0.6}, {AnAmpS, 0.7}, {AnAmpR, 0.4}, {AnVoices, 16}});
  }
}

// --------------------------------------------------------- control laws

namespace {

inline float at(const float* p, int i) { return p[i]; }
// Envelope time factor of a control centred at 0.5 (× 1/8 .. × 8).
inline double timeMul(float v) { return std::pow(8.0, (static_cast<double>(v) - 0.5) * 2.0); }
// Sustain offset around the template's level.
inline double susOf(double s, float v) { return v < 0.5f ? s * 2.0 * v : s + (1.0 - s) * (2.0 * v - 1.0); }
inline double macro(const float* p, int k) { return std::clamp(static_cast<double>(p[Macro1 + k]), 0.0, 1.0); }

// Brightness in octaves: cutoff, post filter (when on), macro 1, tone.
double brightOct(const float* p) {
  double o = (at(p, Cutoff) - 0.5) * 6.0;
  if (at(p, PostOn) >= 0.5f) o += std::min(0.0, (static_cast<double>(at(p, PostCut)) - 1.0) * 1.5);
  o += macro(p, 0) * 2.0;
  o += std::clamp(static_cast<double>(at(p, PTone)), -1.0, 1.0) * 2.0;
  return o;
}

struct W {
  const PluginInfo& info;
  PluginState& s;
  double get(int i) const { return i >= 0 && static_cast<size_t>(i) < s.params.size() ? s.params[static_cast<size_t>(i)] : 0.0; }
  void set(int i, double v) {
    if (i < 0 || static_cast<size_t>(i) >= s.params.size() || static_cast<size_t>(i) >= info.params.size()) return;
    s.params[static_cast<size_t>(i)] = static_cast<float>(info.params[static_cast<size_t>(i)].clamp(v));
  }
};

void applyAnalog(const float* p, W& w) {
  using namespace sy;
  const double oct = brightOct(p);
  w.set(AnCutoff, w.get(AnCutoff) * std::pow(2.0, oct));
  w.set(AnReso, w.get(AnReso) + (at(p, Reso) - 0.5) * 0.8 + macro(p, 1) * 0.35);
  w.set(AnFltEnv, w.get(AnFltEnv) + (at(p, EnvAmt) - 0.5) * 1.2);
  w.set(AnUniDetune, w.get(AnUniDetune) + macro(p, 1) * 0.3);
  w.set(AnAmpA, w.get(AnAmpA) * timeMul(at(p, VolA)) + macro(p, 2) * 1.5);
  w.set(AnAmpD, w.get(AnAmpD) * timeMul(at(p, VolD)) * (1.0 + macro(p, 3) * 3.0));
  w.set(AnAmpS, susOf(w.get(AnAmpS), at(p, VolS)));
  w.set(AnAmpR, w.get(AnAmpR) * timeMul(at(p, VolR)) * (1.0 + macro(p, 3) * 5.0));
  w.set(AnFenA, w.get(AnFenA) * timeMul(at(p, FltA)) + macro(p, 2) * 1.0);
  w.set(AnFenD, w.get(AnFenD) * timeMul(at(p, FltD)));
  w.set(AnFenS, susOf(w.get(AnFenS), at(p, FltS)));
  w.set(AnFenR, w.get(AnFenR) * timeMul(at(p, FltR)));
  if (const double m5 = macro(p, 4); m5 > 0.001) {  // movement: slow filter sweep
    const int dest = anLfo(1, LfoDest);
    if (static_cast<int>(w.get(dest)) == AdNone) w.set(dest, AdCutoff), w.set(anLfo(1, LfoRate), 0.35), w.set(anLfo(1, LfoAmt), 0);
    w.set(anLfo(1, LfoAmt), w.get(anLfo(1, LfoAmt)) + m5 * 0.45);
  }
  if (const double m6 = macro(p, 5); m6 > 0.001) {  // width
    const double was = w.get(AnFxDrive + FxChoOn) >= 0.5 ? w.get(AnFxDrive + FxChoMix) : 0.0;
    w.set(AnFxDrive + FxChoOn, 1);
    w.set(AnFxDrive + FxChoMix, std::max(was, m6 * 0.7));
    w.set(AnUniSpread, w.get(AnUniSpread) + m6);
  }
  w.set(AnFxDrive, w.get(AnFxDrive) + macro(p, 6) * 0.7);
}

void applyKeys(const float* p, W& w) {
  using namespace pp;
  const double oct = brightOct(p);
  w.set(KyMuffle, w.get(KyMuffle) * std::pow(2.0, oct));
  w.set(KyHardness, w.get(KyHardness) + macro(p, 0) * 0.35 + std::clamp(static_cast<double>(at(p, PTone)), -1.0, 1.0) * 0.2 +
                        (at(p, Reso) - 0.5) * 0.3);
  w.set(KyDrive, w.get(KyDrive) + macro(p, 1) * 0.3 + macro(p, 6) * 0.6);
  w.set(KyDecay, w.get(KyDecay) * timeMul(at(p, VolD)) * (1.0 + macro(p, 3) * 2.0));
  w.set(KyRelease, w.get(KyRelease) * timeMul(at(p, VolR)) * (1.0 + macro(p, 3) * 3.0));
  w.set(KyTremDepth, w.get(KyTremDepth) + macro(p, 4) * 0.5);
  w.set(KyStereo, w.get(KyStereo) + macro(p, 5) * 0.5);
}

void applyFm(const float* p, W& w) {
  using namespace pp;
  const double oct = brightOct(p);
  for (int o = 1; o < kFmOps; ++o) w.set(fmParam(o, FmLevel), w.get(fmParam(o, FmLevel)) * std::pow(2.0, oct * 0.5));
  for (int o = 0; o < kFmOps; ++o) {
    w.set(fmParam(o, FmAttack), w.get(fmParam(o, FmAttack)) * timeMul(at(p, VolA)) + macro(p, 2) * 1.0);
    w.set(fmParam(o, FmDecay), w.get(fmParam(o, FmDecay)) * timeMul(at(p, VolD)) * (1.0 + macro(p, 3) * 3.0));
    w.set(fmParam(o, FmRelease), w.get(fmParam(o, FmRelease)) * timeMul(at(p, VolR)) * (1.0 + macro(p, 3) * 3.0));
  }
  w.set(fmParam(0, FmSustain), susOf(w.get(fmParam(0, FmSustain)), at(p, VolS)));
  w.set(FmFeedback, w.get(FmFeedback) + macro(p, 1) * 0.4 + macro(p, 6) * 0.3 + (at(p, Reso) - 0.5) * 0.3);
  w.set(FmVibrato, w.get(FmVibrato) + macro(p, 4) * 25.0);
}

void applyDrums(const float* p, W& w, int pitchSt) {
  namespace m = md;
  const double oct = brightOct(p);
  w.set(m::DmHigh, w.get(m::DmHigh) + oct * 3.0);
  w.set(m::DmLow, w.get(m::DmLow) + macro(p, 1) * 6.0);
  w.set(m::DmLofi, w.get(m::DmLofi) + macro(p, 6) * 0.6);
  const double dec = timeMul(at(p, VolD)) * (1.0 + macro(p, 3) * 2.0);
  for (int k = 0; k < m::kDrumPads; ++k) {
    w.set(m::drumPad(k, m::DpDecay), w.get(m::drumPad(k, m::DpDecay)) * dec);
    if (pitchSt != 0) w.set(m::drumPad(k, m::DpTune), w.get(m::drumPad(k, m::DpTune)) + pitchSt);
  }
}

void applyPlucker(const float* p, W& w) {
  namespace s = md::strs;
  const double oct = brightOct(p);
  w.set(s::PkColor, w.get(s::PkColor) + oct * 18.0);
  w.set(s::PkDecay, w.get(s::PkDecay) + std::log2(timeMul(at(p, VolD)) * timeMul(at(p, VolR))) * 12.0 + macro(p, 3) * 80.0);
}

// Template trims (dB): a typical phrase of each template lands near
// -14 LUFS at full FLEX output volume (Gridline's own loudness meter).
double trimDb(int cat) {
  static const double t[CatCount] = {0, /*bass*/ 0.9, /*sub*/ -5.0, /*lead*/ -2.1, /*pad*/ -3.0, /*keys*/ 3.0, /*ep*/ -1.9,
                                     /*pluck*/ 4.0, /*bell*/ -3.5, /*strings*/ -7.7, /*brass*/ -7.4, /*choir*/ -1.4,
                                     /*stab*/ 0.0, /*arp*/ 4.0, /*guitar*/ 4.0, /*organ*/ -8.0, /*fx*/ -1.4, /*drums*/ -5.3,
                                     /*neutral*/ -3.6};
  return cat >= 0 && cat < CatCount ? t[cat] : 0.0;
}

}  // namespace

void applyControls(int cat, const float* p, const PluginInfo& info, PluginState& work) {
  W w{info, work};
  switch (engineOf(cat)) {
    case EAnalog: applyAnalog(p, w); break;
    case EKeys: applyKeys(p, w); break;
    case EFm: applyFm(p, w); break;
    case EDrums: {
      const bool pitched = work.params.size() > static_cast<size_t>(md::DmKeyMode) && work.params[md::DmKeyMode] >= 0.5f;
      applyDrums(p, w, pitched ? 0 : static_cast<int>(std::lround((at(p, Pitch) - 0.5) * 48.0)));
      break;
    }
    case EPlucker: applyPlucker(p, w); break;
    default: break;
  }
}

FxSettings fxSettings(int cat, const float* p) {
  (void)cat;
  FxSettings f;
  const double m8 = macro(p, 7);
  f.revOn = at(p, RevOn) >= 0.5f || m8 > 0.001;
  f.revWet = static_cast<float>(std::clamp((at(p, RevOn) >= 0.5f ? at(p, RevMix) * 0.6 : 0.0) + m8 * 0.35, 0.0, 1.0));
  f.revSize = std::clamp(at(p, RevSize), 0.0f, 1.0f);
  f.revDecay = static_cast<float>(0.3 * std::pow(25.0, std::clamp(static_cast<double>(at(p, RevDecay)), 0.0, 1.0)));
  f.revHighCut = static_cast<float>(1500.0 * std::pow(10.0, std::clamp(static_cast<double>(at(p, RevColor)), 0.0, 1.0)));
  f.dlyOn = at(p, DlyOn) >= 0.5f;
  f.dlyWet = static_cast<float>(std::clamp(at(p, DlyMix) * 0.5, 0.0, 1.0));
  f.dlyFb = static_cast<float>(std::clamp(at(p, DlyFb) * 0.85, 0.0, 0.9));
  // Delay time: 1..768 ticks of 1/192 beat → nearest Gridline delay division.
  const double beats = (1.0 + std::clamp(static_cast<double>(at(p, DlyTime)), 0.0, 1.0) * 767.0) / 192.0;
  static const double kBeats[16] = {0.125, 1.0 / 6, 0.375, 0.25, 1.0 / 3, 0.4375, 0.5, 2.0 / 3, 0.75, 1, 1.5, 2, 0.0625, 4.0 / 3, 3, 4};
  double best = 1e9;
  for (int i = 0; i < 16; ++i)
    if (const double d = std::fabs(std::log(kBeats[i] / beats)); d < best) best = d, f.dlyDiv = i;
  return f;
}

float outputGain(int cat, const float* p) {
  const double vol = std::sqrt(std::clamp(static_cast<double>(at(p, OutVol)), 0.0, 1.0));
  const double pre = std::sqrt(2.0 * std::max(0.05, static_cast<double>(at(p, LimPre))));
  const double db = trimDb(cat) + std::clamp(static_cast<double>(at(p, PLevel)), -24.0, 12.0);
  return static_cast<float>(vol * std::min(pre, 1.6) * std::pow(10.0, db / 20.0));
}

double pitchOffset(int cat, const float* p) {
  const double st = (at(p, Pitch) - 0.5) * 48.0;
  if (cat == Drums) return 0;
  return st + std::clamp(std::round(static_cast<double>(at(p, POctave))), -2.0, 2.0) * 12.0;
}

bool softClip(const float* p) { return at(p, LimOn) >= 0.5f; }

int drumNote(int note) {
  // General-MIDI-style percussion keys → pads (see drumPiece for the order).
  static const int8_t map[47] = {
      0,  0,  2,  1,  3,  1,  6,  4,  6,  4,  7,  5,  7,  8,  9,  8,  //  35..50
      5,  9,  5,  4,  9,  10, 9,  11, 5,  13, 13, 12, 12, 12, 7,  7,  //  51..66
      10, 10, 4,  4,  11, 11, 4,  4,  11, 11, 11, 14, 14, 10, 10,     //  67..81
  };
  while (note < 35) note += 12;
  while (note > 81) note -= 12;
  return 60 + map[note - 35];
}

// ------------------------------------------------------------- states

PluginState makeState(const std::string& pack, const std::string& preset, const float* controls) {
  PluginState s = defaultPluginState(kType);
  if (s.type.empty()) s.type = kType;
  if (s.params.size() < static_cast<size_t>(PCount)) s.params.resize(PCount, 0.0f);
  for (int k = 0; k < kControls; ++k) s.params[static_cast<size_t>(k)] = std::clamp(controls[k], 0.0f, 1.0f);
  s.data = makeData(pack, preset);
  return s;
}

PluginState standInState(const PluginState& fs) {
  const int cat = categoryOf(fs);
  const Identity id = identity(fs.data);
  PluginState w = templateState(cat, cat == Drums ? drumPiece(id.preset) : -1);
  float p[PCount];
  for (int k = 0; k < PCount; ++k) p[k] = k < static_cast<int>(fs.params.size()) ? fs.params[static_cast<size_t>(k)] : 0.0f;
  if (const PluginInfo* info = findPlugin(w.type)) applyControls(cat, p, *info, w);
  // The output level, pitch and effects become the engine's own settings where it has them.
  if (w.type == "gl.analog") {
    namespace s = sy;
    W ww{*findPlugin(w.type), w};
    ww.set(s::AnVolume, ww.get(s::AnVolume) * outputGain(cat, p));
    ww.set(s::AnTranspose, pitchOffset(cat, p));
    const FxSettings f = fxSettings(cat, p);
    if (f.revOn) ww.set(s::AnFxDrive + s::FxRevOn, 1), ww.set(s::AnFxDrive + s::FxRevMix, f.revWet), ww.set(s::AnFxDrive + s::FxRevSize, f.revSize);
    if (f.dlyOn) ww.set(s::AnFxDrive + s::FxDlyOn, 1), ww.set(s::AnFxDrive + s::FxDlyMix, f.dlyWet), ww.set(s::AnFxDrive + s::FxDlyFb, f.dlyFb);
  } else if (w.type == "gl.keys") {
    W ww{*findPlugin(w.type), w};
    ww.set(pp::KyVolume, ww.get(pp::KyVolume) * outputGain(cat, p));
    ww.set(pp::KyTune, std::fmod(pitchOffset(cat, p), 1.0) * 100.0);
  } else if (w.type == "gl.fm") {
    W ww{*findPlugin(w.type), w};
    ww.set(pp::FmVolume, ww.get(pp::FmVolume) * outputGain(cat, p));
    ww.set(pp::FmOctave, std::round(pitchOffset(cat, p) / 12.0));
  } else if (w.type == "gl.drumkit") {
    W ww{*findPlugin(w.type), w};
    ww.set(md::DmVolume, ww.get(md::DmVolume) * outputGain(cat, p));
    if (ww.get(md::DmKeyMode) < 0.5) {
      // A whole kit: the stand-in maps General-MIDI-style keys onto the pads;
      // a plain Drum Model plays pad k at base + k, so lay the pads out in
      // that order from key 35 (kick, kick, rim, snare, clap, … crash, tom).
      const PluginState kit = w;
      for (int k = 0; k < md::kDrumPads; ++k) {
        const int from = drumNote(35 + k) - 60;
        for (int f = 0; f < md::DpCount; ++f)
          w.params[static_cast<size_t>(md::drumPad(k, f))] = kit.params[static_cast<size_t>(md::drumPad(from, f))];
      }
      ww.set(md::DmBaseNote, 35);
    }
  }
  return w;
}

// ------------------------------------------------------------- migration

bool isLegacy(const PluginState& s) {
  return (s.type == "gl.analog" || s.type == "gl.keys" || s.type == "gl.fm") && s.data.rfind("flex 1\n", 0) == 0;
}

bool migrateLegacy(PluginState& s) {
  if (!isLegacy(s)) return false;
  float v[kControls];
  for (float& x : v) x = 0.5f;
  size_t pos = s.data.find("\nv");
  if (pos != std::string::npos) {
    const char* c = s.data.c_str() + pos + 2;
    char* e = nullptr;
    for (int k = 0; k < kControls; ++k) {
      const double x = platform::parseDouble(c, &e);
      if (e == c) break;
      v[k] = static_cast<float>(x);
      c = e;
    }
  }
  const Identity id = identity(s.data);
  const std::string ins = s.ins;
  s = makeState(id.pack, id.preset, v);
  s.ins = ins;
  return true;
}

}  // namespace gl::flex
