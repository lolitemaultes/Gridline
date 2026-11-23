// Gridline - the Novation Launchpad Pro [MK3] as a control surface (LaunchpadProMk3.h).
// Message formats from Novation's Launchpad Pro [MK3] Programmer's Reference Guide; the page design is
// Gridline's own (see docs/controllers/launchpad-pro-mk3.md).
#include "LaunchpadProMk3.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace gl::surfaces::lpp3 {

// ---------------------------------------------------------------- protocol ----

Bytes sysex(std::initializer_list<uint8_t> body) {
  Bytes b(std::begin(kHeader), std::end(kHeader));
  b.insert(b.end(), body.begin(), body.end());
  b.push_back(0xF7);
  return b;
}

Bytes dawMode(bool on) { return sysex({CmdDawMode, static_cast<uint8_t>(on ? 1 : 0)}); }

Bytes selectLayout(int layout, int page) {
  return sysex({CmdLayout, static_cast<uint8_t>(layout & 0x7F), static_cast<uint8_t>(page & 0x7F), 0x00});
}

Bytes deviceInquiry() { return {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}; }

Bytes faderSetup(int bank, bool horizontal, const std::vector<FaderSpec>& faders) {
  Bytes b(std::begin(kHeader), std::end(kHeader));
  b.push_back(CmdFaderSetup);
  b.push_back(static_cast<uint8_t>(bank & 3));
  b.push_back(horizontal ? 1 : 0);
  for (const FaderSpec& f : faders) {
    b.push_back(static_cast<uint8_t>(f.index & 7));
    b.push_back(f.bipolar ? 1 : 0);
    b.push_back(static_cast<uint8_t>(f.cc & 0x7F));
    b.push_back(static_cast<uint8_t>(f.colour & 0x7F));
  }
  b.push_back(0xF7);
  return b;
}

const std::array<Rgb, 128>& palette() {
  // The colour chart of the Programmer's Reference Guide (Colour palette), each swatch sampled and its
  // channels stretched from the chart's pastel 0x61..0xFF back to 0..255; entry 0 is off.
  static const std::array<Rgb, 128> p = {
      0x000000, 0x848484, 0xC8C8C8, 0xFFFFFF, 0xFF8484, 0xFF0000, 0xC80000, 0x840000,
      0xFFECBB, 0xFF8400, 0xC84500, 0x842200, 0xFFE467, 0xFFFF00, 0xC8C800, 0x848400,
      0xC8FF67, 0x9DFF00, 0x67C800, 0x348400, 0x9DFF84, 0x00FF00, 0x00C800, 0x008400,
      0x9DFF9D, 0x00FF45, 0x00C822, 0x008410, 0x9DFFAD, 0x00FFAD, 0x00C867, 0x008434,
      0x9DFFEC, 0x00FFDB, 0x00C89D, 0x008456, 0x9DECFF, 0x00E4FF, 0x00A5C8, 0x006784,
      0x9DC8FF, 0x00A5FF, 0x0067C8, 0x003484, 0x6745FF, 0x0000FF, 0x0000C8, 0x000084,
      0xAD84FF, 0x6700FF, 0x3400C8, 0x220084, 0xFF84FF, 0xFF00FF, 0xC800C8, 0x840084,
      0xFF84BB, 0xFF009D, 0xC80067, 0x840045, 0xFF2200, 0xDB8400, 0xC89D00, 0x676700,
      0x008400, 0x008445, 0x0045BB, 0x0000FF, 0x008484, 0x4500EC, 0xAD849D, 0x452234,
      0xFF0000, 0xECFF67, 0xE4FA00, 0xADFF00, 0x22C800, 0x00FFAD, 0x00DBFF, 0x0067FF,
      0x4500FF, 0xAD00FA, 0xE445C8, 0x672200, 0xFF6700, 0xC8F500, 0xBBFF45, 0x00FF00,
      0x84FF67, 0xADFABB, 0x84FFF0, 0xADD3FF, 0x679DF0, 0xBB9DF5, 0xF545FF, 0xFF00AD,
      0xFF9D00, 0xECE400, 0xD3FF00, 0xC8AD00, 0x846700, 0x009022, 0x229D45, 0x343467,
      0x3445AD, 0xAD7634, 0xC80000, 0xF58467, 0xF59022, 0xFFEC45, 0xDBF567, 0xBBE422,
      0x343467, 0xF5F5BB, 0xC8FAD3, 0xDBDBFF, 0xD3BBFF, 0x848484, 0xBBBBBB, 0xF5FFFF,
      0xDB0000, 0x760000, 0x34F000, 0x008400, 0xECE400, 0x846700, 0xE49D00, 0x9D2200,
  };
  return p;
}

