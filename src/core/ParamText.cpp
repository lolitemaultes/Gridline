// Gridline - K-311: typed parameter values (see ParamText.h).
#include "ParamText.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace gl::paramtext {

namespace {

std::string squeeze(const std::string& text) {  // lower case, no spaces, decimal commas as points
  std::string s;
  for (char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isspace(u)) continue;
    s += c == ',' ? '.' : static_cast<char>(std::tolower(u));
  }
  // A typed minus sign (U+2212) reads as '-'.
  for (size_t p; (p = s.find("\xE2\x88\x92")) != std::string::npos;) s.replace(p, 3, "-");
  return s;
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
bool endsWith(const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }

// A leading number of s (with an optional fraction "a/b"); `rest` is what follows.
bool number(const std::string& s, double& v, std::string& rest, bool* fraction = nullptr) {
  const char* b = s.c_str();
  char* e = nullptr;
  v = std::strtod(b, &e);
  if (e == b) return false;
  if (*e == '/') {
    char* e2 = nullptr;
    const double den = std::strtod(e + 1, &e2);
    if (e2 != e + 1 && den != 0) {
      v /= den;
      e = e2;
      if (fraction) *fraction = true;
    }
  }
  rest = e;
  return std::isfinite(v);
}

// The note value modifiers after a fraction: dotted x1.5, triplet x2/3.
double noteModifier(std::string& rest, bool& ok) {
  ok = true;
  if (rest.empty()) return 1.0;
  for (const char* d : {"dotted", "dot", "d", "."})
    if (rest == d) return rest.clear(), 1.5;
  for (const char* t : {"triplet", "trip", "t"})
    if (rest == t) return rest.clear(), 2.0 / 3.0;
  ok = false;
  return 1.0;
}

// Note values and beats for a delay time, as ticks (1/192 beat); false when the text is none.
bool noteTicks(const std::string& s, double& ticks) {
  double v;
  std::string rest;
  bool frac = false;
  if (!number(s, v, rest, &frac)) return false;
  if (frac) {  // "1/4", "1/8d", "1/16 triplet": a fraction of a whole note (four beats)
    bool ok;
    const double m = noteModifier(rest, ok);
    if (!ok) return false;
    ticks = v * 4.0 * timeknob::kPpq * m;
    return true;
  }
  for (const char* u : {"bars", "bar"})
    if (rest == u) return ticks = v * 4.0 * timeknob::kPpq, true;
  for (const char* u : {"beats", "beat", "bt", "b"})
    if (rest == u) return ticks = v * timeknob::kPpq, true;
  for (const char* u : {"steps", "step"})
    if (rest == u) return ticks = v * timeknob::kPpq / 4.0, true;
  return false;
}

// Seconds from a time text with its unit ("250ms", "0.5s"); `bareMs`: a bare number is in ms.
bool seconds(const std::string& rest, double v, double& out, bool bareMs) {
  if (rest == "ms") return out = v * 0.001, true;
  if (rest == "s" || rest == "sec" || rest == "secs") return out = v, true;
  if (rest == "min") return out = v * 60.0, true;
  if (rest.empty()) return out = bareMs ? v * 0.001 : v, true;
  return false;
}

// A frequency in Hz from "2.5k", "2.5khz", "440hz", "440".
bool hertz(const std::string& rest, double v, double& out) {
  if (rest.empty() || rest == "hz") return out = v, true;
  if (rest == "k" || rest == "khz") return out = v * 1000.0, true;
  return false;
}

// Percent units a value can be typed in.
bool percent(const std::string& rest) { return rest == "%" || rest == "pct" || rest == "percent"; }

}  // namespace

