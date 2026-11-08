// Gridline - tracker modules (K-76): building a song from a module. See TrackerImport.h.
#include "TrackerImport.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <utility>
#include "Params.h"
#include "PluginInfo.h"
#include "TrackerData.h"
#include "TrackerStream.h"

namespace gl::tracker {

namespace {

namespace fs = std::filesystem;

constexpr int kMaxRowInstances = 100000;
constexpr Rgb kColors[] = {0x5A8FD8, 0xD8775A, 0x6EB55E, 0xC9A23F, 0x9A6CD0, 0x4FB3B0, 0xD0607E, 0x8C9A5B};

// Seconds of one tick at `bpm`, as the tracker counts them (whole samples at its 44 kHz rate).
double tickSeconds(int bpm) {
  bpm = std::clamp(bpm, 32, 255);
  return static_cast<double>(110000 / bpm) / 44000.0;
}

int32_t sampleEnd(const SampleDef& s) { return s.loop == kLoopOff ? s.length : s.loopStart + s.loopLength; }

bool isPorta(const Cell& c) { return c.efx == 3 || c.efx == 5 || (c.vol & 0xF0) == 0xF0; }

// One row as it played (several passes of it with a pattern delay).
struct RowInst {
  int order = 0, pattern = 0, row = 0;
  int speed = 6, passes = 1, bpm = 125;
  int segment = 0;
  double start = 0;  // seconds
};

// One tracker channel on one played row.
struct LaneRow {
  int8_t trig = -1;     // a new voice starts at this tick of the row's first pass (-1 = none)
  bool silent = false;  // ... playing nothing (an empty sample): it only ends the voice before
  bool alive = false;   // the voice's sample still plays after the row
  bool faded = false;   // after a key off, silent (faded out or no volume)
  uint8_t instr = 0;    // the new voice's instrument
  uint8_t note = 0;     // and its note
  int32_t pos = 0;      // the sample position at the row's start (estimate)
};

struct GvEvent {
  int rowIndex = 0, tick = 0, value = 64;
};

// Plays the module through once, recording rows, voices and the global volume.
struct Sim {
  const Module& m;
  std::array<Channel, kMaxChannels> ch{};
  std::array<const InstrumentDef*, kMaxInstruments + 1> bank{};
  struct Track {
    bool on = false;
    const SampleDef* s = nullptr;
    double pos = 0;
    bool back = false;
  };
  std::array<Track, kMaxChannels> trk{};
  // Song state (the tracker's own variables).
  int songPos = 0, row = 0, speed = 6, bpm = 125, tick = 1, gv = 64;
  int pBreakPos = 0, pattDelTime = 0, pattDelTime2 = 0;
  bool pBreakFlag = false, posJumpFlag = false, bxxOverflow = false, wrapped = false, stopped = false;
  std::array<int, kMaxChannels> loopStart{}, loopCounter{};
  // Results.
  std::vector<RowInst> rows;
  std::array<std::vector<LaneRow>, kMaxChannels> lanes;
  std::unordered_map<uint64_t, State> snaps;  // (row index << 5 | lane) -> state before the row
  std::vector<GvEvent> gvEvents;
  int loopTarget = -1;  // row index the song continues at after its end (-1 = it ends)
  bool cut = false;     // stopped at the length limit

  explicit Sim(const Module& mod) : m(mod) {
    for (int i = 1; i <= kMaxInstruments && i < static_cast<int>(m.instruments.size()); ++i) bank[static_cast<size_t>(i)] = &m.instruments[static_cast<size_t>(i)].def;
    for (auto& c : ch) {
      c.reset();
      c.bank = bank.data();
      c.bankSize = static_cast<int>(bank.size());
      c.linear = m.linear;
      c.globalVol = 64;
    }
    speed = m.speed;
    bpm = m.bpm;
  }

  int orderCount() const { return static_cast<int>(m.orders.size()); }
  const Pattern& pattern(int pos) const { return m.patterns[m.orders[static_cast<size_t>(std::clamp(pos, 0, orderCount() - 1))]]; }
  int rowsOf(int pos) const { return pattern(pos).rows; }

  void applyFx(int lane, const SongFx& fx, int rowIndex, int tickInRow) {
    if (fx.speed >= 0) {
      if (fx.speed == 0) stopped = true;
      else tick = speed = fx.speed;
    }
    if (fx.bpm >= 0) bpm = std::clamp(fx.bpm, 32, 255);
    if (fx.jump >= 0) {
      const int pos = fx.jump - 1;
      if (pos < 0 || pos >= orderCount()) bxxOverflow = true;
      else songPos = pos;
      pBreakPos = 0;
      posJumpFlag = true;
    }
    if (fx.breakRow >= 0) {
      pBreakPos = fx.breakRow;
      posJumpFlag = true;
    }
    if (fx.loop >= 0) {
      auto& ls = loopStart[static_cast<size_t>(lane)];
      auto& lc = loopCounter[static_cast<size_t>(lane)];
      if (fx.loop == 0) {
        ls = row & 0xFF;
      } else if (lc == 0) {
        lc = fx.loop;
        pBreakPos = ls;
        pBreakFlag = true;
      } else if (--lc > 0) {
        pBreakPos = ls;
        pBreakFlag = true;
      }
    }
    if (fx.delay >= 0 && pattDelTime2 == 0) pattDelTime = fx.delay + 1;
    const int before = gv;
    if (fx.globalVol >= 0) gv = std::min(fx.globalVol, 64);
    if (fx.globalSlide >= 0) {
      const int q = fx.globalSlide;
      if ((q & 0xF0) == 0) gv = std::max(0, gv - q);
      else gv = std::min(64, gv + (q >> 4));
    }
    if (gv != before) gvEvents.push_back({rowIndex, tickInRow, gv});
  }