namespace {
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

bool isLaunchpadPort(const std::string& name) {
  const std::string n = lower(name);
  return n.find("lppromk3") != std::string::npos || n.find("launchpad pro mk3") != std::string::npos ||
         n.find("launchpad pro [mk3]") != std::string::npos;
}

bool isDawPort(const std::string& name) {
  if (!isLaunchpadPort(name)) return false;
  const std::string n = lower(name);
  if (n.find("daw") != std::string::npos) return true;
  // Windows: the third interface of the device ("MIDIIN3 (LPProMK3 MIDI)").
  return n.find("midiin3 (") != std::string::npos;
}

std::string outputFor(const std::string& in, const std::vector<std::string>& outputs) {
  for (const auto& o : outputs)
    if (o == in) return o;
  std::string want = in;
  if (const size_t k = lower(want).find("midiin"); k != std::string::npos) want.replace(k, 6, "MIDIOUT");
  for (const auto& o : outputs)
    if (o == want || lower(o) == lower(want)) return o;
  // Same device, DAW interface, whatever the system calls the output.
  for (const auto& o : outputs)
    if (isDawPort(o)) return o;
  return in;
}

// ---------------------------------------------------------------- options ----

std::string Options::save() const {
  std::ostringstream s;
  s << "modes=" << (modePages ? 1 : 0) << ";keys=" << (keysPage ? 1 : 0) << ";steps=" << (stepsPage ? 1 : 0)
    << ";faders=" << (faders ? 1 : 0) << ";vel=" << velocity << ";bright=" << brightness << ";rgb=" << (exactColours ? 1 : 0);
  return s.str();
}

Options Options::load(const std::string& s) {
  Options o;
  size_t at = 0;
  while (at < s.size()) {
    size_t end = s.find(';', at);
    if (end == std::string::npos) end = s.size();
    const std::string kv = s.substr(at, end - at);
    at = end + 1;
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = kv.substr(0, eq);
    const int v = std::atoi(kv.c_str() + eq + 1);
    if (k == "modes") o.modePages = v != 0;
    else if (k == "keys") o.keysPage = v != 0;
    else if (k == "steps") o.stepsPage = v != 0;
    else if (k == "faders") o.faders = v != 0;
    else if (k == "vel") o.velocity = std::clamp(v, 0, 3);
    else if (k == "bright") o.brightness = std::clamp(v, 0, 2);
    else if (k == "rgb") o.exactColours = v != 0;
  }
  return o;
}

// ---------------------------------------------------------------- controller ----

namespace {
std::array<bool, 128> usedLeds() {
  std::array<bool, 128> u{};
  for (int n = 0; n < 128; ++n) u[static_cast<size_t>(n)] = isPad(n);
  for (int i = 1; i <= 8; ++i) u[static_cast<size_t>(i)] = true;              // track controls
  for (int r = 1; r <= 8; ++r) u[static_cast<size_t>(r * 10)] = true;         // left column
  for (int r = 1; r <= 8; ++r) u[static_cast<size_t>(r * 10 + 9)] = true;     // scene column
  for (int i = 101; i <= 108; ++i) u[static_cast<size_t>(i)] = true;          // track select
  u[CShift] = u[CLeft] = u[CRight] = u[CLogo] = true;  // (Session .. Projects light themselves)
  return u;
}
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

Controller::Controller(Model& m, Options o, Out out) : m_(m), opt_(o), out_(std::move(out)) {
  leds_.use(usedLeds());
  pads_.fill(-1);
  for (auto& b : posSent_) b.fill(-1);
  for (auto& b : touched_) b.fill(-10.0);
}

void Controller::setOptions(const Options& o) {
  if (o == opt_) return;
  opt_ = o;
  leds_.invalidate();
  for (auto& f : faderSent_) f.clear();
  if (!opt_.faders && page_ == Page::Faders) setPage(before_);
  if (!opt_.keysPage && (page_ == Page::Keys || page_ == Page::Drums)) setPage(Page::Session);
  if (!opt_.stepsPage && page_ == Page::Steps) setPage(Page::Session);
}

void Controller::connect() {
  connected_ = true;
  out_(dawMode(true));
  if (page_ == Page::Faders) {
    requestLayout(LFader, static_cast<int>(bank_), 0);
  } else {
    if (page_ == Page::Device) page_ = Page::Session;
    requestLayout(LSession, 0, 0);
  }
  out_(deviceInquiry());
  leds_.invalidate();
  for (auto& f : faderSent_) f.clear();
  for (auto& b : posSent_) b.fill(-1);
  expectUntil_ = 1e300;  // the first report after the handshake is the answer to it
}

void Controller::disconnect() {
  if (!connected_) return;
  out_(dawMode(false));
  connected_ = false;
}

void Controller::requestLayout(int layout, int page, double now) {
  out_(selectLayout(layout, page));
  expectLayout_ = layout;
  expectUntil_ = now + 0.6;
}

void Controller::setPage(Page p, double now) {
  if (p == Page::Faders) {
    openFaders(bank_, now);
    return;
  }
  if (p == page_) return;
  if (p == Page::Device) {
    page_ = p;
    return;
  }
  if (page_ == Page::Faders || page_ == Page::Device || layout_ != LSession) requestLayout(LSession, 0, now);
  page_ = p;
  held_.fill(false);
  drumHit_ = -1;
}

void Controller::openFaders(Bank b, double now) {
  if (!opt_.faders) return;
  bank_ = b;
  if (page_ != Page::Faders) {
    before_ = page_ == Page::Device ? Page::Session : page_;
    page_ = Page::Faders;
  }
  requestLayout(LFader, static_cast<int>(b), now);
}

Mod Controller::mod() const {
  if (shift_ && dup_) return Mod::Double;
  if (clear_) return Mod::Clear;
  if (dup_) return Mod::Duplicate;
  if (quant_) return Mod::Quantise;
  if (shift_) return Mod::Shift;
  return Mod::None;
}

int Controller::faderOffset() const { return m_.fadersFollowSession() ? col0_ : fad0_; }

bool Controller::onMidi(const MidiMessage& m, double now) {
  const int kind = m.kind(), ch = m.channel();
  if (ch == 0 && (kind == 0x90 || kind == 0x80)) {
    if (isPad(m.d1)) padPress(m.d1, kind == 0x90 && m.d2 > 0);
  } else if (ch == 0 && kind == 0xB0) {
    press(m.d1, m.d2 > 0, now);
  } else if (ch == 4 && kind == 0xB0) {  // DAW fader moved (channel 5)
    const int k = m.d1 - kFaderCcBase;
    if (k >= 0 && k < 32) {
      const auto b = static_cast<Bank>(k / 8);
      const int i = k % 8;
      touched_[static_cast<size_t>(k / 8)][static_cast<size_t>(i)] = now;
      posSent_[static_cast<size_t>(k / 8)][static_cast<size_t>(i)] = m.d2;  // the unit shows it already
      m_.setFader(b, faderOffset() + i, m.d2 / 127.0);
    }
  }
  return true;  // the DAW port only ever talks to the surface (pad notes reach the instrument from the driver)
}

void Controller::onSysex(const Bytes& b, double now) {
  // Device inquiry answer: F0 7E <id> 06 02 00 20 29 13 01 00 00 <4 digits> F7.
  if (b.size() >= 17 && b[0] == 0xF0 && b[1] == 0x7E && b[3] == 0x06 && b[4] == 0x02 && b[5] == 0x00 && b[6] == 0x20 &&
      b[7] == 0x29 && b[8] == 0x13) {
    firmware_.clear();
    for (size_t i = 12; i < 16; ++i) firmware_ += (i > 12 ? "." : "") + std::to_string(b[i]);
    return;
  }
  if (b.size() < 9 || !std::equal(std::begin(kHeader), std::end(kHeader), b.begin()) || b[6] != CmdLayout) return;
  const int layout = b[7], page = b[8];
  layout_ = layout;
  if (layout == expectLayout_ && now <= expectUntil_) {  // the unit confirming what Gridline asked for
    expectLayout_ = -1;
    return;
  }
  expectLayout_ = -1;
  if (layout == LSession) {  // Session pressed: Gridline's Session page
    if (page_ != Page::Session) {
      page_ = Page::Session;
      held_.fill(false);
    }
  } else if (layout == LFader) {
    if (page_ != Page::Faders) before_ = page_ == Page::Device ? Page::Session : page_;
    page_ = Page::Faders;
    bank_ = static_cast<Bank>(std::clamp(page, 0, 3));
  } else if (layout == LNote && opt_.modePages && opt_.keysPage) {
    page_ = m_.keys().drums ? Page::Drums : Page::Keys;
    held_.fill(false);
    requestLayout(LSession, 0, now);
  } else if (layout >= LSeqFirst && layout <= LSeqLast && layout != 13 && opt_.modePages && opt_.stepsPage) {
    page_ = Page::Steps;
    requestLayout(LSession, 0, now);
  } else {
    page_ = Page::Device;  // Chord, Custom, Projects, its own Note or Sequencer, Programmer, Settings
  }
}

void Controller::press(int cc, bool down, double now) {
  switch (cc) {
    case CShift: shift_ = down; return;
    case CClear: clear_ = down; return;
    case CDuplicate: dup_ = down; return;
    case CQuantise: quant_ = down; return;
    default: break;
  }
  if (cc >= CVolume && cc <= CDevice && !down) {  // momentary: held, the page goes back when let go
    if (bankOpened_ && page_ == Page::Faders && now - bankPressAt_ > 0.5) setPage(before_, now);
    bankOpened_ = false;
    return;
  }
  if (!down) return;
  switch (cc) {
    case CPlay: m_.button(shift_ ? Button::Stop : Button::PlayPause); return;
    case CRecord: m_.button(shift_ ? Button::Capture : Button::Record); return;
    case CFixedLength: m_.button(Button::LoopRecord); return;
    case CLeft: arrow(-1, 0); return;
    case CRight: arrow(1, 0); return;
    case CUp: arrow(0, -1); return;
    case CDown: arrow(0, 1); return;
    case CSession: setPage(Page::Session, now); return;  // (when the unit passes its mode buttons on)
    case CNote: if (opt_.keysPage) setPage(m_.keys().drums ? Page::Drums : Page::Keys, now); return;
    case CSequencer: if (opt_.stepsPage) setPage(Page::Steps, now); return;
    default: break;
  }
  if (cc == CRecordArm || cc == CMute || cc == CSolo || cc == CStopClip) {
    if (shift_) {
      m_.button(cc == CRecordArm ? Button::Undo : cc == CMute ? Button::Redo : cc == CSolo ? Button::Click : Button::StopAll);
      return;
    }
    const TrackMode t = cc == CRecordArm ? TrackMode::Arm : cc == CMute ? TrackMode::Mute : cc == CSolo ? TrackMode::Solo : TrackMode::Stop;
    trackMode_ = trackMode_ == t ? TrackMode::Select : t;
    if (page_ != Page::Session && page_ != Page::Faders) setPage(Page::Session, now);
    return;
  }
  if (cc >= CVolume && cc <= CDevice) {
    if (shift_) {
      if (cc == CSends) m_.button(Button::TapTempo);
      return;
    }
    const auto b = static_cast<Bank>(cc - CVolume);
    if (page_ == Page::Faders && bank_ == b) {
      setPage(before_, now);
      bankOpened_ = false;
    } else {
      openFaders(b, now);
      bankPressAt_ = now;
      bankOpened_ = page_ == Page::Faders;
    }
    return;
  }
  if (cc >= CTrackSelect && cc < CTrackSelect + 8) {
    const int i = cc - CTrackSelect;
    if (shift_) {  // page picker
      if (i == 0) setPage(Page::Session, now);
      else if ((i == 1 || i == 2) && opt_.keysPage) setPage(i == 1 ? Page::Keys : Page::Drums, now);
      else if (i == 3 && opt_.stepsPage) setPage(Page::Steps, now);
      else if (i >= 4) openFaders(static_cast<Bank>(i - 4), now);
      return;
    }
    if (page_ == Page::Session) m_.bottomPress(col0_ + i, trackMode_);
    else if (page_ == Page::Keys || page_ == Page::Drums) {
      if (chan0_ + i < m_.channelCount()) m_.selectChannel(chan0_ + i);
    }
    return;
  }
  if (cc >= 19 && cc <= 89 && cc % 10 == 9) {  // scene column
    const int r = (89 - cc) / 10;
    if (page_ == Page::Session) {
      m_.sidePress(row0_ + r, trackMode_, shift_);
    } else if (page_ == Page::Keys || page_ == Page::Drums) {
      if (r == 0) chan0_ = std::max(0, chan0_ - 8);
      else if (r == 7 && chan0_ + 8 < m_.channelCount()) chan0_ += 8;
    } else if (page_ == Page::Steps) {
      if (pat0_ + r < m_.patternCount()) m_.selectPattern(pat0_ + r);
    }
  }
}

void Controller::arrow(int dx, int dy) {
  const int s = shift_ ? 8 : 1;
  switch (page_) {
    case Page::Session:
    case Page::Device:
      col0_ = std::clamp(col0_ + dx * s, 0, std::max(0, m_.sessionCols() - 8));
      row0_ = std::clamp(row0_ + dy * s, 0, std::max(0, m_.sessionRows() - 8));
      break;
    case Page::Keys:
      if (dy) octave_ = std::clamp(octave_ - dy, 0, 9);
      if (dx) {
        if (shift_) m_.shiftRoot(dx);
        else shiftKey_ = std::clamp(shiftKey_ + dx, -24, 24);
      }
      break;
    case Page::Drums:
      drumBase_ = std::clamp(drumBase_ - dy * 16 + dx * 4, 0, 64);
      break;
    case Page::Steps: {
      const int count = m_.steps().count;
      if (dx) step0_ = std::clamp(step0_ + dx * 32, 0, std::max(0, (count - 1) / 32 * 32));
      if (dy) chan0_ = std::clamp(chan0_ + dy * 8, 0, std::max(0, (m_.channelCount() - 1) / 8 * 8));
      break;
    }
    case Page::Faders: {
      const int n = m_.faderCount(bank_);
      if (m_.fadersFollowSession()) col0_ = std::clamp(col0_ + dx * s, 0, std::max(0, n - 8));
      else fad0_ = std::clamp(fad0_ + dx * s, 0, std::max(0, n - 8));
      break;
    }
  }
}

void Controller::padPress(int n, bool down) {
  held_[static_cast<size_t>(n)] = down;
  const int c = padCol(n), r = padRow(n);
  switch (page_) {
    case Page::Session:
    case Page::Device: {
      const int col = col0_ + c, row = row0_ + r;
      if (col >= m_.sessionCols() || row >= m_.sessionRows()) return;
      if (down) m_.sessionPress(col, row, mod());
      else m_.sessionRelease(col, row);
      break;
    }
    case Page::Drums:
      if (down && pads_[static_cast<size_t>(n)] >= 0) drumHit_ = pads_[static_cast<size_t>(n)];
      break;
    case Page::Steps:
      if (!down) return;
      if (r < 4) {
        const int i = step0_ + r * 8 + c;
        if (i < m_.steps().count || shift_) {
          if (shift_) m_.setStepLength(i + 1);
          else m_.toggleStep(i);
        }
      } else if (chan0_ + (r - 4) * 8 + c < m_.channelCount()) {
        m_.selectChannel(chan0_ + (r - 4) * 8 + c);
      }
      break;
    default:
      break;
  }
}

void Controller::computePads() {
  std::array<int16_t, 128> p{};
  p.fill(-1);
  if (page_ == Page::Keys) {
    const Keys k = m_.keys();
    std::vector<int> degs;
    for (int s = 0; s < 12; ++s)
      if (k.scale & (1u << s)) degs.push_back(s);
    const bool chromatic = (k.scale & 0x0FFF) == 0x0FFF || degs.empty();
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c) {
        const int rb = 7 - r;  // row from the bottom
        int pitch;
        if (chromatic) {
          pitch = 12 * octave_ + shiftKey_ + rb * 5 + c;
        } else {
          const int n = static_cast<int>(degs.size()), d = shiftKey_ + rb * 3 + c;
          const int oct = floorDiv(d, n), idx = d - oct * n;
          pitch = 12 * octave_ + k.root + 12 * oct + degs[static_cast<size_t>(idx)];
        }
        if (pitch >= 0 && pitch < 128) p[static_cast<size_t>(padOf(c, r))] = static_cast<int16_t>(pitch);
      }
  } else if (page_ == Page::Drums) {
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c) {
        const int rb = 7 - r, q = (c >= 4 ? 2 : 0) + (rb >= 4 ? 1 : 0);
        const int pitch = drumBase_ + q * 16 + (rb % 4) * 4 + (c % 4);
        if (pitch >= 0 && pitch < 128) p[static_cast<size_t>(padOf(c, r))] = static_cast<int16_t>(pitch);
      }
  }
  if (p != pads_) {
    pads_ = p;
    ++padSerial_;
  }
}