bool parse(const ParamDesc& d, const std::string& text, double& out, const timeknob::Spec* time, double tempo) {
  std::string s = squeeze(text);
  if (s.empty()) return false;
  auto done = [&](double v) {
    if (!std::isfinite(v)) return false;
    if (d.kind == ParamKind::Choice || d.kind == ParamKind::Toggle) v = std::round(v);
    out = d.clamp(v);
    return true;
  };
  if (d.kind == ParamKind::Choice) {
    for (size_t i = 0; i < d.choices.size(); ++i)
      if (squeeze(d.choices[i]) == s) return done(static_cast<double>(i));
  }
  if (d.kind == ParamKind::Toggle) {
    if (s == "on" || s == "yes" || s == "true") return done(1);
    if (s == "off" || s == "no" || s == "false") return done(0);
  }
  const bool noteUnit = d.unit == "note" || d.unit.rfind("note-", 0) == 0;
  if (noteUnit && d.unit.size() > 5 && (s == "any" || s == "auto" || s == "off" || s == "none")) return done(-1);  // 0.7.8: the word below 0
  if (noteUnit) {  // "c4", "a#3", "db2"
    static const char* names[] = {"c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"};
    static const char* flats[] = {"c", "db", "d", "eb", "e", "f", "gb", "g", "ab", "a", "bb", "b"};
    for (int k = 11; k >= 0; --k)
      for (const char* nm : {names[k], flats[k]}) {
        const size_t len = std::strlen(nm);
        if (s.compare(0, len, nm) == 0 && s.size() > len) {
          char* end = nullptr;
          const long oct = std::strtol(s.c_str() + len, &end, 10);
          if (end && *end == 0) return done(static_cast<double>((oct + 1) * 12 + k));
        }
      }
  }
  if (d.unit == "rate" && (s == "off" || s == "max")) return done(d.max);  // J-104: the top of the travel
  if (d.unit == "hHz" && s == "off") return done(d.max);
  if (d.unit == "sep64" && s == "original") return done(0);
  if (d.unit == "pan") {
    if (s == "c" || s == "centre" || s == "center" || s == "centred" || s == "centered" || s == "mid") return done(0);
    // "50%left", "50l", "l50", "30r", "right30"
    double sign = 0;
    std::string body = s;
    for (const char* l : {"left", "l"})
      if (sign == 0 && endsWith(body, l)) sign = -1, body.erase(body.size() - std::strlen(l));
      else if (sign == 0 && startsWith(body, l) && body.size() > std::strlen(l) && (std::isdigit(static_cast<unsigned char>(body[std::strlen(l)])) || body[std::strlen(l)] == '.')) sign = -1, body.erase(0, std::strlen(l));
    for (const char* r : {"right", "r"})
      if (sign == 0 && endsWith(body, r)) sign = 1, body.erase(body.size() - std::strlen(r));
      else if (sign == 0 && startsWith(body, r) && body.size() > std::strlen(r) && (std::isdigit(static_cast<unsigned char>(body[std::strlen(r)])) || body[std::strlen(r)] == '.')) sign = 1, body.erase(0, std::strlen(r));
    double v;
    std::string rest;
    if (!number(body, v, rest)) return false;
    if (sign != 0) return (rest.empty() || percent(rest)) && done(sign * std::fabs(v) * 0.01);
    if (percent(rest) || std::fabs(v) > 1.0) return (rest.empty() || percent(rest)) && done(v * 0.01);  // "-50" or "-50%": percent
    return rest.empty() && done(v);
  }
  // A delay time knob takes note values and beats ("1/4", "1/8 dotted", "2 bars").
  if (time) {
    double ticks;
    if (noteTicks(s, ticks)) {
      switch (time->unit) {
        case timeknob::Unit::Ticks: return done(ticks);
        case timeknob::Unit::Steps: return done(ticks * 4.0 / timeknob::kPpq);
        case timeknob::Unit::Ms: return tempo > 0 && done(ticks / timeknob::kPpq * 60000.0 / tempo);
        case timeknob::Unit::Seconds: return tempo > 0 && done(ticks / timeknob::kPpq * 60.0 / tempo);
        case timeknob::Unit::Beats: return done(ticks / timeknob::kPpq);                                  // K-240
        case timeknob::Unit::Hz: return tempo > 0 && ticks > 0 && done(tempo / 60.0 * timeknob::kPpq / ticks);  // one cycle per note
      }
    }
  }
  if (s.size() > 1 && s[0] == 'x' && (std::isdigit(static_cast<unsigned char>(s[1])) || s[1] == '.' || s[1] == '-')) s = s.substr(1) + "x";  // "x1.5"
  double v;
  std::string rest;
  bool frac = false;
  if (!number(s, v, rest, &frac)) return false;
  if (rest == "x" && d.unit != "times" && d.unit != "%") rest.clear();
  // A percentage on a parameter that is not in percent: that much of the travel.
  if (percent(rest) && d.unit != "%" && d.unit != "pct" && d.unit != "k128" && d.unit != "k2048" && d.unit != "sep64") {
    if (d.unit.empty() && d.min == 0 && d.max == 1) return done(v * 0.01);  // a bare 0..1 amount
    return done(d.fromNormalized(std::clamp(v * 0.01, 0.0, 1.0)));
  }
  const std::string& u = d.unit;
  if (u == "Hz" || u == "rate") {
    double hz;
    return hertz(rest, v, hz) && done(hz);
  }
  if (u == "hHz") {  // stored in 100 Hz steps; typed as a frequency ("2.5k") or bare in kHz
    double hz;
    if (rest.empty()) return done(v * 10.0);
    return hertz(rest, v, hz) && done(hz / 100.0);
  }
  if (u == "s") {
    double sec;
    return seconds(rest, v, sec, !frac && v > 20) && done(sec);  // bare numbers above 20 read as ms
  }
  if (u == "ms") {
    double sec;
    return seconds(rest, v, sec, true) && done(sec * 1000.0);
  }
  if (u == "q384") {  // 1/384 s steps, typed as time
    double sec;
    return seconds(rest, v, sec, true) && done(sec * 384.0);
  }
  if (u == "ds") {  // 1/10 s steps, typed as time (bare: seconds)
    double sec;
    return seconds(rest, v, sec, false) && done(sec * 10.0);
  }
  if (u == "%") {
    if (rest.empty() || percent(rest)) return done(v * 0.01);
    if (rest == "x") return done(v);  // "0.5x": the raw amount
    return false;
  }
  if (u == "pct") return (rest.empty() || percent(rest)) && done(v);
  if (u == "k128" || u == "k2048") return (rest.empty() || percent(rest)) && done(v * (u == "k128" ? 128.0 : 2048.0) / 100.0);
  if (u == "sep64") {  // "50% wide" (negative) / "50% merged"
    std::string r = rest;
    double pc = v;
    if (endsWith(r, "wide")) pc = -std::fabs(v), r.erase(r.size() - 4);
    else if (endsWith(r, "merged")) pc = std::fabs(v), r.erase(r.size() - 6);
    if (!r.empty() && !percent(r)) return false;
    return done(pc * 64.0 / 100.0);
  }
  if (u == "dB") return (rest.empty() || rest == "db") && done(v);
  if (u == "st") return (rest.empty() || rest == "st" || rest == "semi" || rest == "semitones") && done(v);
  if (u == "ct") return (rest.empty() || rest == "ct" || rest == "c" || rest == "cents" || rest == "cent") && done(v);
  if (u == "bits") return (rest.empty() || rest == "bits" || rest == "bit") && done(v);
  if (u == "times") return (rest.empty() || rest == "x") && done(v);
  if (u == "x") {  // a ratio "4:1" or "4"
    if (rest.empty()) return done(v);
    if (rest[0] == ':') {
      double den;
      std::string r2;
      return number(rest.substr(1), den, r2) && r2.empty() && den != 0 && done(v / den);
    }
    return false;
  }
  if (u.empty() || noteUnit) return rest.empty() && done(v);
  // Another unit: the number, with that unit or none.
  return (rest.empty() || rest == squeeze(u)) && done(v);
}

std::string initial(const ParamDesc& d, double value, const timeknob::Spec* time, double tempo) {
  if (time) {  // the note value or time alone, without the milliseconds in brackets
    const std::string r = timeknob::readout(*time, value, tempo, false);
    return r.substr(0, r.find(" ("));
  }
  if (d.unit == "BPM") {  // K-409: keep up to 3 decimal places when typing tempo
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", value);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
  }
  return d.format(value);
}

}  // namespace gl::paramtext
