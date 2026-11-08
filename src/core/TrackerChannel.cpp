// Gridline - tracker modules (K-76): channel logic. See TrackerChannel.h.
#include "TrackerChannel.h"
#include <cmath>

namespace gl::tracker {

namespace {

constexpr double kRefRate = 44000.0;  // FT2's reference mixing rate: its pitch arithmetic is done there

// 2^(i/768) in 8.24 fixed point (the fraction of an octave of a linear period).
const uint32_t* logTable() {
  static const auto t = [] {
    std::array<uint32_t, 768> v{};
    for (int i = 0; i < 768; ++i) v[static_cast<size_t>(i)] = static_cast<uint32_t>(std::lround(16777216.0 * std::exp2(i / 768.0)));
    return v;
  }();
  return t.data();
}

// Vibrato / tremolo half sine (32 steps) and auto vibrato full sine (256 steps).
uint8_t vibSine(int i) {
  static const auto t = [] {
    std::array<uint8_t, 32> v{};
    for (int k = 0; k < 32; ++k) v[static_cast<size_t>(k)] = static_cast<uint8_t>(std::floor(255.0 * std::sin(k * 3.14159265358979323846 / 32.0)));
    return v;
  }();
  return t[static_cast<size_t>(i & 31)];
}
int8_t autoVibSine(int i) {
  static const auto t = [] {
    std::array<int8_t, 256> v{};
    for (int k = 0; k < 256; ++k) v[static_cast<size_t>(k)] = static_cast<int8_t>(std::lround(64.0 * std::sin(-k * 2.0 * 3.14159265358979323846 / 256.0)));
    return v;
  }();
  return t[static_cast<size_t>(i & 255)];
}
// FT2's arpeggio step for a tick counter value: 0, 1, 2 repeating for 16 ticks; past that FT2 reads
// the bytes after its table (the start of its vibrato table): 0 once, then values it treats as 2.
int arpStep(int tickDown) {
  const int i = tickDown & 31;
  if (i < 16) return i % 3;
  return i == 16 ? 0 : 2;
}

}  // namespace

double periodHz(uint16_t period, bool linear) {
  if (period == 0) return 0.0;
  uint32_t d;
  if (linear) {
    static const uint32_t mul = static_cast<uint32_t>(std::lround(256.0 * 65536.0 / kRefRate * 8363.0));
    const uint32_t inv = (12u * 192u * 4u - period) & 0xFFFFu;
    const uint32_t q = inv / 768u, r = inv % 768u;
    const uint32_t shift = (14u - q) & 31u;
    d = static_cast<uint32_t>((static_cast<uint64_t>(logTable()[r]) * mul) >> 24);
    d >>= shift;
  } else {
    static const uint32_t div = static_cast<uint32_t>(std::lround(65536.0 * 1712.0 / kRefRate * 8363.0));
    d = div / period;
  }
  return static_cast<double>(d) * kRefRate / 65536.0;
}

const InstrumentDef& emptyInstrument() {
  static const InstrumentDef e = [] {
    InstrumentDef d;
    d.samples[0].volume = 0;
    return d;
  }();
  return e;
}

void Channel::reset() {
  s = State{};
  ins = nullptr;
  s.status = kUpdVol;
}

const InstrumentDef* Channel::resolve(int n) const {
  if (fixed) return fixed;
  if (bank && n > 0 && n < bankSize && bank[n]) return bank[n];
  return nullptr;
}

void Channel::resetVolumes() {
  s.realVol = s.oldVol;
  s.outVol = s.oldVol;
  s.outPan = s.oldPan;
  s.status |= kUpdVol | kUpdPan | kQuickRamp;
}

void Channel::triggerInstrument() {
  if (!(s.vibTremCtrl & 0x04)) s.vibPos = 0;
  if (!(s.vibTremCtrl & 0x40)) s.tremPos = 0;
  s.retrigCounter = 0;
  s.tremorPos = 0;
  s.keyOff = 0;
  const InstrumentDef& in = instrument();
  if (in.vol.on()) {
    s.volEnvTick = 65535;
    s.volEnvPos = 0;
  }
  if (in.pan.on()) {
    s.panEnvTick = 65535;
    s.panEnvPos = 0;
  }
  s.fadeoutSpeed = in.fadeout;
  s.fadeoutVol = 32768;
  if (in.vibDepth > 0) {
    s.autoVibPos = 0;
    if (in.vibSweep > 0) {
      s.autoVibAmp = 0;
      s.autoVibSweep = static_cast<uint16_t>((in.vibDepth << 8) / in.vibSweep);
    } else {
      s.autoVibAmp = static_cast<uint16_t>(in.vibDepth << 8);
      s.autoVibSweep = 0;
    }
  }
}

void Channel::keyOff() {
  s.keyOff = 1;
  const InstrumentDef& in = instrument();
  if (in.vol.on()) {
    const auto x = static_cast<uint16_t>(in.vol.x[s.volEnvPos % kEnvPoints]);
    if (s.volEnvTick >= x) s.volEnvTick = static_cast<uint16_t>(x - 1);
  } else {
    s.realVol = 0;
    s.outVol = 0;
    s.status |= kUpdVol | kQuickRamp;
  }
  if (!in.pan.on()) {  // (FT2 tests the wrong flag here; kept: modules were made against it)
    const auto x = static_cast<uint16_t>(in.pan.x[s.panEnvPos % kEnvPoints]);
    if (s.panEnvTick >= x) s.panEnvTick = static_cast<uint16_t>(x - 1);
  }
}

void Channel::triggerNote(uint8_t note, uint8_t efx, uint8_t efxData) {
  if (note == kNoteOff) {
    keyOff();
    return;
  }
  if (note == 0) {
    note = s.noteNum;
    if (note == 0) return;
  }
  s.noteNum = note;
  ins = resolve(s.instrNum);
  const InstrumentDef& in = instrument();
  if (note > 96) note = 96;
  s.smpNum = static_cast<uint8_t>(in.keymap[static_cast<size_t>(note - 1)] & 15);
  const SampleDef& smp = in.samples[s.smpNum];
  s.relNote = smp.relNote;
  const int n = note + s.relNote;
  if (n < 0 || n >= 120) return;
  s.oldVol = smp.volume;
  s.oldPan = smp.pan;
  if (efx == 0xE && (efxData & 0xF0) == 0x50)
    s.finetune = static_cast<int8_t>(((efxData & 0x0F) * 16) - 128);
  else
    s.finetune = smp.finetune;
  if (n != 0) {
    const int idx = ((n - 1) * 16) + ((s.finetune >> 3) + 16);
    s.outPeriod = s.realPeriod = periodAt(idx, linear);
  }
  s.status |= kUpdPeriod | kUpdVol | kUpdPan | kTrigger | kQuickRamp;
  if (efx == 9) {
    if (efxData > 0) s.sampleOffset = s.efxData;
    s.smpStartPos = s.sampleOffset << 8;
  } else {
    s.smpStartPos = 0;
  }
}

// ---- tick 0 ----

void Channel::volSlide(uint8_t p) {
  if (p == 0) p = s.volSlideSpeed;
  s.volSlideSpeed = p;
  uint8_t v = s.realVol;
  if ((p & 0xF0) == 0) {
    v = static_cast<uint8_t>(v - p);
    if (static_cast<int8_t>(v) < 0) v = 0;
  } else {
    v = static_cast<uint8_t>(v + (p >> 4));
    if (v > 64) v = 64;
  }
  s.outVol = s.realVol = v;
  s.status |= kUpdVol;
}

void Channel::setEnvelopePos(uint8_t param) {
  const InstrumentDef& in = instrument();
  auto place = [&](const Envelope& e, uint16_t& eTick, int16_t& eDelta, int16_t& eValue, uint8_t& ePos) {
    eTick = static_cast<uint16_t>(param - 1);
    int point = 0;
    bool update = true;
    int t = param;
    if (e.count > 1) {
      ++point;
      for (int i = 0; i < e.count - 1; ++i) {
        if (point < kEnvPoints && t < e.x[static_cast<size_t>(point)]) {
          --point;
          t -= e.x[static_cast<size_t>(point)];
          if (t == 0) {
            update = false;
            break;
          }
          const int x0 = e.x[static_cast<size_t>(point)], x1 = e.x[static_cast<size_t>(point + 1)];
          const int xd = static_cast<int16_t>(x1 - x0);
          if (xd <= 0) {
            update = true;
            break;
          }
          const int y0 = e.y[static_cast<size_t>(point)], y1 = e.y[static_cast<size_t>(point + 1)];
          const int yd = static_cast<int8_t>(y1 - y0);
          eDelta = static_cast<int16_t>((yd * 256) / xd);
          eValue = static_cast<int16_t>((static_cast<int8_t>(y0) * 256) + static_cast<int16_t>(eDelta * (t - 1)));
          ++point;
          update = false;
          break;
        }
        ++point;
      }
      if (update) --point;
    }
    if (point < 0) point = 0;
    if (point >= kEnvPoints) point = kEnvPoints - 1;
    if (update) {
      eDelta = 0;
      eValue = static_cast<int16_t>(static_cast<int8_t>(e.y[static_cast<size_t>(point)]) * 256);
    }
    if (point >= e.count) {
      point = e.count - 1;
      if (point < 0) point = 0;
    }
    ePos = static_cast<uint8_t>(point);
  };
  if (in.vol.on()) place(in.vol, s.volEnvTick, s.volEnvDelta, s.volEnvValue, s.volEnvPos);
  if (in.vol.flags & kEnvSustain)  // (FT2 tests the volume envelope's flag for the pan envelope)
    place(in.pan, s.panEnvTick, s.panEnvDelta, s.panEnvValue, s.panEnvPos);
}

void Channel::moreTickZero(SongFx& fx) {
  const uint8_t p = s.efxData;
  switch (s.efx) {
    case 0xB: fx.jump = p; break;
    case 0xD: {
      const int r = ((p >> 4) * 10) + (p & 0x0F);
      fx.breakRow = r <= 63 ? r : 0;
      break;
    }
    case 0xE: {
      const uint8_t e = p >> 4, v = p & 0x0F;
      switch (e) {
        case 0x1: {
          const uint8_t q = v ? v : s.fPitchUp;
          s.fPitchUp = q;
          s.realPeriod = static_cast<uint16_t>(s.realPeriod - q * 4);
          if (static_cast<int16_t>(s.realPeriod) < 1) s.realPeriod = 1;
          s.outPeriod = s.realPeriod;
          s.status |= kUpdPeriod;
          break;
        }
        case 0x2: {
          const uint8_t q = v ? v : s.fPitchDown;
          s.fPitchDown = q;
          s.realPeriod = static_cast<uint16_t>(s.realPeriod + q * 4);
          if (static_cast<int16_t>(s.realPeriod) >= 32000) s.realPeriod = 32000 - 1;
          s.outPeriod = s.realPeriod;
          s.status |= kUpdPeriod;
          break;
        }
        case 0x3: s.semitonePorta = v != 0; break;
        case 0x4: s.vibTremCtrl = static_cast<uint8_t>((s.vibTremCtrl & 0xF0) | v); break;
        case 0x6: fx.loop = v; break;
        case 0x7: s.vibTremCtrl = static_cast<uint8_t>((v << 4) | (s.vibTremCtrl & 0x0F)); break;
        case 0xA: {
          const uint8_t q = v ? v : s.fVolUp;
          s.fVolUp = q;
          s.realVol = static_cast<uint8_t>(s.realVol + q);
          if (s.realVol > 64) s.realVol = 64;
          s.outVol = s.realVol;
          s.status |= kUpdVol;
          break;
        }
        case 0xB: {
          const uint8_t q = v ? v : s.fVolDown;
          s.fVolDown = q;
          s.realVol = static_cast<uint8_t>(s.realVol - q);
          if (static_cast<int8_t>(s.realVol) < 0) s.realVol = 0;
          s.outVol = s.realVol;
          s.status |= kUpdVol;
          break;
        }
        case 0xC:
          if (v == 0) {
            s.realVol = 0;
            s.outVol = 0;
            s.status |= kUpdVol | kQuickRamp;
          }
          break;
        case 0xE: fx.delay = v; break;
        default: break;
      }
      break;
    }
    case 0xF:
      if (p >= 32) fx.bpm = p;
      else fx.speed = p;
      break;
    case 0x10: fx.globalVol = p > 64 ? 64 : p; break;  // Gxx
    case 0x15: setEnvelopePos(p); break;                // Lxx
    default: break;
  }
}

void Channel::tickZeroEffects(SongFx& fx) {
  uint8_t vc = s.volColumn;  // changed by the volume column, then read by Rxy (an FT2 quirk)
  switch (s.volColumn >> 4) {
    case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: {
      vc = static_cast<uint8_t>(vc - 16);
      if (vc > 64) vc = 64;
      s.outVol = s.realVol = vc;
      s.status |= kUpdVol | kQuickRamp;
      break;
    }
    case 0x8: {
      vc = static_cast<uint8_t>(static_cast<uint8_t>(0 - (s.volColumn & 0x0F)) + s.realVol);
      if (static_cast<int8_t>(vc) < 0) vc = 0;
      s.outVol = s.realVol = vc;
      s.status |= kUpdVol;
      break;
    }
    case 0x9: {
      vc = static_cast<uint8_t>((s.volColumn & 0x0F) + s.realVol);
      if (vc > 64) vc = 64;
      s.outVol = s.realVol = vc;
      s.status |= kUpdVol;
      break;
    }
    case 0xA:
      vc = static_cast<uint8_t>((s.volColumn & 0x0F) * 4);
      if (vc != 0) s.vibSpeed = vc;
      break;
    case 0xC:
      vc = static_cast<uint8_t>(vc << 4);
      s.outPan = vc;
      s.status |= kUpdPan;
      break;
    default: break;
  }
  const uint8_t p = s.efxData;
  if (s.efx == 0 && p == 0) return;
  if (s.efx == 8) {
    s.outPan = p;
    s.status |= kUpdPan;
  } else if (s.efx == 12) {
    s.outVol = s.realVol = p > 64 ? 64 : p;
    s.status |= kUpdVol | kQuickRamp;
  } else if (s.efx == 27) {  // Rxy
    uint8_t t = p & 0x0F;
    if (t == 0) t = s.retrigSpeed;
    s.retrigSpeed = t;
    t = p >> 4;
    if (t == 0) t = s.retrigVol;
    s.retrigVol = t;
    if (vc == 0) doRetrig();
  } else if (s.efx == 33) {  // X1x / X2x
    const uint8_t type = p >> 4;
    uint8_t q = p & 0x0F;
    if (type == 1) {
      if (q == 0) q = s.efPitchUp;
      s.efPitchUp = q;
      uint16_t np = static_cast<uint16_t>(s.realPeriod - q);
      if (static_cast<int16_t>(np) < 1) np = 1;
      s.outPeriod = s.realPeriod = np;
      s.status |= kUpdPeriod;
    } else if (type == 2) {
      if (q == 0) q = s.efPitchDown;
      s.efPitchDown = q;
      uint16_t np = static_cast<uint16_t>(s.realPeriod + q);
      if (static_cast<int16_t>(np) >= 32000) np = 32000 - 1;
      s.outPeriod = s.realPeriod = np;
      s.status |= kUpdPeriod;
    }
  }
  moreTickZero(fx);
}

void Channel::prepPorta(const Cell& c, uint8_t inst) {
  if (c.note > 0) {
    if (c.note == kNoteOff) {
      keyOff();
    } else {
      const int idx = (((c.note - 1) + s.relNote) * 16) + ((s.finetune >> 3) + 16);
      if (idx >= 0 && idx < kPeriodCount) {
        s.portaTarget = periodAt(idx, linear);
        if (s.portaTarget == s.realPeriod) s.portaDir = 0;
        else if (s.portaTarget > s.realPeriod) s.portaDir = 1;
        else s.portaDir = 2;
      }
    }
  }
  if (inst > 0) {
    resetVolumes();
    if (c.note != kNoteOff) triggerInstrument();
  }
}

void Channel::row(const Cell& c, int speed, SongFx& fx) {
  (void)speed;
  s.volColumn = c.vol;
  if (s.efx == 0) {
    if (s.efxData > 0) {  // an arpeggio ran: the period goes back
      s.outPeriod = s.realPeriod;
      s.status |= kUpdPeriod;
    }
  } else if ((s.efx == 4 || s.efx == 6) && (c.efx != 4 && c.efx != 6)) {  // a vibrato ended
    s.outPeriod = s.realPeriod;
    s.status |= kUpdPeriod;
  }
  s.efx = c.efx;
  s.efxData = c.param;
  s.copyOfInstrAndNote = static_cast<uint16_t>((c.instr << 8) | c.note);
  uint8_t inst = c.instr;
  if (inst > 0) {
    if (inst <= 128) s.instrNum = inst;
    else inst = 0;
  }
  if (c.efx == 0x0E && c.param >= 0xD1 && c.param <= 0xDF) return;  // note delay: all of it at its tick
  if (c.efx != 0x0E || c.param != 0x90) {
    if ((s.volColumn & 0xF0) == 0xF0) {  // volume column tone portamento
      const uint8_t q = s.volColumn & 0x0F;
      if (q > 0) s.portaSpeed = static_cast<uint16_t>((q << 4) * 4);
      prepPorta(c, inst);
      tickZeroEffects(fx);
      return;
    }
    if (c.efx == 3 || c.efx == 5) {
      if (c.efx != 5 && c.param != 0) s.portaSpeed = static_cast<uint16_t>(c.param * 4);
      prepPorta(c, inst);
      tickZeroEffects(fx);
      return;
    }
    if (c.efx == 0x14 && c.param == 0) {  // K00
      keyOff();
      if (inst) resetVolumes();
      tickZeroEffects(fx);
      return;
    }
    if (c.note == 0) {
      if (inst > 0) {
        resetVolumes();
        triggerInstrument();
      }
      tickZeroEffects(fx);
      return;
    }
  }
  if (c.note == kNoteOff) keyOff();
  else triggerNote(c.note, c.efx, c.param);
  if (inst > 0) {
    resetVolumes();
    if (c.note != kNoteOff) triggerInstrument();
  }
  tickZeroEffects(fx);
}

// ---- other ticks ----

void Channel::portamento() {
  if (s.portaDir == 0) return;
  if (s.portaDir > 1) {
    s.realPeriod = static_cast<uint16_t>(s.realPeriod - s.portaSpeed);
    if (static_cast<int16_t>(s.realPeriod) <= static_cast<int16_t>(s.portaTarget)) {
      s.portaDir = 1;
      s.realPeriod = s.portaTarget;
    }
  } else {
    s.realPeriod = static_cast<uint16_t>(s.realPeriod + s.portaSpeed);
    if (s.realPeriod >= s.portaTarget) {
      s.portaDir = 1;
      s.realPeriod = s.portaTarget;
    }
  }
  s.outPeriod = s.semitonePorta ? notePeriodOf(s.realPeriod, 0) : s.realPeriod;
  s.status |= kUpdPeriod;
}

uint16_t Channel::notePeriodOf(uint16_t period, uint8_t offset) const {
  const int fine = (s.finetune >> 3) + 16;
  int hi = 8 * 12 * 16, lo = 0;  // (FT2 searches 8 octaves only)
  for (int i = 0; i < 8; ++i) {
    const int t = (((lo + hi) >> 1) & ~15) + fine;
    int look = t - 8;
    if (look < 0) look = 0;
    if (period >= periodAt(look, linear)) hi = (t - fine) & ~15;
    else lo = (t - fine) & ~15;
  }
  int t = lo + fine + (offset << 4);
  if (t >= (8 * 12 * 16 + 15) - 1) t = (8 * 12 * 16 + 16) - 1;
  return periodAt(t, linear);
}

void Channel::doVibrato() {
  uint8_t v = (s.vibPos >> 2) & 0x1F;
  switch (s.vibTremCtrl & 3) {
    case 0: v = vibSine(v); break;
    case 1:
      v = static_cast<uint8_t>(v << 3);
      if (static_cast<int8_t>(s.vibPos) < 0) v = static_cast<uint8_t>(~v);
      break;
    default: v = 255; break;
  }
  v = static_cast<uint8_t>((v * s.vibDepth) >> 5);
  if (static_cast<int8_t>(s.vibPos) < 0) s.outPeriod = static_cast<uint16_t>(s.realPeriod - v);
  else s.outPeriod = static_cast<uint16_t>(s.realPeriod + v);
  s.status |= kUpdPeriod;
  s.vibPos = static_cast<uint8_t>(s.vibPos + s.vibSpeed);
}

void Channel::arpeggio(uint8_t p, int tickDown) {
  const int st = arpStep(tickDown);
  if (st == 0) s.outPeriod = s.realPeriod;
  else s.outPeriod = notePeriodOf(s.realPeriod, st == 1 ? static_cast<uint8_t>(p >> 4) : static_cast<uint8_t>(p & 0x0F));
  s.status |= kUpdPeriod;
}

void Channel::doRetrig() {
  const uint8_t cnt = static_cast<uint8_t>(s.retrigCounter + 1);
  if (cnt < s.retrigSpeed) {
    s.retrigCounter = cnt;
    return;
  }
  s.retrigCounter = 0;
  int v = s.realVol;
  switch (s.retrigVol) {
    case 0x1: v -= 1; break;
    case 0x2: v -= 2; break;
    case 0x3: v -= 4; break;
    case 0x4: v -= 8; break;
    case 0x5: v -= 16; break;
    case 0x6: v = (v >> 1) + (v >> 3) + (v >> 4); break;
    case 0x7: v >>= 1; break;
    case 0x9: v += 1; break;
    case 0xA: v += 2; break;
    case 0xB: v += 4; break;
    case 0xC: v += 8; break;
    case 0xD: v += 16; break;
    case 0xE: v = (v >> 1) + v; break;
    case 0xF: v += v; break;
    default: break;
  }
  v = v < 0 ? 0 : v > 64 ? 64 : v;
  s.realVol = static_cast<uint8_t>(v);
  s.outVol = s.realVol;
  if (s.volColumn >= 0x10 && s.volColumn <= 0x50) {
    s.outVol = static_cast<uint8_t>(s.volColumn - 0x10);
    s.realVol = s.outVol;
  } else if (s.volColumn >= 0xC0 && s.volColumn <= 0xCF) {
    s.outPan = static_cast<uint8_t>((s.volColumn & 0x0F) << 4);
  }
  triggerNote(0, 0, 0);
}

void Channel::tick(int speed, int tickDown, SongFx& fx) {
  // Volume column.
  const uint8_t vq = s.volColumn & 0x0F;
  switch (s.volColumn >> 4) {
    case 0x6: {
      uint8_t v = static_cast<uint8_t>(static_cast<uint8_t>(0 - vq) + s.realVol);
      if (static_cast<int8_t>(v) < 0) v = 0;
      s.outVol = s.realVol = v;
      s.status |= kUpdVol;
      break;
    }
    case 0x7: {
      uint8_t v = static_cast<uint8_t>(vq + s.realVol);
      if (v > 64) v = 64;
      s.outVol = s.realVol = v;
      s.status |= kUpdVol;
      break;
    }
    case 0xB:
      if (vq > 0) s.vibDepth = vq;
      doVibrato();
      break;
    case 0xD: {
      uint16_t t = static_cast<uint16_t>(s.outPan + static_cast<uint8_t>(0 - vq));
      if (t < 256) t = 0;  // (FT2: a slide of 0 sets the pan to 0)
      s.outPan = static_cast<uint8_t>(t);
      s.status |= kUpdPan;
      break;
    }
    case 0xE: {
      uint16_t t = static_cast<uint16_t>(s.outPan + vq);
      if (t > 255) t = 255;
      s.outPan = static_cast<uint8_t>(t);
      s.status |= kUpdPan;
      break;
    }
    case 0xF: portamento(); break;
    default: break;
  }
  const uint8_t p = s.efxData;
  if ((s.efx == 0 && p == 0) || s.efx > 35) return;
  const auto ticksDone = static_cast<uint8_t>(speed - tickDown);
  switch (s.efx) {
    case 0x0: arpeggio(p, tickDown); break;
    case 0x1: {
      const uint8_t q = p ? p : s.pitchUp;
      s.pitchUp = q;
      s.realPeriod = static_cast<uint16_t>(s.realPeriod - q * 4);
      if (static_cast<int16_t>(s.realPeriod) < 1) s.realPeriod = 1;
      s.outPeriod = s.realPeriod;
      s.status |= kUpdPeriod;
      break;
    }
    case 0x2: {
      const uint8_t q = p ? p : s.pitchDown;
      s.pitchDown = q;
      s.realPeriod = static_cast<uint16_t>(s.realPeriod + q * 4);
      if (static_cast<int16_t>(s.realPeriod) >= 32000) s.realPeriod = 32000 - 1;
      s.outPeriod = s.realPeriod;
      s.status |= kUpdPeriod;
      break;
    }
    case 0x3: portamento(); break;
    case 0x4:
      if (p > 0) {
        if (p & 0x0F) s.vibDepth = p & 0x0F;
        if ((p & 0xF0) >> 2) s.vibSpeed = static_cast<uint8_t>((p & 0xF0) >> 2);
      }
      doVibrato();
      break;
    case 0x5:
      portamento();
      volSlide(p);
      break;
    case 0x6:
      doVibrato();
      volSlide(p);
      break;
    case 0x7: {
      if (p > 0) {
        if (p & 0x0F) s.tremDepth = p & 0x0F;
        if ((p & 0xF0) >> 2) s.tremSpeed = static_cast<uint8_t>((p & 0xF0) >> 2);
      }
      uint8_t t = (s.tremPos >> 2) & 0x1F;
      switch ((s.vibTremCtrl >> 4) & 3) {
        case 0: t = vibSine(t); break;
        case 1:
          t = static_cast<uint8_t>(t << 3);
          if (static_cast<int8_t>(s.vibPos) < 0) t = static_cast<uint8_t>(~t);  // (FT2 reads the vibrato position here)
          break;
        default: t = 255; break;
      }
      t = static_cast<uint8_t>((t * s.tremDepth) >> 6);
      int v;
      if (static_cast<int8_t>(s.tremPos) < 0) {
        v = s.realVol - t;
        if (v < 0) v = 0;
      } else {
        v = s.realVol + t;
        if (v > 64) v = 64;
      }
      s.outVol = static_cast<uint8_t>(v);
      s.status |= kUpdVol;
      s.tremPos = static_cast<uint8_t>(s.tremPos + s.tremSpeed);
      break;
    }
    case 0xA: volSlide(p); break;
    case 0xE: {
      const uint8_t e = p >> 4, v = p & 0x0F;
      if (e == 0x9) {
        if (v != 0 && (speed - tickDown) % v == 0) {
          triggerNote(0, 0, 0);
          triggerInstrument();
        }
      } else if (e == 0xC) {
        if (ticksDone == v) {
          s.outVol = s.realVol = 0;
          s.status |= kUpdVol | kQuickRamp;
        }
      } else if (e == 0xD) {
        if (ticksDone == v) {
          triggerNote(static_cast<uint8_t>(s.copyOfInstrAndNote & 0xFF), 0, 0);
          if ((s.copyOfInstrAndNote >> 8) > 0) resetVolumes();
          triggerInstrument();
          if (s.volColumn >= 0x10 && s.volColumn <= 0x50) {
            s.outVol = static_cast<uint8_t>(s.volColumn - 16);
            s.realVol = s.outVol;
          } else if (s.volColumn >= 0xC0 && s.volColumn <= 0xCF) {
            s.outPan = static_cast<uint8_t>((s.volColumn & 0x0F) << 4);
          }
        }
      }
      break;
    }
    case 0x11: {  // Hxy: the song applies it
      const uint8_t q = p ? p : s.globVolSlideSpeed;
      s.globVolSlideSpeed = q;
      fx.globalSlide = q;
      break;
    }
    case 0x14:  // Kxx
      if (ticksDone == (p & 31)) keyOff();
      break;
    case 0x19: {  // Pxy
      const uint8_t q = p ? p : s.panSlideSpeed;
      s.panSlideSpeed = q;
      int np = s.outPan;
      if ((q & 0xF0) == 0) {
        np -= q;
        if (np < 0) np = 0;
      } else {
        np += q >> 4;
        if (np > 255) np = 255;
      }
      s.outPan = static_cast<uint8_t>(np);
      s.status |= kUpdPan;
      break;
    }
    case 0x1B: doRetrig(); break;  // Rxy
    case 0x1D: {                   // Txy
      const uint8_t q = p ? p : s.tremorParam;
      s.tremorParam = q;
      uint8_t sign = s.tremorPos & 0x80, data = s.tremorPos & 0x7F;
      data = static_cast<uint8_t>(data - 1);
      if (static_cast<int8_t>(data) < 0) {
        if (sign == 0x80) {
          sign = 0x00;
          data = q & 0x0F;
        } else {
          sign = 0x80;
          data = q >> 4;
        }
      }
      s.tremorPos = static_cast<uint8_t>(sign | data);
      s.outVol = sign == 0x80 ? s.realVol : 0;
      s.status |= kUpdVol | kQuickRamp;
      break;
    }
    default: break;
  }
}

// ---- every tick ----

namespace {
// One envelope step; returns the 8.8 value.
int16_t stepEnvelope(const Envelope& e, bool keyOff, uint16_t& eTick, int16_t& eDelta, int16_t& eValue, uint8_t& ePos) {
  bool interpolated = false;
  uint8_t pos = ePos;
  int16_t val = 0;
  ++eTick;
  if (pos < kEnvPoints && eTick == static_cast<uint16_t>(e.x[pos])) {
    eValue = static_cast<int16_t>(static_cast<int8_t>(e.y[pos]) * 256);
    ++pos;
    if (e.flags & kEnvLoop) {
      --pos;
      if (pos == e.loopEnd) {
        if (!(e.flags & kEnvSustain) || pos != e.sustain || !keyOff) {
          pos = e.loopStart;
          eTick = static_cast<uint16_t>(e.x[pos % kEnvPoints]);
          eValue = static_cast<int16_t>(static_cast<int8_t>(e.y[pos % kEnvPoints]) * 256);
        }
      }
      ++pos;
    }
    if (pos < e.count) {
      bool interp = true;
      if ((e.flags & kEnvSustain) && !keyOff && pos - 1 == e.sustain) {
        --pos;
        eDelta = 0;
        interp = false;
      }
      if (interp) {
        ePos = pos;
        const int x0 = e.x[static_cast<size_t>((pos - 1) % kEnvPoints)], x1 = e.x[static_cast<size_t>(pos % kEnvPoints)];
        const int xd = static_cast<int16_t>(x1 - x0);
        if (xd > 0) {
          const int y0 = e.y[static_cast<size_t>((pos - 1) % kEnvPoints)], y1 = e.y[static_cast<size_t>(pos % kEnvPoints)];
          const int yd = static_cast<int8_t>(y1 - y0);
          eDelta = static_cast<int16_t>((yd * 256) / xd);
          val = eValue;
          interpolated = true;
        } else {
          eDelta = 0;
        }
      }
    } else {
      eDelta = 0;
    }
  }
  if (!interpolated) {
    eValue = static_cast<int16_t>(eValue + eDelta);
    val = eValue;
    const auto hi = static_cast<uint8_t>(static_cast<uint16_t>(val) >> 8);
    if (hi > 64) {
      val = hi <= 160 ? static_cast<int16_t>(64 * 256) : static_cast<int16_t>(0);
      eDelta = 0;
    }
  }
  return val;
}
}  // namespace

void Channel::updateEnvelopes() {
  const InstrumentDef& in = instrument();
  if (s.keyOff) {
    if (s.fadeoutSpeed > s.fadeoutVol) {
      s.fadeoutVol = 0;
      s.fadeoutSpeed = 0;
    } else {
      s.fadeoutVol = static_cast<uint16_t>(s.fadeoutVol - s.fadeoutSpeed);
    }
    s.status |= kUpdVol;
  }
  if (!in.mute) {
    const int32_t vol = globalVol * s.outVol * s.fadeoutVol;
    float fv = static_cast<float>(vol) * (1.0f / (64.0f * 64.0f * 32768.0f));
    if (in.vol.on()) {
      const int16_t ev = stepEnvelope(in.vol, s.keyOff != 0, s.volEnvTick, s.volEnvDelta, s.volEnvValue, s.volEnvPos);
      float fe = static_cast<float>(static_cast<uint16_t>(ev)) * (1.0f / (64.0f * 256.0f));
      if (fe > 1.0f) fe = 1.0f;
      fv *= fe;
      s.status |= kUpdVol;
    }
    s.finalVol = fv > 1.0f ? 1.0f : fv;
  } else {
    s.finalVol = 0.0f;
  }
  if (in.pan.on()) {
    int16_t ev = stepEnvelope(in.pan, s.keyOff != 0, s.panEnvTick, s.panEnvDelta, s.panEnvValue, s.panEnvPos);
    int16_t mul = static_cast<int16_t>(s.outPan - 128);
    if (mul >= 0) mul = static_cast<int16_t>(-mul);
    mul = static_cast<int16_t>((mul + 128) << 3);
    ev = static_cast<int16_t>(ev - 32 * 256);
    const auto add = static_cast<int8_t>((ev * mul) >> 16);
    s.finalPan = static_cast<uint8_t>(s.outPan + add);
    s.status |= kUpdPan;
  } else {
    s.finalPan = s.outPan;
  }
  if (in.vibDepth > 0) {
    uint16_t amp;
    if (s.autoVibSweep > 0) {
      amp = s.autoVibSweep;
      if (!s.keyOff) {
        amp = static_cast<uint16_t>(amp + s.autoVibAmp);
        if ((amp >> 8) > in.vibDepth) {
          amp = static_cast<uint16_t>(in.vibDepth << 8);
          s.autoVibSweep = 0;
        }
        s.autoVibAmp = amp;
      }
    } else {
      amp = s.autoVibAmp;
    }
    s.autoVibPos = static_cast<uint8_t>(s.autoVibPos + in.vibRate);
    int v;
    if (in.vibType == 1) v = s.autoVibPos > 127 ? 64 : -64;
    else if (in.vibType == 2) v = (((s.autoVibPos >> 1) + 64) & 127) - 64;
    else if (in.vibType == 3) v = ((-(s.autoVibPos >> 1) + 64) & 127) - 64;
    else v = autoVibSine(s.autoVibPos);
    v = static_cast<int16_t>((v * static_cast<int16_t>(amp)) >> (6 + 8));
    uint16_t per = static_cast<uint16_t>(s.outPeriod + v);
    if (per >= 32000) per = 0;
    s.finalPeriod = per;
    s.status |= kUpdPeriod;
  } else {
    s.finalPeriod = s.outPeriod;
  }
}

}  // namespace gl::tracker
