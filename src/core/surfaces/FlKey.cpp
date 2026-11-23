// Gridline - Novation FLkey keyboards as full controllers (see FlKey.h).
#include "FlKey.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include "../Json.h"
#include "../Lanes.h"
#include "../Log.h"
#include "../NoteTools.h"
#include "../Params.h"
#include "../SongTools.h"

namespace gl::flkey {

// ------------------------------------------------------------------ models ----

const std::vector<Model>& models() {
  static const std::vector<Model> m = {
      {"flkey-mini", "Novation FLkey Mini", "FLkey Mini", Gen::First, true, false, false},
      {"flkey-37", "Novation FLkey 37", "FLkey 37", Gen::First, false, false, true},
      {"flkey-49", "Novation FLkey 49", "FLkey 49", Gen::First, false, true, true},
      {"flkey-61", "Novation FLkey 61", "FLkey 61", Gen::First, false, true, true},
      {"flkey2-mini", "Novation FLkey 2 Mini 25", "FLkey 2 Mini", Gen::Second, true, false, true},
      {"flkey2-37", "Novation FLkey 2 37", "FLkey 2 37", Gen::Second, false, false, true},
      {"flkey2-49", "Novation FLkey 2 49", "FLkey 2 49", Gen::Second, false, true, true},
      {"flkey2-61", "Novation FLkey 2 61", "FLkey 2 61", Gen::Second, false, true, true},
  };
  return m;
}

const Model* modelById(const std::string& id) {
  for (const auto& m : models())
    if (id == m.id) return &m;
  return nullptr;
}

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
// Letters and digits only, lower case ("Half Whole Dim" -> "halfwholedim").
std::string squash(const std::string& s) {
  std::string o;
  for (unsigned char c : s)
    if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
  return o;
}
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

PortMatch matchPort(const std::string& portName) {
  PortMatch r;
  const std::string n = lower(portName);
  const size_t at = n.find("flkey");
  if (at == std::string::npos) return r;
  const std::string tail = n.substr(at + 5);  // after "flkey"
  const bool second = tail.rfind(" 2 ", 0) == 0 || tail.rfind(" 2mini", 0) == 0 || tail.rfind("2 ", 0) == 0;
  const std::string size = second ? tail.substr(tail.find('2') + 1) : tail;
  const char* want = has(size, "mini") ? "mini" : has(size, "61") ? "61" : has(size, "49") ? "49" : has(size, "37") ? "37" : nullptr;
  if (!want) want = "49", r.sizeGuessed = true;
  const std::string id = std::string(second ? "flkey2-" : "flkey-") + want;
  r.model = modelById(id);
  // The DAW port: "... DAW Out / In", Windows "MIDIIN2 / MIDIOUT2 (...)", ALSA "... MIDI 2".
  r.daw = has(n, "daw") || has(n, "midiin2") || has(n, "midiout2") || has(n, "midi 2");
  return r;
}

std::string pairedOutput(const std::string& input, const std::vector<std::string>& outputs) {
  auto listed = [&](const std::string& s) { return std::find(outputs.begin(), outputs.end(), s) != outputs.end(); };
  if (listed(input)) return input;  // ALSA and JACK list one name for both directions
  const std::pair<const char*, const char*> swaps[] = {{"DAW Out", "DAW In"}, {"MIDI Out", "MIDI In"}, {"MIDIIN", "MIDIOUT"},
                                                       {"Out", "In"}};
  for (const auto& [from, to] : swaps) {
    const size_t at = input.find(from);
    if (at == std::string::npos) continue;
    std::string o = input;
    o.replace(at, std::string(from).size(), to);
    if (listed(o)) return o;
  }
  // Any output of the same keyboard and port.
  const PortMatch in = matchPort(input);
  for (const auto& o : outputs) {
    const PortMatch m = matchPort(o);
    if (m.model && m.model == in.model && m.daw == in.daw) return o;
  }
  return {};
}

const char* buttonId(Button b) {
  static const char* ids[] = {"play",      "stop",        "record",     "loop",      "scoreLog",  "quantize",  "metronome",
                              "undo",      "redo",        "patternSong", "shift",    "channelUp", "channelDown", "mixerLeft",
                              "mixerRight", "pageLeft",   "pageRight",  "knobBankUp", "knobBankDown", "solo"};
  static_assert(sizeof(ids) / sizeof(ids[0]) == static_cast<size_t>(Button::Count), "a name per button");
  return ids[static_cast<size_t>(b)];
}

const char* padPageName(PadPage p) {
  switch (p) {
    case PadPage::Steps: return "Steps";
    case PadPage::Channels: return "Channels";
    case PadPage::Patterns: return "Patterns";
    case PadPage::Instrument: return "Instrument";
    case PadPage::Device: return "Keyboard";
  }
  return "";
}

const char* knobModeName(KnobMode k) {
  switch (k) {
    case KnobMode::Plugin: return "Plugin";
    case KnobMode::MixerVolume: return "Mixer volume";
    case KnobMode::MixerPan: return "Mixer pan";
    case KnobMode::ChannelVolume: return "Channel volume";
    case KnobMode::ChannelPan: return "Channel pan";
    case KnobMode::Transport: return "Transport";
    case KnobMode::Device: return "Custom";
  }
  return "";
}

Wire defaultWire(const Model& m) {
  Wire w;
  w.buttons.fill(-1);
  auto set = [&](Button b, int cc) { w.buttons[static_cast<size_t>(b)] = cc; };
  // Shared by both families (Launchkey [MK3] and MK4 guides).
  set(Button::Play, 115);
  set(Button::Record, 117);
  set(Button::ChannelUp, 104);
  set(Button::ChannelDown, 105);
  if (m.gen == Gen::First) {
    w.sysexId = 0x0F;
    w.faderCc = 0x35;
    w.modeChannel = 15;
    w.padModeCc = 3, w.knobModeCc = 9, w.faderModeCc = 10;
    w.padModes = {{0, PadPage::Device}, {1, PadPage::Device}, {3, PadPage::Device}, {4, PadPage::Device}, {5, PadPage::Device},
                  {6, PadPage::Device}, {7, PadPage::Device}, {8, PadPage::Device}, {10, PadPage::Device}};
    w.knobModes = {{0, KnobMode::Device}, {1, KnobMode::MixerVolume}, {2, KnobMode::Plugin}, {3, KnobMode::MixerPan},
                   {4, KnobMode::ChannelVolume}, {5, KnobMode::ChannelPan}, {6, KnobMode::Device}, {7, KnobMode::Device},
                   {8, KnobMode::Device}, {9, KnobMode::Device}};
    w.faderModes = {{0, FaderMode::Device}, {1, FaderMode::MixerVolume}, {2, FaderMode::Plugin}, {4, FaderMode::ChannelVolume},
                    {6, FaderMode::Device}, {7, FaderMode::Device}, {8, FaderMode::Device}, {9, FaderMode::Device}};
    w.scaleChannel = 15, w.scaleOnCc = 14, w.scaleTypeCc = 15, w.scaleRootCc = 16;
    w.scaleNames = {"Minor", "Major", "Dorian", "Mixolydian", "Phrygian", "Harmonic Minor", "Minor Pentatonic", "Major Pentatonic"};
    set(Button::Shift, 108);
    if (!m.mini) {
      set(Button::Stop, 116);
      set(Button::ScoreLog, 118);
      set(Button::Quantize, 74);
      set(Button::Metronome, 75);
      set(Button::Undo, 76);
      set(Button::Redo, 77);
      set(Button::MixerLeft, 103);
      set(Button::MixerRight, 102);
      set(Button::PageLeft, 106);
      set(Button::PageRight, 107);
      set(Button::KnobBankUp, 51);
      set(Button::KnobBankDown, 52);
    }
    if (m.faders) set(Button::Solo, 0x2D);
  } else {
    w.sysexId = m.mini ? 0x13 : 0x14;
    w.faderCc = 0x05;
    w.relativeTransport = true;
    w.modeChannel = 6;
    w.padModeCc = 29, w.knobModeCc = 30, w.faderModeCc = 31;
    w.padModes = {{1, PadPage::Device},  {15, PadPage::Device}, {4, PadPage::Device}, {5, PadPage::Device}, {6, PadPage::Device},
                  {7, PadPage::Device},  {8, PadPage::Device},  {13, PadPage::Device}, {14, PadPage::Device}};
    w.knobModes = {{1, KnobMode::MixerVolume}, {2, KnobMode::Plugin}, {4, KnobMode::ChannelVolume}, {5, KnobMode::Transport},
                   {6, KnobMode::Device},      {7, KnobMode::Device}, {8, KnobMode::Device},        {9, KnobMode::Device}};
    w.faderModes = {{1, FaderMode::MixerVolume}, {6, FaderMode::Device}, {7, FaderMode::Device}, {8, FaderMode::Device}, {9, FaderMode::Device}};
    w.rgb = true;
    w.oled = true;
    w.scaleChannel = 6, w.scaleOnCc = 74, w.scaleTypeCc = 62, w.scaleRootCc = 61;
    w.scaleNames = {"Major",          "Minor",           "Dorian",          "Mixolydian",       "Lydian",          "Phrygian",
                    "Locrian",        "Whole Tone",      "Half Whole Dim",  "Whole Half Diminished", "Blues",   "Minor Pentatonic",
                    "Major Pentatonic", "Harmonic Minor", "Harmonic Major", "Dorian #4",        "Phrygian Dominant", "Melodic Minor",
                    "Lydian Augmented", "Lydian Dominant", "Super Locrian",  "8-tone Spanish",   "Bhairav",         "Hungarian Minor",
                    "Hirajoshi",      "In-Sen",          "Iwato",           "Kumoi",            "Pelog-Selisir",   "Pelog-Tembung"};
    set(Button::Shift, 63);
    set(Button::PageLeft, 106);
    set(Button::PageRight, 107);
    if (m.mini) {
      set(Button::KnobBankUp, 55);
      set(Button::KnobBankDown, 56);
    } else {
      set(Button::Stop, 116);
      set(Button::Loop, 118);
      set(Button::ScoreLog, 74);
      set(Button::Undo, 75);
      set(Button::Metronome, 76);
      set(Button::PatternSong, 77);
      set(Button::MixerLeft, 103);
      set(Button::MixerRight, 102);
      set(Button::KnobBankUp, 51);
      set(Button::KnobBankDown, 52);
    }
    if (m.faders) set(Button::Solo, 0x2D);
  }
  return w;
}

namespace {

bool pageByName(const std::string& s, PadPage& out) {
  const std::string n = squash(s);
  for (PadPage p : {PadPage::Steps, PadPage::Channels, PadPage::Patterns, PadPage::Instrument, PadPage::Device})
    if (n == squash(padPageName(p)) || (p == PadPage::Device && n == "device")) return out = p, true;
  return false;
}
bool knobByName(const std::string& s, KnobMode& out) {
  const std::string n = squash(s);
  for (KnobMode k : {KnobMode::Plugin, KnobMode::MixerVolume, KnobMode::MixerPan, KnobMode::ChannelVolume, KnobMode::ChannelPan,
                     KnobMode::Transport, KnobMode::Device})
    if (n == squash(knobModeName(k)) || (k == KnobMode::Device && n == "device")) return out = k, true;
  return false;
}
bool faderByName(const std::string& s, FaderMode& out) {
  const std::string n = squash(s);
  if (n == "mixervolume") return out = FaderMode::MixerVolume, true;
  if (n == "channelvolume") return out = FaderMode::ChannelVolume, true;
  if (n == "plugin") return out = FaderMode::Plugin, true;
  if (n == "device" || n == "custom") return out = FaderMode::Device, true;
  return false;
}
uint8_t byteOf(const Json& j, const char* key, uint8_t dflt) {
  const Json* v = j.find(key);
  return v && v->is(Json::Type::Number) ? static_cast<uint8_t>(std::clamp(jsonInt<int>(v->number), 0, 127)) : dflt;
}
template <class T, class F>
void modeMap(const Json& j, const char* key, std::map<int, T>& out, F byName) {
  const Json* o = j.find(key);
  if (!o || !o->is(Json::Type::Object)) return;
  for (const auto& [k, v] : o->object) {
    T t{};
    if (v.is(Json::Type::String) && byName(v.str, t)) out[std::atoi(k.c_str())] = t;
  }
}

void applyOne(Wire& w, const Json& j) {
  w.sysexId = byteOf(j, "sysexId", w.sysexId);
  w.padTop = byteOf(j, "padTop", w.padTop);
  w.padBottom = byteOf(j, "padBottom", w.padBottom);
  w.knobCc = byteOf(j, "knobCc", w.knobCc);
  w.faderCc = byteOf(j, "faderCc", w.faderCc);
  w.faderButtonCc = byteOf(j, "faderButtonCc", w.faderButtonCc);
  w.transportCc = byteOf(j, "transportCc", w.transportCc);
  w.modeChannel = std::clamp(static_cast<int>(j.num("modeChannel", w.modeChannel + 1)) - 1, 0, 15);
  w.padModeCc = byteOf(j, "padModeCc", static_cast<uint8_t>(w.padModeCc));
  w.knobModeCc = byteOf(j, "knobModeCc", static_cast<uint8_t>(w.knobModeCc));
  w.faderModeCc = byteOf(j, "faderModeCc", static_cast<uint8_t>(w.faderModeCc));
  w.padDawValue = byteOf(j, "padDawValue", static_cast<uint8_t>(w.padDawValue));
  if (const Json* v = j.find("rgb"); v && v->is(Json::Type::Bool)) w.rgb = v->boolean;
  if (const Json* v = j.find("oled"); v && v->is(Json::Type::Bool)) w.oled = v->boolean;
  if (const Json* v = j.find("relativeTransport"); v && v->is(Json::Type::Bool)) w.relativeTransport = v->boolean;
  if (const Json* b = j.find("buttons"); b && b->is(Json::Type::Object))
    for (size_t i = 0; i < static_cast<size_t>(Button::Count); ++i)
      if (const Json* v = b->find(buttonId(static_cast<Button>(i))); v && v->is(Json::Type::Number))
        w.buttons[i] = std::clamp(jsonInt<int>(v->number), -1, 127);
  modeMap(j, "padModes", w.padModes, pageByName);
  modeMap(j, "knobModes", w.knobModes, knobByName);
  modeMap(j, "faderModes", w.faderModes, faderByName);
  if (const Json* s = j.find("scale"); s && s->is(Json::Type::Object)) {
    w.scaleChannel = std::clamp(static_cast<int>(s->num("channel", w.scaleChannel + 1)) - 1, 0, 15);
    w.scaleOnCc = byteOf(*s, "on", static_cast<uint8_t>(w.scaleOnCc));
    w.scaleTypeCc = byteOf(*s, "type", static_cast<uint8_t>(w.scaleTypeCc));
    w.scaleRootCc = byteOf(*s, "root", static_cast<uint8_t>(w.scaleRootCc));
  }
}

}  // namespace

bool applyOverrides(Wire& w, const Model& m, const std::string& json, std::string* err) {
  Json j;
  if (!jsonParse(json, j) || !j.is(Json::Type::Object)) {
    if (err) *err = "not a JSON object";
    return false;
  }
  if (const Json* all = j.find("*"); all && all->is(Json::Type::Object)) applyOne(w, *all);
  if (const Json* one = j.find(m.id); one && one->is(Json::Type::Object)) applyOne(w, *one);
  return true;
}

// ------------------------------------------------------------------ colours ----

namespace {

// The LED palette as it looks (measured from the swatches printed in Novation's guides; index =
// palette entry). Used only to pick the nearest entry for a colour.
constexpr uint32_t kPalette[128] = {
    0x000000, 0xB3B3B3, 0xDDDDDD, 0xFFFFFF, 0xFFB3B3, 0xFF6161, 0xDD6161, 0xB36161, 0xFFF3D5, 0xFFB361, 0xDD8C61, 0xB37661,
    0xFFEEA1, 0xFFFF61, 0xDDDD61, 0xB3B361, 0xDDFFA1, 0xC2FF61, 0xA1DD61, 0x81B361, 0xC2FFB3, 0x61FF61, 0x61DD61, 0x61B361,
    0xC2FFC2, 0x61FF8C, 0x61DD76, 0x61B36B, 0xC2FFCC, 0x61FFCC, 0x61DDA1, 0x61B381, 0xC2FFF3, 0x61FFE9, 0x61DDC2, 0x61B396,
    0xC2F3FF, 0x61EEFF, 0x61C7DD, 0x61A1B3, 0xA1B8D4, 0x50A5D4, 0x5086B8, 0x506B95, 0x8674D4, 0x5050D4, 0x5050B8, 0x505095,
    0x8877AA, 0x6B40AA, 0x564093, 0x4E4077, 0xAA77AA, 0xAA40AA, 0x934093, 0x774077, 0x7F596A, 0x7F3061, 0x6E3050, 0x593046,
    0x7F3B30, 0x745930, 0x6E6130, 0x505030, 0x61B361, 0x61B38C, 0x618CD5, 0x6161FF, 0x61B3B3, 0x8C61F3, 0xCCB3C2, 0x8C7681,
    0xFF6161, 0xF3FFA1, 0xEEFC61, 0xCCFF61, 0x76DD61, 0x61FFCC, 0x61E9FF, 0x61A1FF, 0x8C61FF, 0xCC61FC, 0xEE8CDD, 0xA17661,
    0xFFA161, 0xDDF961, 0xD5FF8C, 0x61FF61, 0xB3FFA1, 0xCCFCD5, 0xB3FFF6, 0xCCE4FF, 0xA1C2F6, 0xD5C2F9, 0xF98CFF, 0xFF61CC,
    0xFFC261, 0xF3EE61, 0xE4FF61, 0xDDCC61, 0xB3A161, 0x61BA76, 0x76C28C, 0x8181A1, 0x6B74AA, 0xAA8D6B, 0xB85050, 0xCF9586,
    0xCF9B62, 0xD4CA74, 0xC2CF86, 0xB1C662, 0x56566B, 0xA6A68E, 0x93A898, 0x9B9BAA, 0x988EAA, 0x777777, 0x8E8E8E, 0xA6AAAA,
    0x743030, 0x553030, 0x407B30, 0x305930, 0x797730, 0x595030, 0x776130, 0x613B30,
};

int red(uint32_t c) { return static_cast<int>((c >> 16) & 0xFF); }
int green(uint32_t c) { return static_cast<int>((c >> 8) & 0xFF); }
int blue(uint32_t c) { return static_cast<int>(c & 0xFF); }
uint32_t rgb(int r, int g, int b) {
  return static_cast<uint32_t>(std::clamp(r, 0, 255)) << 16 | static_cast<uint32_t>(std::clamp(g, 0, 255)) << 8 |
         static_cast<uint32_t>(std::clamp(b, 0, 255));
}
// A colour at `k` of its brightness (dim steps, unselected pads); never fully dark.
uint32_t dim(uint32_t c, double k) {
  auto f = [&](int v) { return std::max(4, static_cast<int>(std::lround(v * k))); };
  return rgb(f(red(c)), f(green(c)), f(blue(c)));
}
constexpr uint32_t kWhite = 0xFFFFFF;

}  // namespace

int paletteIndex(uint32_t c) {
  if (c == 0) return 0;
  // The palette shows colours lifted (pale): compare the colour lifted the same way, by hue mostly.
  const int r = red(c), g = green(c), b = blue(c);
  const int mx = std::max({r, g, b});
  const double lift = mx > 0 ? 255.0 / mx : 1.0;  // compare at full brightness
  const double rr = r * lift, gg = g * lift, bb = b * lift;
  int best = 1;
  double bestD = 1e18;
  for (int i = 1; i < 128; ++i) {
    const uint32_t p = kPalette[i];
    const int pm = std::max({red(p), green(p), blue(p)});
    const double pl = pm > 0 ? 255.0 / pm : 1.0;
    const double dr = red(p) * pl - rr, dg = green(p) * pl - gg, db = blue(p) * pl - bb;
    const double dBright = (pm - mx) * 0.6;  // then by brightness
    const double d = dr * dr * 0.30 + dg * dg * 0.59 + db * db * 0.11 + dBright * dBright;
    if (d < bestD) bestD = d, best = i;
  }
  return best;
}

uint32_t paletteColour(int index) { return index >= 0 && index < 128 ? kPalette[index] : 0; }

// ------------------------------------------------------------------- hosts ----

void TestHost::action(RemoteAction a) {
  actions.push_back(a);
  switch (a) {
    case RemoteAction::PlayPause: isPlaying = !isPlaying; break;
    case RemoteAction::Stop: isPlaying = false, pos = 0; break;
    case RemoteAction::Record: isRecording = !isRecording; break;
    case RemoteAction::SongMode: isSongMode = !isSongMode; break;
    case RemoteAction::Metronome: metronome = !metronome; break;
    case RemoteAction::Undo: doc.undo(); break;
    case RemoteAction::Redo: doc.redo(); break;
    default: break;
  }
}

// ----------------------------------------------------------------- surface ----

namespace {

double normGet(const Song& s, ParamId pid) {
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  return d && v ? d->toNormalized(*v) : 0.0;
}
void normSet(Document& doc, ParamId pid, double v) {
  auto d = describeParam(doc.song(), pid);
  if (!d || !std::isfinite(v)) return;
  doc.setParam(pid, d->fromNormalized(std::clamp(v, 0.0, 1.0)), true);
}
std::string valueText(const Song& s, ParamId pid) {
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  return d && v ? d->format(*v) : std::string();
}
// The screens show printable ASCII only.
std::string ascii(const std::string& t, size_t width) {
  std::string o;
  for (unsigned char c : t) {
    if (c >= 0x20 && c < 0x7F)
      o += static_cast<char>(c);
    else if (c >= 0x80 && (c & 0xC0) != 0x80)
      o += '?';  // one mark per non-ASCII character (UTF-8 lead byte)
  }
  if (o.size() > width) o.resize(width);
  return o;
}
constexpr double kHoldEdit = 1.0;  // a step held this long stays selected for knob edits

}  // namespace

Surface::Surface(Host& host, const Model& model, Wire wire, Out out) : host_(host), model_(model), wire_(std::move(wire)), out_(std::move(out)) {
  padPitch_.fill(-1);
  pickup_.fill(-2);
}

int Surface::padOf(int note) const {
  if (note >= wire_.padTop && note < wire_.padTop + 8) return note - wire_.padTop;
  if (note >= wire_.padBottom && note < wire_.padBottom + 8) return 8 + note - wire_.padBottom;
  return -1;
}
int Surface::padNote(int pad) const { return pad < 8 ? wire_.padTop + pad : wire_.padBottom + pad - 8; }

void Surface::sendSysex(std::initializer_list<uint8_t> body, const std::string& text) {
  std::vector<uint8_t> b = {0xF0, 0x00, 0x20, 0x29, 0x02, wire_.sysexId};
  b.insert(b.end(), body.begin(), body.end());
  for (char c : text) b.push_back(static_cast<uint8_t>(c));
  b.push_back(0xF7);
  out_(b);
}

void Surface::selectModes() {
  const uint8_t st = static_cast<uint8_t>(0xB0 | wire_.modeChannel);
  sendBytes({st, static_cast<uint8_t>(wire_.padModeCc), static_cast<uint8_t>(wire_.padDawValue)});
}

void Surface::connect() {
  connected_ = true;
  sendBytes({0x9F, 0x0C, 0x7F});  // DAW mode on
  selectModes();
  if (wire_.oled) {
    // Each knob's and fader's own temporary display: a name and a text value, shown on move and touch.
    for (int k = 0; k < 8; ++k) sendSysex({0x04, static_cast<uint8_t>(0x15 + k), 0x61});
    if (model_.faders)
      for (int k = 0; k < 9; ++k) sendSysex({0x04, static_cast<uint8_t>(0x05 + k), 0x61});
    sendSysex({0x04, 0x20, 0x02});  // the stationary display: three lines
  }
  invalidate();
}

void Surface::disconnect() {
  if (!connected_) return;
  if (model_.screen && !wire_.oled) sendSysex({0x06});  // the LCD's default display back to the keyboard
  sendBytes({0x9F, 0x0C, 0x00});  // DAW mode off: standalone again
  connected_ = false;
}

void Surface::invalidate() {
  padKnown_.fill(false);
  buttonShown_.clear();
  textKnown_.fill(false);
  screenKnown_ = false;
  scaleRoot_ = scaleType_ = -1;
}

std::pair<std::string, std::string> Surface::controlText(int k) const {
  return k >= 0 && k < 17 ? textShown_[static_cast<size_t>(k)] : std::pair<std::string, std::string>{};
}

ObjectId Surface::channelId(int rackIndex) const {
  const auto& c = host_.doc.song().channels;
  return rackIndex >= 0 && static_cast<size_t>(rackIndex) < c.size() ? c[static_cast<size_t>(rackIndex)].id : kNoId;
}

int Surface::stepCount() const {
  const Song& s = host_.doc.song();
  const Pattern* p = s.pattern(host_.currentPattern());
  const ObjectId ch = channelId(host_.selectedChannel());
  return p && ch != kNoId ? lanes::steps(*p, ch) : 0;
}

int Surface::playStep() const {
  if (!host_.playing()) return -1;
  const Song& s = host_.doc.song();
  const Pattern* p = s.pattern(host_.currentPattern());
  const ObjectId ch = channelId(host_.selectedChannel());
  if (!p || ch == kNoId) return -1;
  double t = static_cast<double>(host_.position());
  if (host_.songMode()) {  // the clip of this pattern under the play position
    bool found = false;
    for (const Clip& c : s.playlist) {
      if (c.kind != ClipKind::Pattern || c.pattern != p->id) continue;
      const Tick len = s.clipLength(c);
      if (host_.position() >= c.start && host_.position() < c.start + len) {
        t = static_cast<double>(host_.position() - c.start + c.offset);
        found = true;
        break;
      }
    }
    if (!found) return -1;
  }
  const double st = lanes::stepTicks(s, *p, ch);
  const int n = lanes::steps(*p, ch);
  if (st <= 0 || n <= 0) return -1;
  return static_cast<int>(static_cast<int64_t>(std::floor(t / st)) % n);
}

int Surface::instrumentPitch(int pad) const {
  const int row = pad < 8 ? 1 : 0;  // the bottom row plays the lower notes
  const int col = pad % 8;
  const ops::KeyAt key = ops::keyAt(host_.doc.song(), host_.position());
  const auto& sc = tools::scales();
  const int base = 60 + 12 * octave_;
  if (key.scale > 0 && static_cast<size_t>(key.scale) < sc.size()) {
    const auto& steps = sc[static_cast<size_t>(key.scale)].steps;
    const int n = static_cast<int>(steps.size());
    const int degree = row * 8 + col;  // two octaves of the scale, root at the bottom left
    return base + key.root + 12 * (degree / n) + steps[static_cast<size_t>(degree % n)];
  }
  return base + row * 8 + col;
}

std::array<uint32_t, 16> Surface::drawPads() const {
  std::array<uint32_t, 16> c{};
  const Song& s = host_.doc.song();
  switch (page_) {
    case PadPage::Steps: {
      const Pattern* p = s.pattern(host_.currentPattern());
      const ObjectId ch = channelId(host_.selectedChannel());
      const Channel* chan = s.channel(ch);
      if (!p || !chan) break;
      const int n = lanes::steps(*p, ch);
      const auto it = p->steps.find(ch);
      const int play = playStep();
      for (int i = 0; i < 16; ++i) {
        const int st = stepPage_ * 16 + i;
        if (st >= n) continue;
        const bool on = it != p->steps.end() && static_cast<size_t>(st) < it->second.size() && it->second[static_cast<size_t>(st)] > 0;
        c[static_cast<size_t>(i)] = st == play ? kWhite : heldSince_[static_cast<size_t>(i)] > 0 ? dim(kWhite, 0.55) : on ? chan->color : dim(chan->color, 0.12);
      }
      break;
    }
    case PadPage::Channels: {
      const int sel = host_.selectedChannel();
      for (int i = 0; i < 16; ++i) {  // bottom row first (it lines up with the knobs), then the top row
        const int idx = channelBank_ + (i < 8 ? 8 + i : i - 8);
        const Channel* ch = s.channel(channelId(idx));
        if (ch) c[static_cast<size_t>(i)] = idx == sel ? kWhite : ch->color;
      }
      break;
    }
    case PadPage::Patterns: {
      const ObjectId cur = host_.currentPattern();
      for (int i = 0; i < 16; ++i) {
        const int idx = patternBank_ * 8 + i;  // the bank: eight patterns
        if (static_cast<size_t>(idx) < s.patterns.size()) {
          const Pattern& p = s.patterns[static_cast<size_t>(idx)];
          c[static_cast<size_t>(i)] = p.id == cur ? kWhite : p.color;
        } else if (static_cast<size_t>(idx) == s.patterns.size()) {
          c[static_cast<size_t>(i)] = dim(kWhite, 0.15);  // a new pattern
        }
      }
      break;
    }
    case PadPage::Instrument: {
      const Channel* chan = s.channel(channelId(host_.selectedChannel()));
      if (!chan) break;
      const ops::KeyAt key = ops::keyAt(s, host_.position());
      for (int i = 0; i < 16; ++i) {
        const int pitch = instrumentPitch(i);
        if (pitch < 0 || pitch > 127) continue;
        const bool root = ((pitch - (key.scale > 0 ? key.root : 0)) % 12 + 12) % 12 == 0;
        c[static_cast<size_t>(i)] = padPitch_[static_cast<size_t>(i)] >= 0 ? kWhite : root ? chan->color : dim(chan->color, 0.25);
      }
      break;
    }
    case PadPage::Device: break;
  }
  return c;
}

uint32_t Surface::buttonColour(Button b) const {
  const Song& s = host_.doc.song();
  switch (b) {
    case Button::ChannelUp: return host_.selectedChannel() > 0 ? kWhite : 0;
    case Button::ChannelDown: return host_.selectedChannel() + 1 < static_cast<int>(s.channels.size()) ? kWhite : 0;
    case Button::PageLeft: return (page_ == PadPage::Steps && stepPage_ > 0) || (page_ == PadPage::Instrument && octave_ > -5) ? kWhite : 0;
    case Button::PageRight: return page_ == PadPage::Steps && (stepPage_ + 1) * 16 < stepCount() ? kWhite : page_ == PadPage::Instrument ? kWhite : 0;
    case Button::Solo: return soloMode_ ? kWhite : dim(kWhite, 0.2);
    case Button::Play: return host_.playing() ? 0x00FF00 : 0;
    case Button::Record: return host_.recording() ? 0xFF0000 : 0;
    case Button::PatternSong: return host_.songMode() ? kWhite : 0;
    default: return 0;
  }
}

Surface::Target Surface::knobTarget(int k) const { return target(knobMode_, k); }

Surface::Target Surface::target(KnobMode mode, int k) const {
  Target t;
  const Song& s = host_.doc.song();
  switch (mode) {
    case KnobMode::MixerVolume:
    case KnobMode::MixerPan: {
      const size_t m = static_cast<size_t>(1 + mixerBank_ + k);
      if (m < s.mixer.size()) {
        t.pid = mixerParam(s.mixer[m].id, mode == KnobMode::MixerVolume ? MixerParam::Volume : MixerParam::Pan);
        t.name = s.mixer[m].name;
      }
      break;
    }
    case KnobMode::ChannelVolume:
    case KnobMode::ChannelPan: {
      const Channel* c = s.channel(channelId(channelBank_ + k));
      if (c) {
        t.pid = channelParam(c->id, mode == KnobMode::ChannelVolume ? ChannelParam::Volume : ChannelParam::Pan);
        t.name = c->name;
      }
      break;
    }
    case KnobMode::Plugin: {
      // The focused plugin's parameters, eight per bank; the last bank: the channel's macro knobs.
      const auto [owner, slot] = host_.focusedPlugin();
      const PluginState* st = nullptr;
      if (slot < 0) {
        if (const Channel* c = s.channel(owner)) st = &c->plugin;
      } else if (const MixerTrack* mt = s.mixerTrack(owner); mt && static_cast<size_t>(slot) < mt->slots.size()) {
        st = &mt->slots[static_cast<size_t>(slot)].plugin;
      }
      const int params = st ? static_cast<int>(st->params.size()) : 0;
      const int banks = (params + 7) / 8;
      const int i = knobBank_ * 8 + k;
      if (knobBank_ < banks) {
        if (i < params) t.pid = slot < 0 ? channelPluginParam(owner, i) : slotParam(owner, slot, i);
      } else if (slot < 0 && s.channel(owner)) {
        t.pid = layerMacroParam(owner, k);
        t.macro = true;
      }
      if (t.pid.owner != kNoId) {
        auto d = describeParam(s, t.pid);
        t.name = d ? d->name : "";
        if (t.macro) {
          const Channel* c = s.channel(owner);
          t.name = c && !c->macros[static_cast<size_t>(k)].name.empty() ? c->macros[static_cast<size_t>(k)].name : "Macro " + std::to_string(k + 1);
        }
      }
      break;
    }
    case KnobMode::Transport: {
      static const char* names[8] = {"Position", "", "", "", "Marker", "", "", "Tempo"};
      t.name = names[k];
      if (k == 7) t.pid = songParam(SongParam::Tempo);
      break;
    }
    case KnobMode::Device: break;
  }
  return t;
}

Surface::Target Surface::faderTarget(int k) const {
  Target t;
  const Song& s = host_.doc.song();
  if (k == 8) {  // the ninth fader: the master
    if (faderMode_ != FaderMode::Device && faderMode_ != FaderMode::Plugin && !s.mixer.empty()) {
      t.pid = mixerParam(s.mixer[0].id, MixerParam::Volume);
      t.name = s.mixer[0].name;
    }
    return t;
  }
  switch (faderMode_) {
    case FaderMode::MixerVolume: {
      const size_t m = static_cast<size_t>(1 + mixerBank_ + k);
      if (m < s.mixer.size()) t.pid = mixerParam(s.mixer[m].id, MixerParam::Volume), t.name = s.mixer[m].name;
      break;
    }
    case FaderMode::ChannelVolume:
      if (const Channel* c = s.channel(channelId(channelBank_ + k))) t.pid = channelParam(c->id, ChannelParam::Volume), t.name = c->name;
      break;
    case FaderMode::Plugin: t = target(KnobMode::Plugin, k); break;
    case FaderMode::Device: break;
  }
  return t;
}

std::pair<std::string, std::string> Surface::knobLabel(int k) const {
  const Target t = knobTarget(k);
  return {t.name, t.pid.owner != kNoId || t.pid.index != 0 ? valueText(host_.doc.song(), t.pid) : std::string()};
}

void Surface::flash(const std::string& line1, const std::string& line2) {
  flashTop_ = line1;
  flashBottom_ = line2;
  flashUntil_ = host_.now() + 1.5;
  screenKnown_ = false;
  if (wire_.oled) {  // the global temporary display, brought up now
    sendSysex({0x04, 0x21, 0x01});
    sendSysex({0x06, 0x21, 0x00}, ascii(line1, 20));
    sendSysex({0x06, 0x21, 0x01}, ascii(line2, 20));
    sendSysex({0x04, 0x21, 0x7F});
  }
}

void Surface::showControl(int idx, const Target& t) {
  // idx 0..7 knobs, 8..16 faders. The name and the value as the screen shows them on a move.
  const std::string name = ascii(t.name, 16);
  const std::string value = t.pid.owner != kNoId || t.pid.index != 0 ? ascii(valueText(host_.doc.song(), t.pid), 16) : std::string();
  auto& shown = textShown_[static_cast<size_t>(idx)];
  bool& known = textKnown_[static_cast<size_t>(idx)];
  if (!model_.screen) {
    shown = {name, value};
    known = true;
    return;
  }
  const uint8_t target = wire_.oled ? static_cast<uint8_t>(idx < 8 ? 0x15 + idx : 0x05 + idx - 8)
                                    : static_cast<uint8_t>(idx < 8 ? 0x38 + idx : 0x50 + idx - 8);
  if (!known || shown.first != name) {
    if (wire_.oled)
      sendSysex({0x06, target, 0x00}, name);
    else
      sendSysex({0x07, target}, name);
  }
  if (!known || shown.second != value) {
    if (wire_.oled)
      sendSysex({0x06, target, 0x01}, value);
    else
      sendSysex({0x08, target}, value);
  }
  shown = {name, value};
  known = true;
}

void Surface::drawScreen() {
  if (!model_.screen) return;
  const Song& s = host_.doc.song();
  std::string top, bottom;
  if (host_.now() < flashUntil_) {
    top = flashTop_, bottom = flashBottom_;
  } else {
    const Pattern* p = s.pattern(host_.currentPattern());
    const Channel* c = s.channel(channelId(host_.selectedChannel()));
    top = p ? p->name : "No pattern";
    bottom = c ? c->name : "No channel";
    if (page_ != PadPage::Device) bottom = std::string(padPageName(page_)).substr(0, 4) + " " + bottom;
  }
  if (wire_.oled) {
    top = ascii(top, 20), bottom = ascii(bottom, 20);
    if (screenKnown_ && top == screenTop_ && bottom == screenBottom_) return;
    sendSysex({0x06, 0x20, 0x00}, "Gridline");
    sendSysex({0x06, 0x20, 0x01}, top);
    sendSysex({0x06, 0x20, 0x02}, bottom);
  } else {
    top = ascii(top, 16), bottom = ascii(bottom, 16);
    if (screenKnown_ && top == screenTop_ && bottom == screenBottom_) return;
    if (!screenKnown_ || top != screenTop_) sendSysex({0x04, 0x00}, top);
    if (!screenKnown_ || bottom != screenBottom_) sendSysex({0x04, 0x01}, bottom);
  }
  screenTop_ = top, screenBottom_ = bottom;
  screenKnown_ = true;
}

void Surface::syncScale() {
  const ops::KeyAt key = ops::keyAt(host_.doc.song(), host_.position());
  if (key.marker < 0 || key.scale <= 0) return;  // no key set in the song: the keyboard keeps its own
  const auto& sc = tools::scales();
  if (static_cast<size_t>(key.scale) >= sc.size()) return;
  std::string want = squash(sc[static_cast<size_t>(key.scale)].name);
  if (want == "naturalminor") want = "minor";
  if (want == "halfwhole") want = "halfwholedim";
  int type = -1;
  for (size_t i = 0; i < wire_.scaleNames.size(); ++i)
    if (squash(wire_.scaleNames[i]) == want) type = static_cast<int>(i);
  const uint8_t st = static_cast<uint8_t>(0xB0 | wire_.scaleChannel);
  if (key.root != scaleRoot_) sendBytes({st, static_cast<uint8_t>(wire_.scaleRootCc), static_cast<uint8_t>(key.root)});
  if (type >= 0 && type != scaleType_) sendBytes({st, static_cast<uint8_t>(wire_.scaleTypeCc), static_cast<uint8_t>(type)});
  scaleRoot_ = key.root;
  if (type >= 0) scaleType_ = type;
}

void Surface::idle(double now) {
  if (!connected_) return;
  if (lastIdle_ >= 0 && now - lastIdle_ < 1.0 / 30.0) return;
  lastIdle_ = now;
  // Steps held long enough stay selected for knob edits (latch); nothing to send for that.
  // Pads.
  const auto want = drawPads();
  for (size_t i = 0; i < 16; ++i) {
    if (padKnown_[i] && padShown_[i] == want[i]) continue;
    const uint8_t note = static_cast<uint8_t>(padNote(static_cast<int>(i)));
    if (wire_.rgb) {
      const uint32_t c = want[i];
      sendSysex({0x01, 0x43, note, static_cast<uint8_t>(red(c) >> 1), static_cast<uint8_t>(green(c) >> 1), static_cast<uint8_t>(blue(c) >> 1)});
    } else {
      sendBytes({0x90, note, static_cast<uint8_t>(paletteIndex(want[i]))});
    }
    padShown_[i] = want[i];
    padKnown_[i] = true;
  }
  // Buttons with a light.
  for (Button b : {Button::ChannelUp, Button::ChannelDown, Button::PageLeft, Button::PageRight, Button::Solo, Button::Play, Button::Record,
                   Button::PatternSong}) {
    const int cc = wire_.buttons[static_cast<size_t>(b)];
    if (cc < 0) continue;
    const int v = paletteIndex(buttonColour(b));
    auto it = buttonShown_.find(cc);
    if (it != buttonShown_.end() && it->second == v) continue;
    sendBytes({0xB0, static_cast<uint8_t>(cc), static_cast<uint8_t>(v)});
    buttonShown_[cc] = v;
  }
  // Fader buttons: mute / solo state in the track's colour.
  if (model_.faders) {
    const Song& s = host_.doc.song();
    for (int k = 0; k < 8; ++k) {
      uint32_t c = 0;
      if (faderMode_ == FaderMode::MixerVolume) {
        const size_t m = static_cast<size_t>(1 + mixerBank_ + k);
        if (m < s.mixer.size()) {
          const MixerTrack& t = s.mixer[m];
          c = soloMode_ ? (t.solo ? t.color : dim(t.color, 0.12)) : (t.muted ? 0 : t.color);
        }
      } else if (faderMode_ == FaderMode::ChannelVolume) {
        if (const Channel* ch = s.channel(channelId(channelBank_ + k))) c = soloMode_ ? (ch->muted ? dim(ch->color, 0.12) : ch->color) : (ch->muted ? 0 : ch->color);
      }
      const int cc = wire_.faderButtonCc + k;
      const int code = wire_.rgb ? static_cast<int>(c | 1u << 24) : paletteIndex(c);
      auto it = buttonShown_.find(cc);
      if (it != buttonShown_.end() && it->second == code) continue;
      if (wire_.rgb)
        sendSysex({0x01, 0x53, static_cast<uint8_t>(cc), static_cast<uint8_t>(red(c) >> 1), static_cast<uint8_t>(green(c) >> 1),
                   static_cast<uint8_t>(blue(c) >> 1)});
      else
        sendBytes({0xB0, static_cast<uint8_t>(cc), static_cast<uint8_t>(code)});
      buttonShown_[cc] = code;
    }
  }
  // Knob names and values (the screen shows them when a knob moves), and on FLkey 2 the endless
  // encoders' positions, so a turn starts from the value the control has now.
  for (int k = 0; k < 8; ++k) {
    const Target t = knobTarget(k);
    showControl(k, t);
    if (wire_.oled && knobMode_ != KnobMode::Transport && knobMode_ != KnobMode::Device) {
      const int v = t.pid.owner != kNoId || t.pid.index != 0 ? static_cast<int>(std::lround(normGet(host_.doc.song(), t.pid) * 127)) : 0;
      const int cc = 0x1000 + wire_.knobCc + k;  // a key of its own in buttonShown_
      auto it = buttonShown_.find(cc);
      if (it == buttonShown_.end() || it->second != v) {
        sendBytes({0xBF, static_cast<uint8_t>(wire_.knobCc + k), static_cast<uint8_t>(v)});
        buttonShown_[cc] = v;
      }
    }
  }
  if (model_.faders)
    for (int k = 0; k < 9; ++k) showControl(8 + k, faderTarget(k));
  drawScreen();
  syncScale();
}

void Surface::moveChannel(int by) {
  const int n = static_cast<int>(host_.doc.song().channels.size());
  if (n == 0) return;
  const int sel = std::clamp(host_.selectedChannel() + by, 0, n - 1);
  host_.selectChannel(sel);
  channelBank_ = sel / 8 * 8;
  const Channel* c = host_.doc.song().channel(channelId(sel));
  if (c) flash("Channel " + std::to_string(sel + 1), c->name);
  for (auto& p : pickup_) p = -2;
}

void Surface::stepPreset(int by) {
  const std::string name = host_.stepPreset(by);
  flash("Preset", name.empty() ? std::string("No presets") : name);
}

void Surface::moveMixer(int by) {
  const int inserts = std::max(0, static_cast<int>(host_.doc.song().mixer.size()) - 1);
  if (inserts == 0) return;
  int sel = std::max(1, host_.selectedMixer());
  if (by == 8 || by == -8) {  // a bank of eight
    mixerBank_ = std::clamp(mixerBank_ + by, 0, std::max(0, (inserts - 1) / 8 * 8));
    sel = std::min(inserts, mixerBank_ + 1);
  } else {
    sel = std::clamp(sel + by, 1, inserts);
    mixerBank_ = (sel - 1) / 8 * 8;
  }
  host_.selectMixer(sel);
  const auto& mx = host_.doc.song().mixer;
  flash("Mixer " + std::to_string(mixerBank_ + 1) + "-" + std::to_string(std::min(inserts, mixerBank_ + 8)), mx[static_cast<size_t>(sel)].name);
  for (auto& p : pickup_) p = -2;
}

void Surface::button(Button b, bool down) {
  if (down) logging::verbose("flkey", std::string(model_.name) + ": button " + buttonId(b) + (shift_ ? " (with Shift)" : ""));
  if (b == Button::Shift) {
    shift_ = down;
    return;
  }
  if (!down) return;
  Document& doc = host_.doc;
  const Song& s = doc.song();
  auto cyclePage = [&](int by) {
    static const PadPage order[] = {PadPage::Steps, PadPage::Channels, PadPage::Patterns, PadPage::Instrument};
    int i = 0;
    while (i < 4 && order[i] != (page_ == PadPage::Device ? lastHostPage_ : page_)) ++i;
    page_ = lastHostPage_ = order[((i + by) % 4 + 4) % 4];
    flash("Pads", padPageName(page_));
  };
  switch (b) {
    case Button::Play: host_.action(RemoteAction::PlayPause); break;
    case Button::Stop: host_.action(RemoteAction::Stop); break;
    case Button::Record: host_.action(RemoteAction::Record); break;
    case Button::Loop: host_.action(RemoteAction::LoopRecord); break;
    case Button::Metronome: host_.action(RemoteAction::Metronome); break;
    case Button::Undo: host_.action(shift_ ? RemoteAction::Redo : RemoteAction::Undo); break;
    case Button::Redo: host_.action(RemoteAction::Redo); break;
    case Button::PatternSong: host_.action(RemoteAction::SongMode); break;
    case Button::ScoreLog:
      if (!shift_) {
        const int n = host_.scoreLog();
        flash("Score log", n > 0 ? std::to_string(n) + " notes" : "No notes");
        break;
      }
      [[fallthrough]];  // Shift + score log: quantize
    case Button::Quantize: {
      const Pattern* p = s.pattern(host_.currentPattern());
      const ObjectId ch = channelId(host_.selectedChannel());
      if (!p || ch == kNoId) break;
      const Tick grid = std::max<Tick>(1, s.ticksPerStep(*p));
      doc.editPattern(p->id, "quantize notes", [&](Pattern& x) {
        if (auto it = x.notes.find(ch); it != x.notes.end()) ops::quantizeStarts(it->second, grid);
      });
      flash("Quantize", "Note starts");
      break;
    }
    case Button::ChannelUp:
    case Button::ChannelDown: {
      const int by = b == Button::ChannelUp ? -1 : 1;
      const bool second = model_.gen == Gen::Second;
      if (shift_ && second) {  // FLkey 2 guides: Channel Rack (Preset): "hold shift to change Preset"
        stepPreset(by);
      } else if (shift_ && wire_.buttons[static_cast<size_t>(Button::PageLeft)] < 0) {
        cyclePage(by);  // no page buttons (first FLkey Mini): Shift + up / down picks the pad page
      } else if (page_ == PadPage::Patterns) {
        // FLkey 2 guides (Patterns Pad Mode): up / down move the patterns shown by eight
        patternBank_ = std::clamp(patternBank_ + by, 0, static_cast<int>(s.patterns.size()) / 8);
      } else if (page_ == PadPage::Channels) {
        // Channel Rack Banking: the first FLkey by groups of eight, the FLkey 2 by one channel
        const int n = static_cast<int>(s.channels.size());
        const int step = second ? 1 : 8;
        channelBank_ = std::clamp(channelBank_ + step * by, 0, std::max(0, second ? n - 16 : (n - 1) / 8 * 8));
      } else {
        moveChannel(by);
      }
      break;
    }
    case Button::MixerLeft:
    case Button::MixerRight: {
      // FLkey 2: banks of eight, Shift one track. First FLkey (Pot Mode: Banking): eight in the
      // Channel Rack pad mode, one track in the other pad modes.
      const bool eight = model_.gen == Gen::Second ? !shift_ : (!shift_ && page_ == PadPage::Channels);
      moveMixer((b == Button::MixerLeft ? -1 : 1) * (eight ? 8 : 1));
      break;
    }
    case Button::PageLeft:
    case Button::PageRight: {
      const int by = b == Button::PageLeft ? -1 : 1;
      if (shift_ && by > 0 && page_ == PadPage::Patterns && model_.gen == Gen::Second) {
        // FLkey 2 guides (Patterns Pad Mode): Shift + Page down clones the selected pattern
        const ObjectId id = doc.clonePattern(host_.currentPattern());
        if (id != kNoId) {
          host_.selectPattern(id);
          flash("Clone pattern", doc.song().pattern(id)->name);
        }
        break;
      }
      if (shift_) {
        cyclePage(by);
        break;
      }
      switch (page_) {
        case PadPage::Steps: {
          const int pages = std::max(1, (stepCount() + 15) / 16);
          stepPage_ = std::clamp(stepPage_ + by, 0, pages - 1);
          flash("Steps", std::to_string(stepPage_ * 16 + 1) + "-" + std::to_string(stepPage_ * 16 + 16));
          break;
        }
        case PadPage::Channels: {
          const int n = static_cast<int>(s.channels.size());
          channelBank_ = std::clamp(channelBank_ + 8 * by, 0, std::max(0, (n - 1) / 8 * 8));
          break;
        }
        case PadPage::Patterns:
          patternBank_ = std::clamp(patternBank_ + 2 * by, 0, static_cast<int>(s.patterns.size()) / 8);  // 16 pads
          break;
        case PadPage::Instrument:
          octave_ = std::clamp(octave_ + by, -5, 4);
          flash("Octave", (octave_ > 0 ? "+" : "") + std::to_string(octave_));
          break;
        case PadPage::Device: break;
      }
      break;
    }
    case Button::KnobBankUp:
    case Button::KnobBankDown: {
      const int by = b == Button::KnobBankUp ? -1 : 1;
      if (model_.gen == Gen::First && !shift_) {  // first FLkey guides: Preset ▲▼ navigate the plugin presets
        stepPreset(by);
        break;
      }
      // FLkey 2: the encoder bank buttons; first FLkey: Shift + Preset ▲▼ (not in its guide)
      if (knobMode_ == KnobMode::MixerVolume || knobMode_ == KnobMode::MixerPan)
        knobMode_ = knobMode_ == KnobMode::MixerVolume ? KnobMode::MixerPan : KnobMode::MixerVolume;
      else if (knobMode_ == KnobMode::ChannelVolume || knobMode_ == KnobMode::ChannelPan)
        knobMode_ = knobMode_ == KnobMode::ChannelVolume ? KnobMode::ChannelPan : KnobMode::ChannelVolume;
      else if (knobMode_ == KnobMode::Plugin)
        knobBank_ = std::clamp(knobBank_ + by, 0, 64);
      flash("Knobs", knobMode_ == KnobMode::Plugin ? "Bank " + std::to_string(knobBank_ + 1) : knobModeName(knobMode_));
      for (auto& p : pickup_) p = -2;
      break;
    }
    case Button::Solo: {
      const double t = host_.now();
      if (t - lastSolo_ < 0.4) {  // a double tap: every track or channel of the faders plays again
        doc.beginGroup("unmute all");
        if (faderMode_ == FaderMode::ChannelVolume) {
          for (const Channel& c : s.channels)
            if (c.muted) doc.setParam(channelParam(c.id, ChannelParam::Mute), 0.0, false);
        } else {
          for (const MixerTrack& m : s.mixer) {
            if (m.muted) doc.setParam(mixerParam(m.id, MixerParam::Mute), 0.0, false);
            if (m.solo) doc.editMixerTrack(m.id, "unsolo track", [](MixerTrack& x) { x.solo = false; });
          }
        }
        doc.endGroup();
        soloMode_ = false;
        lastSolo_ = -10;
        flash("Solo", "All playing");
      } else {
        soloMode_ = !soloMode_;
        lastSolo_ = t;
      }
      break;
    }
    default: break;
  }
}

void Surface::faderButton(int k) {
  Document& doc = host_.doc;
  const Song& s = doc.song();
  if (faderMode_ == FaderMode::MixerVolume) {
    const size_t m = static_cast<size_t>(1 + mixerBank_ + k);
    if (m >= s.mixer.size()) return;
    const MixerTrack& t = s.mixer[m];
    if (soloMode_) {
      const bool on = !t.solo;
      doc.editMixerTrack(t.id, on ? "solo track" : "unsolo track", [on](MixerTrack& x) { x.solo = on; });
    } else {
      doc.setParam(mixerParam(t.id, MixerParam::Mute), t.muted ? 0.0 : 1.0, false);
    }
  } else if (faderMode_ == FaderMode::ChannelVolume) {
    const Channel* c = s.channel(channelId(channelBank_ + k));
    if (!c) return;
    if (soloMode_) {  // this channel alone (one step); pressed again on the soloed one: all play
      const ObjectId id = c->id;
      bool alone = !c->muted;
      for (const Channel& o : s.channels)
        if (o.id != id && !o.muted) alone = false;
      doc.beginGroup(alone ? "unsolo channel" : "solo channel");
      for (const Channel& o : s.channels) {
        const bool mute = !alone && o.id != id;
        if (o.muted != mute) doc.setParam(channelParam(o.id, ChannelParam::Mute), mute ? 1.0 : 0.0, false);
      }
      doc.endGroup();
    } else {
      doc.setParam(channelParam(c->id, ChannelParam::Mute), c->muted ? 0.0 : 1.0, false);
    }
  }
}

void Surface::pressPad(int pad, int velocity) {
  Document& doc = host_.doc;
  const Song& s = doc.song();
  switch (page_) {
    case PadPage::Steps: {
      const Pattern* p = s.pattern(host_.currentPattern());
      const ObjectId ch = channelId(host_.selectedChannel());
      const int st = stepPage_ * 16 + pad;
      if (!p || ch == kNoId || st >= lanes::steps(*p, ch)) return;
      bool anyHeld = false;
      for (double h : heldSince_) anyHeld = anyHeld || h > 0;
      if (!anyHeld) stepEdited_ = false;
      heldSince_[static_cast<size_t>(pad)] = std::max(1e-6, host_.now());
      const auto it = p->steps.find(ch);
      const bool on = it != p->steps.end() && static_cast<size_t>(st) < it->second.size() && it->second[static_cast<size_t>(st)] > 0;
      if (!on) {
        doc.setStep(p->id, ch, st, 100);  // an empty step turns on at once (it is then held for edits)
        stepEdited_ = true;
      }
      break;
    }
    case PadPage::Channels: {
      const int idx = channelBank_ + (pad < 8 ? 8 + pad : pad - 8);
      const ObjectId id = channelId(idx);
      if (id == kNoId) return;
      host_.selectChannel(idx);
      host_.note(id, 60, std::max(1, velocity));
      padPitch_[static_cast<size_t>(pad)] = 60;
      flash("Channel " + std::to_string(idx + 1), s.channel(id)->name);
      break;
    }
    case PadPage::Patterns: {
      const int idx = patternBank_ * 8 + pad;
      if (static_cast<size_t>(idx) < s.patterns.size()) {
        host_.selectPattern(s.patterns[static_cast<size_t>(idx)].id);
        flash("Pattern", s.patterns[static_cast<size_t>(idx)].name);
      } else if (static_cast<size_t>(idx) == s.patterns.size()) {
        const ObjectId id = doc.addPattern("Pattern " + std::to_string(s.patterns.size() + 1));
        if (id != kNoId) {
          host_.selectPattern(id);
          flash("New pattern", doc.song().pattern(id)->name);
        }
      }
      break;
    }
    case PadPage::Instrument: {
      const ObjectId id = channelId(host_.selectedChannel());
      const int pitch = instrumentPitch(pad);
      if (id == kNoId || pitch < 0 || pitch > 127) return;
      host_.note(id, pitch, std::max(1, velocity));
      padPitch_[static_cast<size_t>(pad)] = pitch;
      break;
    }
    case PadPage::Device: break;
  }
}

void Surface::releasePad(int pad) {
  const size_t i = static_cast<size_t>(pad);
  if (page_ == PadPage::Steps && heldSince_[i] > 0) {
    const bool latched = host_.now() - heldSince_[i] >= kHoldEdit;
    heldSince_[i] = 0;
    if (!stepEdited_ && !latched) {  // a short press on a lit step: off
      const Song& s = host_.doc.song();
      const Pattern* p = s.pattern(host_.currentPattern());
      const ObjectId ch = channelId(host_.selectedChannel());
      if (p && ch != kNoId) host_.doc.setStep(p->id, ch, stepPage_ * 16 + pad, 0);
    }
    return;
  }
  if (padPitch_[i] >= 0) {
    const ObjectId id = channelId(host_.selectedChannel());
    if (id != kNoId) host_.note(id, padPitch_[i], 0);
    padPitch_[i] = -1;
  }
}

void Surface::stepKnob(int k, int value) {
  // A held step: the knobs set its properties (pitch, velocity, release, fine, pan, mod X, mod Y, shift).
  Document& doc = host_.doc;
  const Pattern* p = doc.song().pattern(host_.currentPattern());
  const ObjectId ch = channelId(host_.selectedChannel());
  if (!p || ch == kNoId) return;
  static const char* names[8] = {"Pitch", "Velocity", "Release", "Fine", "Pan", "Mod X", "Mod Y", "Shift"};
  std::string shown;
  const uint64_t merge = 0xF1E70000ull + static_cast<uint64_t>(k);
  doc.editPattern(p->id, "edit step", [&](Pattern& x) {
    auto& row = x.steps[ch];
    auto& props = x.stepProps[ch];
    const int n = lanes::steps(x, ch);
    if (static_cast<int>(row.size()) < n) row.resize(static_cast<size_t>(n), 0);
    if (static_cast<int>(props.size()) < n) props.resize(static_cast<size_t>(n));
    for (int pad = 0; pad < 16; ++pad) {
      if (heldSince_[static_cast<size_t>(pad)] <= 0) continue;
      const int st = stepPage_ * 16 + pad;
      if (st >= n) continue;
      StepProps& sp = props[static_cast<size_t>(st)];
      switch (k) {
        case 0: sp.pitch = value, shown = tools::pitchName(value); break;
        case 1: row[static_cast<size_t>(st)] = static_cast<uint8_t>(std::max(1, value)), shown = std::to_string(std::max(1, value)); break;
        case 2: sp.release = value, shown = std::to_string(value); break;
        case 3: sp.fine = (value - 64) * 100 / 64, shown = std::to_string(sp.fine) + " ct"; break;
        case 4: sp.pan = std::clamp(value - 64, -64, 64), shown = std::to_string(sp.pan); break;
        case 5: sp.modX = value * 2, shown = std::to_string(sp.modX); break;
        case 6: sp.modY = value * 2, shown = std::to_string(sp.modY); break;
        case 7: sp.shift = value * kStepShiftMax / 127, shown = std::to_string(sp.shift) + "%"; break;
        default: break;
      }
    }
  }, merge);
  stepEdited_ = true;
  flash(names[k], shown);
}

void Surface::turnKnob(int k, int value) {
  for (double h : heldSince_)
    if (h > 0 && page_ == PadPage::Steps) return stepKnob(k, value);
  const Target t = knobTarget(k);
  if (t.pid.owner == kNoId && t.pid.index == 0) return;
  const double v = value / 127.0;
  if (model_.gen == Gen::First) {  // potentiometers: pick the value up before moving it
    int& pk = pickup_[static_cast<size_t>(k)];
    const double cur = normGet(host_.doc.song(), t.pid);
    if (pk != -1) {
      const int side = v < cur - 1.5 / 127 ? 0 : v > cur + 1.5 / 127 ? 1 : 2;
      if (pk == -2 && side != 2) {
        pk = side;
      } else if (side == 2 || side != pk) {
        pk = -1;  // reached (or crossed) the value
      }
      if (pk != -1) return showControl(k, t);
    }
  }
  normSet(host_.doc, t.pid, v);
  showControl(k, t);
}

void Surface::transportKnob(int k, int delta) {
  const Song& s = host_.doc.song();
  if (k == 0) {  // the play position, a sixteenth per detent
    host_.setPosition(std::max<Tick>(0, host_.position() + delta * static_cast<Tick>(s.ppq / 4)));
  } else if (k == 4 && !s.markers.empty()) {  // to the next / previous marker
    const Tick pos = host_.position();
    Tick to = pos;
    if (delta > 0) {
      for (const Marker& m : s.markers)
        if (m.pos > pos) {
          to = m.pos;
          break;
        }
    } else {
      for (auto it = s.markers.rbegin(); it != s.markers.rend(); ++it)
        if (it->pos < pos) {
          to = it->pos;
          break;
        }
    }
    host_.setPosition(to);
  } else if (k == 7) {
    host_.doc.setParam(songParam(SongParam::Tempo), std::clamp(s.tempo + delta * (shift_ ? 0.1 : 1.0), 10.0, 522.0), true);
  }
  showControl(k, knobTarget(k));
}

void Surface::moveFader(int k, int value) {
  const Target t = faderTarget(k);
  if (t.pid.owner == kNoId && t.pid.index == 0) return;
  const double v = value / 127.0;
  int& pk = pickup_[static_cast<size_t>(8 + k)];
  const double cur = normGet(host_.doc.song(), t.pid);
  if (pk != -1) {
    const int side = v < cur - 1.5 / 127 ? 0 : v > cur + 1.5 / 127 ? 1 : 2;
    if (pk == -2 && side != 2)
      pk = side;
    else if (side == 2 || side != pk)
      pk = -1;
    if (pk != -1) return showControl(8 + k, t);
  }
  normSet(host_.doc, t.pid, v);
  showControl(8 + k, t);
}

bool Surface::onMidi(const MidiMessage& m) {
  const int kind = m.kind();
  // HUI (FLkey 2's documented mode for other software): a ping reply or a zone / port select.
  if ((kind == 0x90 && m.channel() == 0 && m.d1 == 0x00 && m.d2 == 0x7F) || (kind == 0xB0 && m.channel() == 0 && (m.d1 == 0x0F || m.d1 == 0x2F))) {
    huiSeen_ = true;
    return true;
  }
  if (kind == 0x90 || kind == 0x80) {
    const int pad = m.channel() == 0 ? padOf(m.d1) : -1;
    if (pad < 0) return false;
    if (m.noteOn())
      pressPad(pad, m.d2);
    else
      releasePad(pad);
    return true;
  }
  if (kind == 0xA0) return m.channel() == 0 && padOf(m.d1) >= 0;  // pad pressure: the DAW layout has no use for it
  if (kind != 0xB0) return false;
  const int ch = m.channel();
  // Mode reports.
  if (ch == wire_.modeChannel && (m.d1 == wire_.padModeCc || m.d1 == wire_.knobModeCc || m.d1 == wire_.faderModeCc)) {
    if (m.d1 == wire_.padModeCc) {
      if (m.d2 == wire_.padDawValue) {
        page_ = lastHostPage_;
      } else if (auto it = wire_.padModes.find(m.d2); it != wire_.padModes.end()) {
        if (page_ != PadPage::Device) lastHostPage_ = page_;
        page_ = it->second;
        if (page_ != PadPage::Device) lastHostPage_ = page_;
      } else {
        logging::info("flkey", std::string(model_.name) + ": unhandled pad mode " + std::to_string(m.d2));
      }
    } else if (m.d1 == wire_.knobModeCc) {
      if (auto it = wire_.knobModes.find(m.d2); it != wire_.knobModes.end())
        knobMode_ = it->second, knobBank_ = 0;
      else
        logging::info("flkey", std::string(model_.name) + ": unhandled knob mode " + std::to_string(m.d2));
      for (auto& p : pickup_) p = -2;
    } else {
      if (auto it = wire_.faderModes.find(m.d2); it != wire_.faderModes.end())
        faderMode_ = it->second;
      else
        logging::info("flkey", std::string(model_.name) + ": unhandled fader mode " + std::to_string(m.d2));
      for (auto& p : pickup_) p = -2;
    }
    return true;
  }
  if (ch == 15) {
    if (m.d1 >= wire_.knobCc && m.d1 < wire_.knobCc + 8) {
      turnKnob(m.d1 - wire_.knobCc, m.d2);
      return true;
    }
    if (wire_.relativeTransport && m.d1 >= wire_.transportCc && m.d1 < wire_.transportCc + 8) {
      transportKnob(m.d1 - wire_.transportCc, static_cast<int>(m.d2) - 64);
      return true;
    }
    if (model_.faders && m.d1 >= wire_.faderCc && m.d1 < wire_.faderCc + 9) {
      moveFader(m.d1 - wire_.faderCc, m.d2);
      return true;
    }
  }
  if (ch == 14) return true;  // touch events
  if (ch == 0 || ch == 15) {
    for (size_t i = 0; i < static_cast<size_t>(Button::Count); ++i)
      if (wire_.buttons[i] == m.d1) {
        button(static_cast<Button>(i), m.d2 >= 64);
        return true;
      }
    if (model_.faders && m.d1 >= wire_.faderButtonCc && m.d1 < wire_.faderButtonCc + 8) {
      if (m.d2 >= 64) faderButton(m.d1 - wire_.faderButtonCc);
      return true;
    }
  }
  const std::string key = std::to_string(m.status) + " " + std::to_string(m.d1);
  if (unknownLogged_[key]++ == 0)
    logging::info("flkey", std::string(model_.name) + ": unhandled message " + std::to_string(m.status) + " " + std::to_string(m.d1) + " " +
                               std::to_string(m.d2));
  return true;  // the DAW port carries no notes to play
}

}  // namespace gl::flkey