  // After a channel's tick: the sample it plays (triggers, position).
  void follow(int lane, uint8_t st, double secs) {
    Channel& c = ch[static_cast<size_t>(lane)];
    Track& t = trk[static_cast<size_t>(lane)];
    if (st & kTrigger) {
      const SampleDef& s = c.sample();
      t.s = &s;
      t.on = s.present() && c.s.smpStartPos < sampleEnd(s);
      t.pos = c.s.smpStartPos;
      t.back = false;
    }
    if (!t.on || !t.s) return;
    const double hz = periodHz(c.s.finalPeriod, m.linear);
    double step = hz * secs;
    const SampleDef& s = *t.s;
    if (s.loop == kLoopOff) {
      t.pos += step;
      if (t.pos >= s.length) t.on = false;
      return;
    }
    const double ls = s.loopStart, le = s.loopStart + s.loopLength, len = s.loopLength;
    if (s.loop == kLoopForward) {
      t.pos += step;
      if (t.pos >= le) t.pos = ls + std::fmod(t.pos - le, len);
      return;
    }
    // Ping-pong: unfolded into one forward-and-back cycle of twice the loop's length.
    double u = t.back ? 2 * le - t.pos : t.pos;
    u += step;
    if (u >= le) u = ls + std::fmod(u - ls, 2 * len);
    if (u < le) {
      t.pos = u;
      t.back = false;
    } else {
      t.pos = 2 * le - u;
      t.back = true;
    }
  }