int Controller::pal(Rgb c, float f) const {
  if (c == 0) return 0;
  static constexpr float kBright[3] = {1.0f, 0.6f, 0.35f};
  return nearestColour(palette(), dim(c, f * kBright[std::clamp(opt_.brightness, 0, 2)]));
}

Led Controller::colour(Rgb c, float f) const {
  if (c == 0) return Led::off();
  if (opt_.exactColours) {
    static constexpr float kBright[3] = {1.0f, 0.6f, 0.35f};
    const Rgb d = dim(c, f * kBright[std::clamp(opt_.brightness, 0, 2)]);
    return Led::exact(d == 0 ? 0x010101 : d);
  }
  return Led::solid(pal(c, f));
}

Led Controller::cellLed(const Cell& c) const {
  switch (c.state) {
    case State::Empty: return Led::off();
    case State::Filled: return colour(c.colour);
    case State::Playing: return Led::pulse(std::max(1, pal(c.colour)));
    case State::Queued: return Led::flash(std::max(1, pal(c.colour)), KGreen);
    case State::Recording: return Led::pulse(KRed);
    case State::RecordQueued: return Led::flash(KDimRed, KRed);
    case State::ArmedEmpty: return Led::solid(KDimRed);
    case State::Dim: return colour(c.colour, 0.3f);
    case State::Selected: return Led::solid(KWhite);
    case State::Marker: return Led::solid(KDimWhite);
  }
  return Led::off();
}

