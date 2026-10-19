// Gridline — FLP import: second-wave effect decoders (plugins used across FL's
// supplied projects beyond the first three demos). Must not override names
// already registered by the first wave. The decoders live in FlpFx2*.cpp;
// this file holds their shared helpers and the registry entry point.
#include <cctype>
#include "FlpFx2.h"

namespace gl::flp {

namespace fx2 {

std::vector<uint8_t> stateBytes(const std::vector<Event>& evs) {
  const Event* e = pluginState(evs);
  return e && e->data ? std::vector<uint8_t>(e->data, e->data + e->size) : std::vector<uint8_t>{};
}

std::vector<double> ints(Reader& r, size_t count) {
  std::vector<double> v;
  for (size_t i = 0; i < count && r.has(4); ++i) v.push_back(r.i32());
  return v;
}

namespace {
void putUtf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xF0 | (cp >> 18));
    s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
}
}  // namespace

std::string utf16(const uint8_t* p, size_t units) {
  std::string s;
  for (size_t i = 0; i < units; ++i) {
    uint32_t cu = static_cast<uint32_t>(p[2 * i] | (p[2 * i + 1] << 8));
    if (cu == 0) break;
    if (cu >= 0xD800 && cu < 0xDC00 && i + 1 < units) {
      uint32_t lo = static_cast<uint32_t>(p[2 * i + 2] | (p[2 * i + 3] << 8));
      if (lo >= 0xDC00 && lo < 0xE000) {
        putUtf8(s, 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00));
        ++i;
        continue;
      }
    }
    putUtf8(s, cu);
  }
  return s;
}

std::string latin1(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && p[i]; ++i) putUtf8(s, p[i]);
  return s;
}

std::string rtfToText(const std::string& rtf) {
  if (rtf.rfind("{\\rtf", 0) != 0) return rtf;
  std::string out;
  // Groups whose text is not document text (font / colour tables, …).
  std::vector<bool> skip{false};
  size_t i = 0;
  const size_t n = rtf.size();
  while (i < n) {
    char ch = rtf[i];
    if (ch == '{') {
      skip.push_back(skip.back());
      ++i;
      if (i + 1 < n && rtf[i] == '\\' && rtf[i + 1] == '*') skip.back() = true;
      continue;
    }
    if (ch == '}') {
      if (skip.size() > 1) skip.pop_back();
      ++i;
      continue;
    }
    if (ch == '\\') {
      ++i;
      if (i >= n) break;
      char c2 = rtf[i];
      if (c2 == '\\' || c2 == '{' || c2 == '}') {
        if (!skip.back()) out += c2;
        ++i;
        continue;
      }
      if (c2 == '\'') {  // \'hh
        if (i + 2 < n) {
          int v = std::stoi(rtf.substr(i + 1, 2), nullptr, 16);
          if (!skip.back()) putUtf8(out, static_cast<uint32_t>(v));
        }
        i += 3;
        continue;
      }
      std::string word;
      while (i < n && std::isalpha(static_cast<unsigned char>(rtf[i]))) word += rtf[i++];
      std::string num;
      if (i < n && (rtf[i] == '-' || std::isdigit(static_cast<unsigned char>(rtf[i])))) {
        num += rtf[i++];
        while (i < n && std::isdigit(static_cast<unsigned char>(rtf[i]))) num += rtf[i++];
      }
      if (i < n && rtf[i] == ' ') ++i;  // delimiter
      if (word == "fonttbl" || word == "colortbl" || word == "stylesheet" || word == "info" || word == "pict")
        skip.back() = true;
      else if (!skip.back()) {
        if (word == "par" || word == "line") out += '\n';
        else if (word == "tab") out += '\t';
        else if (word == "u" && !num.empty()) {
          long cp = std::strtol(num.c_str(), nullptr, 10);
          if (cp < 0) cp += 65536;
          putUtf8(out, static_cast<uint32_t>(cp));
          if (i < n && rtf[i] == '?') ++i;  // ANSI fallback character
        }
      }
      continue;
    }
    if (ch == '\r' || ch == '\n') {
      ++i;
      continue;
    }
    if (!skip.back()) out += ch;
    ++i;
  }
  return normalizeLines(out);
}

std::string normalizeLines(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r') {
      out += '\n';
      if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
    } else {
      out += s[i];
    }
  }
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  size_t b = 0;
  while (b < out.size() && (out[b] == ' ' || out[b] == '\n')) ++b;
  return out.substr(b);
}

int nearestChoice(double v, const std::vector<double>& choices) {
  int best = 0;
  for (size_t i = 1; i < choices.size(); ++i)
    if (std::fabs(choices[i] - v) < std::fabs(choices[static_cast<size_t>(best)] - v)) best = static_cast<int>(i);
  return best;
}

void bindKnobs(PluginMapping& m, const KnobTable& table) {
  auto t = std::make_shared<const KnobTable>(table);
  fx::bindTable(m, t);
  // Pattern automation records the control's own integer value: normalize by
  // the control range (the table rows carry FL's min..max).
  m.rawNorm = [t](int flParam, int32_t raw) {
    for (const Knob& k : *t)
      if (k.flTag == flParam) {
        if (k.flMax == k.flMin) return std::nan("");
        return clampd((static_cast<double>(raw) - k.flMin) / (k.flMax - k.flMin), 0.0, 1.0);
      }
    return std::nan("");
  };
}

bool decodeEmbedded(Context& c, const std::string& flName, const uint8_t* state, size_t size, PluginMapping& out) {
  const PluginDecoder* dec = flp::findDecoder(flName);
  if (!dec) return false;
  // Synthetic event list: 201 (UTF-16 name) + 213 (state).
  auto name = std::make_shared<std::vector<uint8_t>>();
  for (char ch : flName) name->push_back(static_cast<uint8_t>(ch)), name->push_back(0);
  name->push_back(0), name->push_back(0);
  std::vector<Event> evs(2);
  evs[0].id = 201;
  evs[0].data = name->data();
  evs[0].size = static_cast<uint32_t>(name->size());
  evs[1].id = 213;
  evs[1].data = state;
  evs[1].size = static_cast<uint32_t>(size);
  out.flName = flName;
  bool ok = (*dec)(c, evs, out) && !out.glType.empty();
  if (ok && out.state.type.empty()) out.state.type = out.glType;
  if (ok) normalizePluginState(out.state);
  return ok;
}

bool isChannelPlugin(const Context& c, const std::vector<Event>& evs) {
  for (const auto& [idx, ch] : c.sec.channels)
    if (&ch == &evs) return true;
  return false;
}

}  // namespace fx2

void registerEffectDecoders2(std::map<std::string, PluginDecoder>& d) {
  std::map<std::string, PluginDecoder> mine;
  fx2::registerFx2Mod(mine);
  fx2::registerFx2Pitch(mine);
  fx2::registerFx2Tone(mine);
  fx2::registerFx2Space(mine);
  fx2::registerFx2Patcher(mine);
  for (auto& [name, dec] : mine) d.emplace(name, dec);  // never overrides the first wave
}

}  // namespace gl::flp