  void run(double maxSeconds) {
    std::map<std::pair<int, int>, int> visited;  // (order, row) at a row start with no loop running -> row index
    std::array<State, kMaxChannels> before{};
    double secs = 0;
    int cur = -1;            // current row index
    int tickInRow = 0;       // tick within the current row instance (passes included)
    int lastOrder = -1, lastRow = -1;
    while (!stopped) {
      // The song's end, as the tracker's song renderer sees it.
      if (wrapped && row == 0 && tick == 1) break;
      bool tickZero = false;
      if (--tick <= 0) {
        tick = speed;
        tickZero = true;
      }
      const bool readNew = tickZero && pattDelTime2 == 0;
      if (readNew) {
        // A new row starts.
        bool loopsRunning = false;
        for (int k = 0; k < m.channels; ++k) loopsRunning = loopsRunning || loopCounter[static_cast<size_t>(k)] != 0;
        const auto key = std::make_pair(songPos, row);
        if (!loopsRunning) {
          auto it = visited.find(key);
          if (it != visited.end() && !(lastOrder == songPos && lastRow + 1 == row)) {
            loopTarget = it->second;
            break;
          }
          visited.emplace(key, static_cast<int>(rows.size()));
        }
        if (static_cast<int>(rows.size()) >= kMaxRowInstances || secs >= maxSeconds) {
          cut = true;
          break;
        }
        RowInst ri;
        ri.order = songPos;
        ri.pattern = m.orders[static_cast<size_t>(songPos)];
        ri.row = row;
        ri.start = secs;
        const bool continues = cur >= 0 && lastOrder == songPos && lastRow + 1 == row && rows[static_cast<size_t>(cur)].pattern == ri.pattern;
        ri.segment = cur < 0 ? 0 : rows[static_cast<size_t>(cur)].segment + (continues ? 0 : 1);
        rows.push_back(ri);
        cur = static_cast<int>(rows.size()) - 1;
        tickInRow = 0;
        lastOrder = songPos;
        lastRow = row;
        for (int k = 0; k < kMaxChannels; ++k) {
          lanes[static_cast<size_t>(k)].emplace_back();
          before[static_cast<size_t>(k)] = ch[static_cast<size_t>(k)].s;
          lanes[static_cast<size_t>(k)].back().pos = static_cast<int32_t>(trk[static_cast<size_t>(k)].on ? trk[static_cast<size_t>(k)].pos : 0);
        }
        const bool segStart = !continues;
        const Pattern& p = m.patterns[static_cast<size_t>(ri.pattern)];
        for (int k = 0; k < m.channels; ++k) {
          Channel& c = ch[static_cast<size_t>(k)];
          const Cell& cell = p.at(row, k);
          SongFx fx;
          c.row(cell, speed, fx);
          applyFx(k, fx, cur, 0);
          c.updateEnvelopes();
          const uint8_t st = c.takeStatus();
          LaneRow& lr = lanes[static_cast<size_t>(k)].back();
          if ((st & kTrigger) && cell.note >= 1 && cell.note <= 96 && !isPorta(cell)) {
            lr.trig = 0;
            lr.instr = c.s.instrNum;
            lr.note = cell.note;
            lr.silent = !c.sample().present() || c.s.smpStartPos >= sampleEnd(c.sample());
          }
          follow(k, st, 0);
          const uint64_t sk = (static_cast<uint64_t>(cur) << 5) | static_cast<uint64_t>(k);
          if (segStart || lr.trig >= 0 || (cell.efx == 0xE && (cell.param >> 4) == 0xD)) snaps[sk] = before[static_cast<size_t>(k)];
        }
      } else {
        if (tickZero && cur >= 0) rows[static_cast<size_t>(cur)].passes++;
        for (int k = 0; k < m.channels; ++k) {
          Channel& c = ch[static_cast<size_t>(k)];
          SongFx fx;
          c.tick(speed, tick, fx);
          applyFx(k, fx, cur, tickInRow);
          c.updateEnvelopes();
          const uint8_t st = c.takeStatus();
          if (cur >= 0) {
            LaneRow& lr = lanes[static_cast<size_t>(k)][static_cast<size_t>(cur)];
            const RowInst& ri = rows[static_cast<size_t>(cur)];
            const Cell& cell = m.patterns[static_cast<size_t>(ri.pattern)].at(ri.row, k);
            // A delayed note starting on its tick (first pass).
            if ((st & kTrigger) && lr.trig < 0 && tickInRow < ri.speed && cell.efx == 0xE && (cell.param >> 4) == 0xD && cell.note >= 1 &&
                cell.note <= 96 && tickInRow == (cell.param & 15)) {
              lr.trig = static_cast<int8_t>(tickInRow);
              lr.instr = c.s.instrNum;
              lr.note = cell.note;
              lr.silent = !c.sample().present() || c.s.smpStartPos >= sampleEnd(c.sample());
            }
          }
          follow(k, st, 0);
        }
      }
      if (cur >= 0) {
        RowInst& ri = rows[static_cast<size_t>(cur)];
        if (tickZero && readNew) {
          ri.speed = speed;
          ri.bpm = bpm;
        }
      }
      // The tick's audio: every sample moves on.
      const double ts = tickSeconds(bpm);
      for (int k = 0; k < m.channels; ++k) follow(k, 0, ts);
      secs += ts;
      ++tickInRow;
      if (cur >= 0 && tick == 1) {
        // The row (or one of its passes) ends: what sounds after it.
        for (int k = 0; k < m.channels; ++k) {
          LaneRow& lr = lanes[static_cast<size_t>(k)][static_cast<size_t>(cur)];
          const Channel& c = ch[static_cast<size_t>(k)];
          lr.alive = trk[static_cast<size_t>(k)].on;
          lr.faded = c.s.keyOff && c.s.finalVol <= 0.0f && (c.s.fadeoutVol == 0 || !c.instrument().vol.on() || (c.s.volEnvDelta == 0 && c.s.volEnvValue == 0));
        }
      }
      nextPos();
    }
    if (wrapped && loopTarget < 0) {
      // The song plays on from its restart position: where that first played.
      for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].order == m.restart && rows[i].row == 0) {
          loopTarget = static_cast<int>(i);
          break;
        }
    }
  }

  void nextPos() {
    if (tick != 1) return;
    ++row;
    if (pattDelTime > 0) {
      pattDelTime2 = pattDelTime;
      pattDelTime = 0;
    }
    if (pattDelTime2 > 0) {
      if (--pattDelTime2 > 0) --row;
    }
    if (pBreakFlag) {
      pBreakFlag = false;
      row = pBreakPos;
    }
    if (row >= rowsOf(songPos) || posJumpFlag) {
      row = pBreakPos;
      pBreakPos = 0;
      posJumpFlag = false;
      if (bxxOverflow) {
        songPos = 0;
        bxxOverflow = false;
      } else if (++songPos >= orderCount()) {
        wrapped = true;
        songPos = m.restart;
      }
      if (row >= rowsOf(songPos)) row = 0;
    }
  }
};

// Cells that bring a voice back after its sample ended / after it faded out.
bool retriggers(const Cell& c) {
  if (c.efx == 0x1B) return true;
  if (c.efx == 0xE && (c.param >> 4) == 0x9 && (c.param & 15) > 0) return true;
  if (c.efx == 0xE && (c.param >> 4) == 0xD && (c.param & 15) > 0) return true;
  return false;
}
bool revivesVolume(const Cell& c) {
  if (retriggers(c) || c.instr > 0) return true;
  if (c.vol >= 0x10 && c.vol <= 0x50) return true;
  const int vh = c.vol >> 4;
  if (vh == 0x7 || vh == 0x9) return true;
  if (c.efx == 0xC || c.efx == 0xA || c.efx == 0x5 || c.efx == 0x6 || c.efx == 0x7 || c.efx == 0x1D) return true;
  if (c.efx == 0xE && (c.param >> 4) == 0xA) return true;
  return false;
}