void Controller::computeLeds() {
  const Transport t = m_.transport();
  const Page grid = page_ == Page::Faders ? before_ : page_ == Page::Device ? Page::Session : page_;
  // ---- the grid ----
  for (int r = 0; r < 8; ++r)
    for (int c = 0; c < 8; ++c) {
      const int n = padOf(c, r);
      Led l;
      if (grid == Page::Session) {
        const int col = col0_ + c, row = row0_ + r;
        if (col < m_.sessionCols() && row < m_.sessionRows()) l = cellLed(m_.sessionCell(col, row));
      } else if (grid == Page::Keys) {
        const Keys k = m_.keys();
        const int pitch = pads_[static_cast<size_t>(n)];
        if (held_[static_cast<size_t>(n)] && pitch >= 0) {
          l = Led::solid(KWhite);
        } else if (pitch >= 0) {
          const bool chromatic = (k.scale & 0x0FFF) == 0x0FFF;
          const int rel = ((pitch - (chromatic ? 0 : k.root)) % 12 + 12) % 12;
          if (rel == 0) l = colour(k.colour);
          else if (chromatic || (k.scale & (1u << rel))) l = colour(k.colour, 0.25f);
        }
      } else if (grid == Page::Drums) {
        const Keys k = m_.keys();
        const int pitch = pads_[static_cast<size_t>(n)];
        if (held_[static_cast<size_t>(n)] && pitch >= 0) {
          l = Led::solid(KWhite);
        } else if (pitch >= 0) {
          const int hitBlock = drumHit_ >= 0 ? (drumHit_ - drumBase_) / 16 : 0;
          const int block = (pitch - drumBase_) / 16;
          l = colour(k.colour, block == hitBlock ? 1.0f : 0.3f);
        }
      } else if (grid == Page::Steps) {
        if (r < 4) {
          const Steps s = m_.steps();
          const int i = step0_ + r * 8 + c;
          if (i < s.count) {
            const bool on = i < static_cast<int>(s.velocity.size()) && s.velocity[static_cast<size_t>(i)] > 0;
            if (i == s.playhead) l = Led::solid(KWhite);
            else l = colour(s.colour, on ? 1.0f : 0.15f);
          }
        } else {
          const int ch = chan0_ + (r - 4) * 8 + c;
          if (ch < m_.channelCount()) l = ch == m_.selectedChannel() ? Led::solid(KWhite) : colour(m_.channelColour(ch), 0.6f);
        }
      }
      leds_.want(n, l);
    }
  // ---- the buttons ----
  const bool session = grid == Page::Session;
  auto btn = [&](int cc, int c) { leds_.want(cc, Led::solid(c)); };
  btn(CShift, shift_ ? KWhite : KDimWhite);
  leds_.want(CLogo, t.recording ? Led::pulse(KRed) : t.playing ? Led::pulse(KGreen) : Led::solid(KDimWhite));
  btn(CPlay, shift_ ? KGold : t.playing ? KGreen : KDimWhite);
  btn(CRecord, shift_ ? KGold : t.recording ? KRed : KDimRed);
  btn(CFixedLength, shift_ ? KOff : t.loopRecord ? KBlue : KDimWhite);
  btn(CClear, shift_ || !(session || grid == Page::Steps) ? KOff : clear_ ? KWhite : KDimWhite);
  btn(CDuplicate, !session ? KOff : shift_ ? KGold : dup_ ? KWhite : KDimWhite);
  btn(CQuantise, !session || shift_ ? KOff : quant_ ? KWhite : KDimWhite);
  // Arrows: lit where they go somewhere.
  bool canL = false, canR = false, canU = false, canD = false;
  if (page_ == Page::Faders) {
    const int off = faderOffset(), n = m_.faderCount(bank_);
    canL = off > 0, canR = off + 8 < n;
  } else if (session) {
    canL = col0_ > 0, canR = col0_ + 8 < m_.sessionCols(), canU = row0_ > 0, canD = row0_ + 8 < m_.sessionRows();
  } else if (grid == Page::Keys) {
    canL = canR = true, canU = octave_ < 9, canD = octave_ > 0;
  } else if (grid == Page::Drums) {
    canL = drumBase_ > 0, canR = drumBase_ < 64, canU = drumBase_ < 64, canD = drumBase_ > 0;
  } else if (grid == Page::Steps) {
    const int count = m_.steps().count;
    canL = step0_ > 0, canR = step0_ + 32 < count, canU = chan0_ > 0, canD = chan0_ + 32 < m_.channelCount();
  }
  auto arrowLed = [&](int cc, bool can) { btn(cc, can ? (shift_ ? KGold : KDimWhite) : KOff); };
  arrowLed(CLeft, canL), arrowLed(CRight, canR), arrowLed(CUp, canU), arrowLed(CDown, canD);
  // Track controls.
  if (shift_) {
    btn(CRecordArm, KGold), btn(CMute, KGold), btn(CSolo, t.click ? KGreen : KRed), btn(CVolume, KOff), btn(CPan, KOff);
    btn(CSends, KGold), btn(CDevice, KOff), btn(CStopClip, KGold);
  } else {
    auto modeLed = [&](int cc, TrackMode m, int on) { btn(cc, page_ != Page::Session ? KOff : trackMode_ == m ? on : KDimWhite); };
    modeLed(CRecordArm, TrackMode::Arm, KRed), modeLed(CMute, TrackMode::Mute, KAmber), modeLed(CSolo, TrackMode::Solo, KBlue);
    modeLed(CStopClip, TrackMode::Stop, KRed);
    for (int b = 0; b < 4; ++b)
      btn(CVolume + b, !opt_.faders ? KOff : page_ == Page::Faders && static_cast<int>(bank_) == b ? KGreen : KDimWhite);
  }
  // Track Select row.
  for (int i = 0; i < 8; ++i) {
    const int cc = CTrackSelect + i;
    if (shift_) {
      const bool avail = i == 0 || ((i == 1 || i == 2) && opt_.keysPage) || (i == 3 && opt_.stepsPage) || (i >= 4 && opt_.faders);
      const bool cur = (i == 0 && page_ == Page::Session) || (i == 1 && page_ == Page::Keys) || (i == 2 && page_ == Page::Drums) ||
                       (i == 3 && page_ == Page::Steps) || (i >= 4 && page_ == Page::Faders && static_cast<int>(bank_) == i - 4);
      btn(cc, !avail ? KOff : cur ? KGreen : KGold);
    } else if (session && page_ != Page::Faders) {
      leds_.want(cc, cellLed(m_.bottomCell(col0_ + i, trackMode_)));
    } else if (grid == Page::Keys || grid == Page::Drums) {
      const int ch = chan0_ + i;
      leds_.want(cc, ch >= m_.channelCount() ? Led::off() : ch == m_.selectedChannel() ? Led::solid(KWhite) : colour(m_.channelColour(ch)));
    } else {
      leds_.want(cc, Led::off());
    }
  }
  // Scene column.
  for (int r = 0; r < 8; ++r) {
    const int cc = CScene - r * 10;
    Led l;
    if (session) {
      l = shift_ ? Led::solid(KGold) : row0_ + r < m_.sessionRows() ? cellLed(m_.sideCell(row0_ + r, trackMode_)) : Led::off();
    } else if (grid == Page::Keys || grid == Page::Drums) {
      if (r == 0 && chan0_ > 0) l = Led::solid(KDimWhite);
      if (r == 7 && chan0_ + 8 < m_.channelCount()) l = Led::solid(KDimWhite);
    } else if (grid == Page::Steps && pat0_ + r < m_.patternCount()) {
      l = pat0_ + r == m_.selectedPattern() ? Led::solid(KWhite) : colour(m_.patternColour(pat0_ + r));
    }
    leds_.want(cc, l);
  }
}

