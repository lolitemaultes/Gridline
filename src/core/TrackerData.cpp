// Gridline - tracker modules (K-76): instrument text. See TrackerData.h.
#include "TrackerData.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace gl::tracker {

namespace {

const char* kHex = "0123456789abcdef";

void writeEnv(std::string& o, char which, const Envelope& e) {
  o += "env ";
  o += which;
  o += " " + std::to_string(e.flags) + " " + std::to_string(e.count) + " " + std::to_string(e.sustain) + " " +
       std::to_string(e.loopStart) + " " + std::to_string(e.loopEnd);
  for (int i = 0; i < e.count && i < kEnvPoints; ++i)
    o += " " + std::to_string(e.x[static_cast<size_t>(i)]) + ":" + std::to_string(e.y[static_cast<size_t>(i)]);
  o += "\n";
}

long num(std::istringstream& in, long lo, long hi, long def) {
  std::string w;
  if (!(in >> w)) return def;
  char* end = nullptr;
  const long v = std::strtol(w.c_str(), &end, 10);
  if (end == w.c_str()) return def;
  return std::clamp(v, lo, hi);
}

void readEnv(std::istringstream& in, Envelope& e) {
  e.flags = static_cast<uint8_t>(num(in, 0, 7, 0));
  e.count = static_cast<uint8_t>(num(in, 0, kEnvPoints, 0));
  e.sustain = static_cast<uint8_t>(num(in, 0, kEnvPoints - 1, 0));
  e.loopStart = static_cast<uint8_t>(num(in, 0, kEnvPoints - 1, 0));
  e.loopEnd = static_cast<uint8_t>(num(in, 0, kEnvPoints - 1, 0));
  std::string w;
  int i = 0;
  while (i < e.count && in >> w) {
    const size_t c = w.find(':');
    if (c == std::string::npos) break;
    e.x[static_cast<size_t>(i)] = static_cast<int16_t>(std::clamp(std::atol(w.substr(0, c).c_str()), 0L, 65535L));
    e.y[static_cast<size_t>(i)] = static_cast<int16_t>(std::clamp(std::atol(w.substr(c + 1).c_str()), 0L, 64L));
    ++i;
  }
  e.count = static_cast<uint8_t>(i);
  if (e.count == 0) e.flags = 0;
}

}  // namespace

std::string encodeInstrument(const InstrumentText& t) {
  std::string o;
  o += (t.files.empty() ? std::string() : t.files[0]) + "\n";
  for (size_t i = 1; i < t.files.size(); ++i) o += "file " + t.files[i] + "\n";
  o += "#gl-tracker 1\n";
  std::string nm = t.name;
  std::replace(nm.begin(), nm.end(), '\n', ' ');
  o += "name " + nm + "\n";
  const InstrumentDef& d = t.def;
  o += "ins " + std::to_string(d.fadeout) + " " + std::to_string(d.vibType) + " " + std::to_string(d.vibSweep) + " " +
       std::to_string(d.vibDepth) + " " + std::to_string(d.vibRate) + "\n";
  o += "map ";
  for (uint8_t k : d.keymap) o += kHex[k & 15];
  o += "\n";
  writeEnv(o, 'v', d.vol);
  writeEnv(o, 'p', d.pan);
  for (int k = 0; k < kMaxSamples; ++k) {
    const SampleDef& s = d.samples[static_cast<size_t>(k)];
    const int f = t.fileOf[static_cast<size_t>(k)];
    if (!s.present() && f < 0) continue;
    std::string sn = t.sampleNames[static_cast<size_t>(k)];
    std::replace(sn.begin(), sn.end(), '\n', ' ');
    o += "smp " + std::to_string(k) + " " + std::to_string(f) + " " + std::to_string(s.length) + " " + std::to_string(s.loopStart) + " " +
         std::to_string(s.loopLength) + " " + std::to_string(s.loop) + " " + std::to_string(s.volume) + " " + std::to_string(s.finetune) + " " +
         std::to_string(s.pan) + " " + std::to_string(s.relNote) + (sn.empty() ? "" : " " + sn) + "\n";
  }
  return o;
}

bool decodeInstrument(const std::string& data, InstrumentText& out) {
  out = InstrumentText{};
  out.def.samples.fill(SampleDef{});
  for (auto& s : out.def.samples) s.length = 0;
  std::istringstream lines(data);
  std::string line;
  size_t index = 0;
  bool body = false;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (index++ == 0) {
      if (line.rfind("#gl-tracker", 0) == 0) {
        body = true;
      } else {
        out.files.push_back(line);
      }
      continue;
    }
    if (!body) {
      if (line.rfind("file ", 0) == 0) out.files.push_back(line.substr(5));
      else if (line.rfind("#gl-tracker", 0) == 0) body = true;
      continue;
    }
    std::istringstream in(line);
    std::string key;
    in >> key;
    InstrumentDef& d = out.def;
    if (key == "name") {
      out.name = line.size() > 5 ? line.substr(5) : std::string();
    } else if (key == "ins") {
      d.fadeout = static_cast<uint16_t>(num(in, 0, 4095, 0));
      d.vibType = static_cast<uint8_t>(num(in, 0, 3, 0));
      d.vibSweep = static_cast<uint8_t>(num(in, 0, 255, 0));
      d.vibDepth = static_cast<uint8_t>(num(in, 0, 15, 0));
      d.vibRate = static_cast<uint8_t>(num(in, 0, 63, 0));
    } else if (key == "map") {
      std::string m;
      in >> m;
      for (size_t i = 0; i < d.keymap.size() && i < m.size(); ++i) {
        const char c = m[i];
        d.keymap[i] = static_cast<uint8_t>(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0);
      }
    } else if (key == "env") {
      std::string w;
      in >> w;
      if (w == "v") readEnv(in, d.vol);
      else if (w == "p") readEnv(in, d.pan);
    } else if (key == "smp") {
      const long k = num(in, -1, kMaxSamples - 1, -1);
      if (k < 0) continue;
      SampleDef s;
      const int f = static_cast<int>(num(in, -1, 1 << 20, -1));
      s.length = static_cast<int32_t>(num(in, 0, 0x7FFFFFFF, 0));
      s.loopStart = static_cast<int32_t>(num(in, 0, 0x7FFFFFFF, 0));
      s.loopLength = static_cast<int32_t>(num(in, 0, 0x7FFFFFFF, 0));
      s.loop = static_cast<uint8_t>(num(in, 0, 2, 0));
      s.volume = static_cast<uint8_t>(num(in, 0, 64, 64));
      s.finetune = static_cast<int8_t>(num(in, -128, 127, 0));
      s.pan = static_cast<uint8_t>(num(in, 0, 255, 128));
      s.relNote = static_cast<int8_t>(num(in, -96, 95, 0));
      // Loops inside the sample, as the player cleans them.
      if (s.loopStart > s.length) s.loopStart = s.length;
      if (s.loopLength > s.length - s.loopStart) s.loopLength = s.length - s.loopStart;
      if (s.loopLength < 1) s.loop = kLoopOff;
      std::string rest;
      std::getline(in, rest);
      if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
      d.samples[static_cast<size_t>(k)] = s;
      out.fileOf[static_cast<size_t>(k)] = f < static_cast<int>(out.files.size()) ? f : -1;
      out.sampleNames[static_cast<size_t>(k)] = rest;
    }
  }
  return body;
}

}  // namespace gl::tracker