struct SegNote {
  int instrument = 0;
  Note note;
};

// Where a note's stored channel state sits in its stream (none: [4, 4)).
std::pair<size_t, size_t> stateRange(const Note& n) {
  if (n.tracker.size() >= 4 + sizeof(State) && (n.tracker[1] & kHasState)) return {4, 4 + sizeof(State)};
  return {4, 4};
}

// The notes without their stored states: two places in the song that play the same cells.
uint64_t hashNotes(const std::vector<SegNote>& v) {
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&](uint64_t x) {
    h ^= x;
    h *= 0x100000001b3ull;
  };
  for (const auto& n : v) {
    mix(static_cast<uint64_t>(n.instrument));
    mix(static_cast<uint64_t>(n.note.start));
    mix(static_cast<uint64_t>(n.note.length));
    mix(static_cast<uint64_t>(n.note.pitch));
    const auto [a, b] = stateRange(n.note);
    for (size_t i = 0; i < n.note.tracker.size(); ++i)
      if (i < a || i >= b) mix(n.note.tracker[i]);
  }
  return h;
}

// The faders at 0 dB: channel volume and mixer volume (their law is (value / unity) squared).
constexpr float kUnityChannel = 0.78f, kUnityMixer = 0.8f;
// The mixer fader value that plays at gain `g`.
double faderFor(double g) { return kUnityMixer * std::sqrt(std::max(0.0, g)); }

bool sameCells(const Note& x, const Note& y) {
  if (x.start != y.start || x.length != y.length || x.pitch != y.pitch || x.velocity != y.velocity || x.tracker.size() != y.tracker.size()) return false;
  const auto [a, b] = stateRange(x);
  for (size_t i = 0; i < x.tracker.size(); ++i)
    if ((i < a || i >= b) && x.tracker[i] != y.tracker[i]) return false;
  return true;
}

// Two notes with the same cells but different stored states sound the same: the instrument
// plays both tick for tick to the same volume, pitch, pan and sample (and a note that hands its
// voice on leaves it in the same state). Then one pattern serves both places.
bool soundsTheSame(const InstrumentDef& def, bool linear, const Note& x, const Note& y) {
  StreamHeader hx, hy;
  StreamReader rx, ry;
  if (!rx.open(x.tracker.data(), x.tracker.size(), hx) || !ry.open(y.tracker.data(), y.tracker.size(), hy)) return false;
  if (std::memcmp(&hx.state, &hy.state, sizeof(State)) == 0) return true;
  if (hx.flags & kTie) return false;  // a continuation takes over the sounding voice: only the same state
  Channel cx, cy;
  for (Channel* c : {&cx, &cy}) {
    c->reset();
    c->fixed = &def;
    c->linear = linear;
    c->globalVol = 64;
  }
  cx.s = hx.state;
  cy.s = hy.state;
  cx.s.status = cy.s.status = 0;
  for (int guard = 0; guard < 4096; ++guard) {
    const StreamRow r = rx.next();
    ry.next();
    if (rx.done()) break;
    const int speed = std::max<int>(1, r.speed), ticks = speed * std::max<int>(1, r.passes);
    for (int t = 0; t < ticks; ++t) {
      if (r.endTick >= 0 && r.endTick < ticks && t == r.endTick) return true;
      SongFx fx;
      const int j = t % speed;
      if (t == 0) {
        cx.row(r.cell, speed, fx);
        cy.row(r.cell, speed, fx);
      } else {
        cx.tick(speed, j == 0 ? speed : speed - j, fx);
        cy.tick(speed, j == 0 ? speed : speed - j, fx);
      }
      cx.updateEnvelopes();
      cy.updateEnvelopes();
      const uint8_t sx = cx.takeStatus(), sy = cy.takeStatus();
      if (sx != sy || cx.s.finalVol != cy.s.finalVol || cx.s.finalPeriod != cy.s.finalPeriod || cx.s.finalPan != cy.s.finalPan ||
          cx.s.smpNum != cy.s.smpNum || ((sx & kTrigger) && cx.s.smpStartPos != cy.s.smpStartPos))
        return false;
    }
  }
  if (hx.flags & kHandOver) {
    cx.s.status = cy.s.status = 0;
    return std::memcmp(&cx.s, &cy.s, sizeof(State)) == 0;
  }
  return true;
}

bool writeWav16(const fs::path& path, const std::vector<int16_t>& data, int rate) {
  std::ofstream o(path, std::ios::binary | std::ios::trunc);
  if (!o) return false;
  auto u32 = [&](uint32_t v) {
    const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF), static_cast<char>((v >> 16) & 0xFF), static_cast<char>(v >> 24)};
    o.write(b, 4);
  };
  auto u16 = [&](uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>(v >> 8)};
    o.write(b, 2);
  };
  const uint32_t bytes = static_cast<uint32_t>(data.size() * 2);
  o.write("RIFF", 4);
  u32(36 + bytes);
  o.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(1);
  u32(static_cast<uint32_t>(rate));
  u32(static_cast<uint32_t>(rate * 2));
  u16(2);
  u16(16);
  o.write("data", 4);
  u32(bytes);
  for (int16_t s : data) u16(static_cast<uint16_t>(s));
  return static_cast<bool>(o);
}