void Controller::sendLeds() {
  const std::vector<int> todo = leds_.take(kMaxPerIdle);
  Bytes rgb;
  for (int i : todo) {
    const Led& l = leds_.wanted(i);
    const bool pad = isPad(i);
    auto msg = [&](int ch, int v) {
      out_({static_cast<uint8_t>((pad ? 0x90 : 0xB0) | ch), static_cast<uint8_t>(i), static_cast<uint8_t>(v & 0x7F)});
    };
    switch (l.mode) {
      case LedMode::Off: msg(0, 0); break;
      case LedMode::Static: msg(0, l.a); break;
      case LedMode::Flash: msg(0, l.a), msg(1, l.b); break;
      case LedMode::Pulse: msg(2, l.a); break;
      case LedMode::Rgb:
        if (rgb.empty()) rgb.assign(std::begin(kHeader), std::end(kHeader)), rgb.push_back(CmdLed);
        rgb.push_back(3);
        rgb.push_back(static_cast<uint8_t>(i));
        rgb.push_back(static_cast<uint8_t>(((l.rgb >> 16) & 0xFF) >> 1));
        rgb.push_back(static_cast<uint8_t>(((l.rgb >> 8) & 0xFF) >> 1));
        rgb.push_back(static_cast<uint8_t>((l.rgb & 0xFF) >> 1));
        break;
    }
  }
  if (!rgb.empty()) {
    rgb.push_back(0xF7);
    out_(rgb);
  }
}