std::string twoDigits(int v) {
  char b[16];
  std::snprintf(b, sizeof b, "%02d", v);
  return b;
}

}  // namespace

std::string sampleFileName(int instrument, int slot) {
  char b[48];
  std::snprintf(b, sizeof b, "instrument%03d-sample%02d.wav", instrument, slot);
  return b;
}

bool writeSamples(const Module& m, const std::string& folderIn, std::vector<std::string>& names, std::string* err) {
  const fs::path folder = fs::u8path(folderIn);
  std::error_code ec;
  fs::create_directories(folder, ec);
  for (int i = 1; i <= kMaxInstruments && i < static_cast<int>(m.instruments.size()); ++i)
    for (int k = 0; k < kMaxSamples; ++k) {
      const Sample& s = m.instruments[static_cast<size_t>(i)].samples[static_cast<size_t>(k)];
      if (s.data.empty()) continue;
      const std::string fn = sampleFileName(i, k);
      if (!writeWav16(folder / fs::u8path(fn), s.data, 8363)) {
        if (err) *err = "cannot write the sample file " + (folder / fs::u8path(fn)).u8string();
        return false;
      }
      names.push_back(fn);
    }
  return true;
}

bool buildSong(const Module& m, const std::string& name, const ImportOptions& o, Song& song, ImportReport& rep, std::string* err) {
  rep = ImportReport{};
  rep.warnings = m.warnings;
  if (m.orders.empty() || m.patterns.size() < kMaxPatterns || m.instruments.size() < static_cast<size_t>(kMaxInstruments + 1)) {
    if (err) *err = "the module has no song";
    return false;
  }
  Sim sim(m);
  sim.run(o.maxSeconds);
  const int N = static_cast<int>(sim.rows.size());
  if (N == 0) {
    if (err) *err = "the module plays nothing";
    return false;
  }
  if (sim.cut) rep.warnings.push_back("the song does not end: it was cut after " + std::to_string(static_cast<int>(sim.rows.back().start / 60)) + " minutes");

  // ---- timing: rows per beat so every row's tempo fits the song tempo's range ----
  std::vector<double> rowSecs(static_cast<size_t>(N));
  for (int i = 0; i < N; ++i) {
    const RowInst& r = sim.rows[static_cast<size_t>(i)];
    rowSecs[static_cast<size_t>(i)] = r.speed * r.passes * tickSeconds(r.bpm);
  }
  int rpb = 4;
  {
    int best = -1, bestFit = -1;
    for (int cand : {4, 8, 2, 16, 1}) {
      int fit = 0;
      for (double s : rowSecs) {
        const double t = 60.0 / (cand * s);
        fit += (t >= 10.0 && t <= 522.0) ? 1 : 0;
      }
      if (fit > bestFit) {
        bestFit = fit;
        best = cand;
      }
      if (fit == N) break;
    }
    rpb = best;
    if (bestFit < N) rep.warnings.push_back("the tempo goes beyond the song tempo's range on " + std::to_string(N - bestFit) + " rows: those rows play at the nearest tempo");
  }
  rep.rowsPerBeat = rpb;

  {  // (a named blank: GCC 12.2 wrongly warns that a Song{} temporary's strings may be uninitialized)
    Song blank;
    song = std::move(blank);
  }
  song.ppq = kDefaultPpq;
  const Tick rowTicks = static_cast<Tick>(song.ppq / rpb);
  song.info.title = m.title.empty() ? name : m.title;

  // ---- instruments -> channels ----
  std::array<int, kMaxInstruments + 1> chanOf{};
  chanOf.fill(-1);
  std::array<bool, kMaxInstruments + 1> used{};
  for (int k = 0; k < kMaxChannels; ++k)
    for (const auto& lr : sim.lanes[static_cast<size_t>(k)])
      if (lr.trig >= 0 && !lr.silent && lr.instr >= 1 && lr.instr <= kMaxInstruments) used[lr.instr] = true;
  std::vector<int> instrList;
  for (int i = 1; i <= kMaxInstruments; ++i)
    if (used[static_cast<size_t>(i)] || m.instruments[static_cast<size_t>(i)].hasAudio()) instrList.push_back(i);
  ensureMixer(song, std::max<int>(kDefaultInsertTracks, static_cast<int>(instrList.size())));
  song.mixer[0].volume = kUnityMixer;
  const fs::path folder = fs::u8path(o.sampleFolder);
  std::error_code ec;
  if (!o.sampleFolder.empty() && o.write) fs::create_directories(folder, ec);
  std::string comments;
  for (size_t n = 0; n < instrList.size(); ++n) {
    const int i = instrList[n];
    const Instrument& in = m.instruments[static_cast<size_t>(i)];
    InstrumentText t;
    t.name = in.name;
    t.def = in.def;
    for (int k = 0; k < kMaxSamples; ++k) {
      const Sample& s = in.samples[static_cast<size_t>(k)];
      t.sampleNames[static_cast<size_t>(k)] = s.name;
      if (s.data.empty()) continue;
      const std::string fn = sampleFileName(i, k);
      const fs::path full = folder / fs::u8path(fn);
      if (!o.sampleFolder.empty() && o.write) {
        if (!writeWav16(full, s.data, 8363)) {
          if (err) *err = "cannot write the sample file " + full.u8string();
          return false;
        }
        rep.files.push_back(fn);
      }
      t.fileOf[static_cast<size_t>(k)] = static_cast<int>(t.files.size());
      t.files.push_back(o.sampleFolder.empty() ? fn : full.u8string());
      ++rep.samples;
    }
    gl::Channel ch;
    ch.id = song.allocateId();
    ch.name = in.name.empty() ? "Instrument " + twoDigits(i) : in.name;
    ch.color = kColors[n % (sizeof kColors / sizeof kColors[0])];
    ch.plugin = defaultPluginState(kInstrumentType);
    if (ch.plugin.params.size() >= PCount) {
      ch.plugin.params[PAmiga] = m.linear ? 0.0f : 1.0f;
      ch.plugin.params[PRowsPerBeat] = static_cast<float>(rowsPerBeatChoice(rpb));
      ch.plugin.params[PTicks] = static_cast<float>(m.speed);
      ch.plugin.params[PSound] = 0.0f;  // a module instrument plays its own samples only
    }
    ch.plugin.data = encodeInstrument(t);
    ch.gain = kUnityChannel;
    MixerTrack& mt = song.mixer[n + 1];
    mt.name = ch.name;
    mt.volume = kUnityMixer;
    ch.mixerTrack = mt.id;
    chanOf[static_cast<size_t>(i)] = static_cast<int>(song.channels.size());
    song.channels.push_back(ch);
    if (!in.name.empty()) comments += in.name + "\n";
  }
  while (!comments.empty() && comments.back() == '\n') comments.pop_back();
  song.info.comments = comments;
  rep.instruments = static_cast<int>(song.channels.size());

  // ---- voices -> notes, per segment ----
  int segCount = sim.rows.back().segment + 1;
  std::vector<std::vector<SegNote>> segNotes(static_cast<size_t>(segCount));
  auto snapOf = [&](int rowIndex, int lane) -> const State* {
    auto it = sim.snaps.find((static_cast<uint64_t>(rowIndex) << 5) | static_cast<uint64_t>(lane));
    return it == sim.snaps.end() ? nullptr : &it->second;
  };
  for (int k = 0; k < m.channels; ++k) {
    const auto& lane = sim.lanes[static_cast<size_t>(k)];
    // Next row (from i on) that starts a voice / retriggers / revives the volume.
    std::vector<int> nextTrig(static_cast<size_t>(N + 1), N), nextRetrig(static_cast<size_t>(N + 1), N), nextRevive(static_cast<size_t>(N + 1), N);
    for (int i = N - 1; i >= 0; --i) {
      const RowInst& r = sim.rows[static_cast<size_t>(i)];
      const Cell& c = m.patterns[static_cast<size_t>(r.pattern)].at(r.row, k);
      nextTrig[static_cast<size_t>(i)] = lane[static_cast<size_t>(i)].trig >= 0 ? i : nextTrig[static_cast<size_t>(i + 1)];
      nextRetrig[static_cast<size_t>(i)] = retriggers(c) ? i : nextRetrig[static_cast<size_t>(i + 1)];
      nextRevive[static_cast<size_t>(i)] = revivesVolume(c) ? i : nextRevive[static_cast<size_t>(i + 1)];
    }
    auto close = [&](int start, int endEx, int endTick, uint8_t instr, uint8_t note) {
      if (endEx <= start) return;
      const int ci = instr <= kMaxInstruments ? chanOf[instr] : -1;
      if (ci < 0) return;
      int a = start;
      while (a < endEx) {
        const int seg = sim.rows[static_cast<size_t>(a)].segment;
        int b = a + 1;
        while (b < endEx && sim.rows[static_cast<size_t>(b)].segment == seg) ++b;
        const bool first = a == start, last = b == endEx;
        StreamHeader h;
        h.lane = static_cast<uint8_t>(k);
        h.flags = kHasState;
        if (!first) h.flags |= kTie;
        if (!last) h.flags |= kHandOver;
        h.startTick = first ? static_cast<uint8_t>(std::max<int>(0, lane[static_cast<size_t>(start)].trig)) : 0;
        if (const State* s = snapOf(a, k)) h.state = *s;
        if (!first) h.state.smpStartPos = lane[static_cast<size_t>(a)].pos;
        StreamWriter w(h);
        if (first && a > 0) {
          const RowInst& p0 = sim.rows[static_cast<size_t>(a - 1)];
          const RowInst& p1 = sim.rows[static_cast<size_t>(a)];
          if (rowLength(p0.bpm, p0.speed, p0.passes) != rowLength(p1.bpm, p1.speed, p1.passes))
            w.prior(static_cast<uint8_t>(std::clamp(p0.bpm, 32, 255)), static_cast<uint8_t>(std::clamp(p0.speed, 1, 255)),
                    static_cast<uint8_t>(std::clamp(p0.passes, 1, 255)));
        }
        for (int i = a; i < b; ++i) {
          const RowInst& r = sim.rows[static_cast<size_t>(i)];
          StreamRow sr;
          sr.cell = m.patterns[static_cast<size_t>(r.pattern)].at(r.row, k);
          sr.speed = static_cast<uint8_t>(std::clamp(r.speed, 1, 255));
          sr.passes = static_cast<uint8_t>(std::clamp(r.passes, 1, 255));
          sr.bpm = static_cast<uint8_t>(std::clamp(r.bpm, 32, 255));
          w.row(sr);
        }
        if (last && endTick >= 0) w.end(static_cast<uint8_t>(endTick));
        Note x(static_cast<Tick>(sim.rows[static_cast<size_t>(a)].row) * rowTicks, static_cast<Tick>(b - a) * rowTicks,
               std::clamp(note + kPitchOffset, 0, 127), 100);
        x.tracker = w.finish();
        segNotes[static_cast<size_t>(seg)].push_back({ci, std::move(x)});
        ++rep.notes;
        a = b;
      }
    };
    int vStart = -1;
    uint8_t vInstr = 0, vNote = 0;
    for (int i = 0; i < N; ++i) {
      const LaneRow& lr = lane[static_cast<size_t>(i)];
      if (lr.trig >= 0) {
        if (vStart >= 0) {
          if (lr.trig > 0) close(vStart, i + 1, lr.trig, vInstr, vNote);
          else close(vStart, i, 255, vInstr, vNote);
        }
        vStart = -1;
        if (!lr.silent) {
          vStart = i;
          vInstr = lr.instr;
          vNote = lr.note;
        }
      }
      if (vStart >= 0) {
        const int nt = nextTrig[static_cast<size_t>(i + 1)];
        const bool sampleGone = !lr.alive && nextRetrig[static_cast<size_t>(i + 1)] >= nt;
        const bool silentNow = lr.faded && nextRevive[static_cast<size_t>(i + 1)] >= nt;
        if (sampleGone || silentNow) {
          close(vStart, i + 1, -1, vInstr, vNote);
          vStart = -1;
        }
      }
    }
    if (vStart >= 0) close(vStart, N, -1, vInstr, vNote);
  }

  // ---- segments -> pattern variants and clips ----
  const int stepsPerBar = 4 * rpb;
  struct Variant {
    ObjectId id = kNoId;
    uint64_t hash = 0;
    const std::vector<SegNote>* notes = nullptr;
  };
  std::map<int, std::vector<Variant>> variants;  // module pattern -> its Gridline patterns
  for (auto& v : segNotes)
    std::stable_sort(v.begin(), v.end(), [](const SegNote& x, const SegNote& y) {
      if (x.note.start != y.note.start) return x.note.start < y.note.start;
      return x.instrument < y.instrument;
    });
  Tick at = 0;
  int segStartRow = 0;
  for (int s = 0; s < segCount; ++s) {
    int e = segStartRow;
    while (e < N && sim.rows[static_cast<size_t>(e)].segment == s) ++e;
    if (e == segStartRow) continue;
    const RowInst& r0 = sim.rows[static_cast<size_t>(segStartRow)];
    const int count = e - segStartRow;
    const std::vector<SegNote>& notes = segNotes[static_cast<size_t>(s)];
    const uint64_t h = hashNotes(notes);
    auto& vs = variants[r0.pattern];
    ObjectId pid = kNoId;
    for (const auto& v : vs) {
      if (v.hash != h || v.notes->size() != notes.size()) continue;
      bool same = true;
      for (size_t q = 0; q < notes.size() && same; ++q) {
        const SegNote& a = (*v.notes)[q];
        const SegNote& b = notes[q];
        same = a.instrument == b.instrument && sameCells(a.note, b.note) &&
               soundsTheSame(m.instruments[static_cast<size_t>(instrList[static_cast<size_t>(a.instrument)])].def, m.linear, a.note, b.note);
      }
      if (same) {
        pid = v.id;
        break;
      }
    }
    if (pid == kNoId) {
      if (static_cast<int>(song.patterns.size()) >= gl::kMaxPatterns) {
        rep.warnings.push_back("the song needs more patterns than a project holds: it was cut");
        break;
      }
      gl::Pattern pat;
      pat.id = song.allocateId();
      pat.name = "Pattern " + twoDigits(r0.pattern) + (vs.empty() ? std::string() : " (" + std::to_string(vs.size() + 1) + ")");
      pat.stepsPerBar = stepsPerBar;
      const int rows = m.patterns[static_cast<size_t>(r0.pattern)].rows;
      pat.bars = std::max(1, (rows + stepsPerBar - 1) / stepsPerBar);
      pat.loop = false;
      pat.color = 0x6E9F4B;
      for (const auto& sn : notes) pat.notes[song.channels[static_cast<size_t>(sn.instrument)].id].push_back(sn.note);
      song.patterns.push_back(std::move(pat));
      pid = song.patterns.back().id;
      vs.push_back({pid, h, &notes});
    }
    Clip c;
    c.id = song.allocateId();
    c.pattern = pid;
    c.track = 0;
    c.start = at;
    c.offset = static_cast<Tick>(r0.row) * rowTicks;
    c.length = static_cast<Tick>(count) * rowTicks;
    song.playlist.push_back(c);
    at += c.length;
    segStartRow = e;
  }
  rep.clips = static_cast<int>(song.playlist.size());
  rep.patterns = static_cast<int>(variants.size());
  rep.variants = static_cast<int>(song.patterns.size());
  rep.rows = N;
  rep.seconds = sim.rows.back().start + rowSecs.back();
  if (song.tracks.size() < 3) song.tracks.resize(3);
  song.tracks[0].name = "Song";

  // ---- tempo ----
  auto tempoOf = [&](int i) { return std::clamp(60.0 / (rpb * rowSecs[static_cast<size_t>(i)]), 10.0, 522.0); };
  song.tempo = tempoOf(0);
  bool tempoVaries = false;
  for (int i = 1; i < N && !tempoVaries; ++i) tempoVaries = std::abs(tempoOf(i) - song.tempo) > 1e-9;
  if (tempoVaries) {
    auto desc = describeParam(song, songParam(SongParam::Tempo));
    if (desc) {
      AutomationClip a;
      a.id = song.allocateId();
      a.name = "Tempo";
      a.owner = kSongId;
      a.index = static_cast<uint32_t>(SongParam::Tempo);
      double last = -1;
      for (int i = 0; i < N; ++i) {
        const double t = tempoOf(i);
        if (std::abs(t - last) < 1e-9) continue;
        last = t;
        AutoPoint p;
        p.pos = static_cast<Tick>(i) * rowTicks;
        const double norm = desc->toNormalized(t);
        p.value = static_cast<float>(norm);
        p.fine = norm - static_cast<double>(p.value);  // the exact tempo: rows land on their frame
        p.shape = AutoShape::Hold;
        a.points.push_back(p);
      }
      song.automations.push_back(a);
      Clip c;
      c.id = song.allocateId();
      c.kind = ClipKind::Automation;
      c.source = a.id;
      c.track = 1;
      c.start = 0;
      c.length = std::max<Tick>(at, 1);
      song.playlist.push_back(c);
      song.tracks[1].name = "Tempo";
    }
  }

  // ---- global volume -> master volume ----
  if (!sim.gvEvents.empty()) {
    auto desc = describeParam(song, {song.masterId(), static_cast<uint32_t>(MixerParam::Volume)});
    if (desc) {
      AutomationClip a;
      a.id = song.allocateId();
      a.name = "Global volume";
      a.owner = song.masterId();
      a.index = static_cast<uint32_t>(MixerParam::Volume);
      AutoPoint p0;
      p0.pos = 0;
      p0.value = static_cast<float>(desc->toNormalized(faderFor(1.0)));
      p0.shape = AutoShape::Hold;
      a.points.push_back(p0);
      for (const auto& ev : sim.gvEvents) {
        if (ev.rowIndex < 0 || ev.rowIndex >= N) continue;
        const RowInst& r = sim.rows[static_cast<size_t>(ev.rowIndex)];
        const double ticks = static_cast<double>(ev.rowIndex) * static_cast<double>(rowTicks) +
                             static_cast<double>(ev.tick) * static_cast<double>(rowTicks) / std::max(1, r.speed * r.passes);
        AutoPoint p;
        p.pos = static_cast<Tick>(std::floor(ticks));
        p.frac = static_cast<float>(ticks - std::floor(ticks));
        p.value = static_cast<float>(desc->toNormalized(faderFor(ev.value / 64.0)));
        p.shape = AutoShape::Hold;
        if (!a.points.empty() && a.points.back().pos == p.pos && a.points.back().frac == p.frac) a.points.back() = p;
        else a.points.push_back(p);
      }
      song.automations.push_back(a);
      Clip c;
      c.id = song.allocateId();
      c.kind = ClipKind::Automation;
      c.source = a.id;
      c.track = 2;
      c.start = 0;
      c.length = std::max<Tick>(at, 1);
      song.playlist.push_back(c);
      song.tracks[2].name = "Global volume";
    }
  }

  // ---- loop ----
  if (sim.loopTarget >= 0 && sim.loopTarget < N) {
    Marker mk;
    mk.pos = static_cast<Tick>(sim.loopTarget) * rowTicks;
    mk.name = "Loop";
    mk.action = MarkerAction::SongLoop;
    song.markers.push_back(mk);
    rep.loops = true;
  }
  return true;
}

bool importModule(const std::vector<uint8_t>& bytes, const std::string& name, const ImportOptions& o, Song& song, ImportReport& rep, std::string* err) {
  Module m;
  if (!parseModule(bytes, m, err)) return false;
  return buildSong(m, name, o, song, rep, err);
}

}  // namespace gl::tracker