void Controller::computeFaders(double now) {
  if (!opt_.faders) return;
  const int off = faderOffset();
  for (int b = 0; b < 4; ++b) {
    const auto bank = static_cast<Bank>(b);
    std::vector<FaderSpec> specs;
    std::array<Fader, 8> f{};
    for (int i = 0; i < 8; ++i) {
      f[static_cast<size_t>(i)] = off + i < m_.faderCount(bank) ? m_.fader(bank, off + i) : Fader{};
      const Fader& x = f[static_cast<size_t>(i)];
      specs.push_back({i, x.bipolar, kFaderCcBase + b * 8 + i, x.shown ? std::max(1, pal(x.colour)) : 0});
    }
    auto& sent = faderSent_[static_cast<size_t>(b)];
    if (specs.size() != sent.size() ||
        !std::equal(specs.begin(), specs.end(), sent.begin(), [](const FaderSpec& p, const FaderSpec& q) {
          return p.index == q.index && p.bipolar == q.bipolar && p.cc == q.cc && p.colour == q.colour;
        })) {
      out_(faderSetup(b, bank == Bank::Pan, specs));
      sent = specs;
      posSent_[static_cast<size_t>(b)].fill(-1);
    }
    for (int i = 0; i < 8; ++i) {
      const Fader& x = f[static_cast<size_t>(i)];
      if (!x.shown) continue;
      const int v = std::clamp(static_cast<int>(std::lround(x.value * 127.0)), 0, 127);
      int& was = posSent_[static_cast<size_t>(b)][static_cast<size_t>(i)];
      if (v == was || now - touched_[static_cast<size_t>(b)][static_cast<size_t>(i)] < 0.3) continue;
      out_({0xB4, static_cast<uint8_t>(kFaderCcBase + b * 8 + i), static_cast<uint8_t>(v)});
      was = v;
    }
  }
}

void Controller::idle(double now) {
  if (!connected_) return;
  computePads();
  computeLeds();
  sendLeds();
  computeFaders(now);
}

}  // namespace gl::surfaces::lpp3
