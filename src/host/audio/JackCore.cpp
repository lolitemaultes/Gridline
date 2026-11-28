// Gridline: platform-independent JACK / PipeWire core logic (K-357). See JackCore.h.
#include "JackCore.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <sstream>

namespace gl::jack {

namespace {

bool isAsciiCtrl(unsigned char c) { return c < 0x20 || c == 0x7f; }

// Clamp UTF-8 string cleanly without splitting a multibyte sequence.
std::string clampUtf8(const std::string& s, size_t maxLen) {
  if (s.size() <= maxLen) return s;
  size_t n = maxLen;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
  return s.substr(0, n);
}

// Natural comparison key for port names so playback_2 sorts before playback_10.
bool naturalPortLess(const std::string& a, const std::string& b) {
  size_t ia = 0, ib = 0;
  while (ia < a.size() && ib < b.size()) {
    const bool da = std::isdigit(static_cast<unsigned char>(a[ia])) != 0;
    const bool db = std::isdigit(static_cast<unsigned char>(b[ib])) != 0;
    if (da && db) {
      size_t ja = ia, jb = ib;
      while (ja < a.size() && std::isdigit(static_cast<unsigned char>(a[ja]))) ++ja;
      while (jb < b.size() && std::isdigit(static_cast<unsigned char>(b[jb]))) ++jb;
      // Compare integer value without overflow (strip leading zeros, compare lengths, then lex).
      size_t za = ia, zb = ib;
      while (za + 1 < ja && a[za] == '0') ++za;
      while (zb + 1 < jb && b[zb] == '0') ++zb;
      const size_t la = ja - za, lb = jb - zb;
      if (la != lb) return la < lb;
      const int cmp = a.compare(za, la, b, zb, lb);
      if (cmp != 0) return cmp < 0;
      ia = ja;
      ib = jb;
    } else {
      if (a[ia] != b[ib]) return static_cast<unsigned char>(a[ia]) < static_cast<unsigned char>(b[ib]);
      ++ia;
      ++ib;
    }
  }
  return a.size() < b.size();
}

}  // namespace

std::string sanitizeName(const std::string& raw, size_t maxLen, const std::string& fallback) {
  std::string out;
  out.reserve(std::min(raw.size(), maxLen));
  bool prevSpace = true;
  for (unsigned char c : raw) {
    if (isAsciiCtrl(c)) continue;
    if (c == ':' || c == '/' || c == '\\') {
      if (!prevSpace) {
        out.push_back(' ');
        prevSpace = true;
      }
      continue;
    }
    if (c == ' ') {
      if (!prevSpace) {
        out.push_back(' ');
        prevSpace = true;
      }
      continue;
    }
    out.push_back(static_cast<char>(c));
    prevSpace = false;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  out = clampUtf8(out, maxLen);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.empty()) return clampUtf8(fallback, maxLen);
  return out;
}

std::string sanitizeClientName(const std::string& raw) {
  return sanitizeName(raw, kMaxClientNameLen, "Gridline");
}

std::string formatTrackAlias(const std::string& basePort, const std::string& trackName) {
  const std::string cleanBase = sanitizeName(basePort, kMaxPortNameLen, "In 1");
  const std::string cleanTrack = sanitizeName(trackName, kMaxPortNameLen, "");
  if (cleanTrack.empty()) return cleanBase;
  const std::string prefix = cleanBase + " (";
  if (prefix.size() + 2 >= kMaxPortNameLen) return cleanBase;
  const size_t budget = kMaxPortNameLen - prefix.size() - 1;
  std::string trimmed = clampUtf8(cleanTrack, budget);
  while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
  if (trimmed.empty()) return cleanBase;
  return prefix + trimmed + ")";
}

PortPlan planPorts(int inputs, int extraPairs) {
  PortPlan plan;
  const int nIn = std::clamp(inputs, 0, kMaxInputs);
  const int nPairs = std::clamp(extraPairs, 0, kMaxExtraPairs);
  plan.ins.reserve(static_cast<size_t>(nIn));
  for (int i = 0; i < nIn; ++i) {
    const std::string name = "In " + std::to_string(i + 1);
    plan.ins.push_back({name, name, true, i});
  }
  plan.outs.reserve(static_cast<size_t>(2 + 2 * nPairs));
  plan.outs.push_back({"Out 1", "Main L", false, 0});
  plan.outs.push_back({"Out 2", "Main R", false, 1});
  for (int p = 0; p < nPairs; ++p) {
    const int chL = 2 + 2 * p;
    const int chR = 3 + 2 * p;
    plan.outs.push_back({"Out " + std::to_string(chL + 1), "Pair " + std::to_string(p + 1) + " L", false, chL});
    plan.outs.push_back({"Out " + std::to_string(chR + 1), "Pair " + std::to_string(p + 1) + " R", false, chR});
  }
  return plan;
}

std::vector<std::string> planInputAliases(const PortPlan& plan, const std::vector<TrackInputRef>& tracks) {
  std::vector<std::string> out;
  out.reserve(plan.ins.size());
  for (const PortSpec& p : plan.ins) {
    std::string trackName;
    for (const TrackInputRef& t : tracks) {
      if (t.input < 0 || t.name.empty()) continue;
      if (t.input == p.channel || (t.stereo && t.input + 1 == p.channel)) {
        trackName = t.name;
        break;
      }
    }
    out.push_back(formatTrackAlias(p.name, trackName));
  }
  return out;
}

std::vector<Link> parseLinks(const std::string& text) {
  std::vector<Link> out;
  std::istringstream in(text);
  std::string line;
  auto trim = [](std::string s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
  };
  while (std::getline(in, line)) {
    const size_t sep = line.find("->");
    if (sep == std::string::npos) continue;
    Link l{trim(line.substr(0, sep)), trim(line.substr(sep + 2))};
    if (l.own.empty() || l.peer.empty()) continue;
    if (std::find(out.begin(), out.end(), l) == out.end()) out.push_back(std::move(l));
  }
  return out;
}

std::string formatLinks(const std::vector<Link>& links) {
  std::string out;
  for (const Link& l : links) {
    if (l.own.empty() || l.peer.empty()) continue;
    if (!out.empty()) out += '\n';
    out += l.own + " -> " + l.peer;
  }
  return out;
}

std::vector<Link> linksToRestore(const std::vector<Link>& saved, const std::vector<std::string>& ownPorts,
                                 const std::vector<std::string>& existingPeers, const std::vector<Link>& current) {
  std::set<std::string> ownSet(ownPorts.begin(), ownPorts.end());
  std::set<std::string> peerSet(existingPeers.begin(), existingPeers.end());
  std::vector<Link> out;
  for (const Link& l : saved) {
    if (l.own.empty() || l.peer.empty()) continue;
    if (!ownSet.count(l.own) || !peerSet.count(l.peer)) continue;
    if (std::find(current.begin(), current.end(), l) != current.end()) continue;
    if (std::find(out.begin(), out.end(), l) != out.end()) continue;
    out.push_back(l);
  }
  return out;
}

std::vector<std::string> pickPhysical(const std::vector<std::string>& physicalPorts, int wantCount) {
  if (wantCount <= 0 || physicalPorts.empty()) return {};
  std::vector<std::string> sorted = physicalPorts;
  std::stable_sort(sorted.begin(), sorted.end(), naturalPortLess);
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  if (static_cast<int>(sorted.size()) > wantCount) sorted.resize(static_cast<size_t>(wantCount));
  return sorted;
}

int recordLatency(const std::vector<LatencyRange>& captureRanges, const std::vector<LatencyRange>& playbackRanges) {
  uint32_t cap = 0, play = 0;
  for (const auto& r : captureRanges) cap = std::max(cap, r.max);
  for (const auto& r : playbackRanges) play = std::max(play, r.max);
  const uint64_t total = static_cast<uint64_t>(cap) + static_cast<uint64_t>(play);
  return total > 1000000u ? 1000000 : static_cast<int>(total);
}

std::string pipewireLatencyEnv(int requestFrames, int sampleRate) {
  if (requestFrames <= 0) return {};
  const int sr = sampleRate > 0 ? sampleRate : 48000;
  return std::to_string(requestFrames) + "/" + std::to_string(sr);
}

std::vector<int> resolveExtraMixers(int extraPairs, const std::vector<asio::ExtraOut>& extras) {
  const int n = std::clamp(extraPairs, 0, kMaxExtraPairs);
  std::vector<int> out(static_cast<size_t>(n), -1);
  for (const auto& e : extras) {
    if (e.mixer > 0 && e.firstOut >= 0 && e.firstOut < n) out[static_cast<size_t>(e.firstOut)] = e.mixer;
  }
  return out;
}

std::string formatStatusLine(const StatusInfo& s) {
  char buf[256];
  const char* via = s.fake ? " (simulated server)" : s.viaPipeWire ? " via PipeWire" : "";
  std::snprintf(buf, sizeof buf, "Running on JACK%s as %s: %d Hz, %d-frame buffer, %d input%s, %d output%s.", via,
                s.clientName.empty() ? "Gridline" : s.clientName.c_str(), s.rate, s.buffer, s.inputs, s.inputs == 1 ? "" : "s",
                s.outputs, s.outputs == 1 ? "" : "s");
  return buf;
}

std::string formatLatencyLine(const StatusInfo& s) {
  char buf[128];
  std::snprintf(buf, sizeof buf, "Latency %d in / %d out, %d dropout%s.", s.inLatency, s.outLatency, s.xruns,
                s.xruns == 1 ? "" : "s");
  return buf;
}

std::string formatConnectedPeers(const std::vector<std::string>& peers) {
  std::vector<std::string> uniq;
  for (const auto& p : peers) {
    if (!p.empty() && std::find(uniq.begin(), uniq.end(), p) == uniq.end()) uniq.push_back(p);
  }
  std::string out;
  for (size_t i = 0; i < uniq.size(); ++i) {
    if (i > 0) out += ", ";
    out += uniq[i];
  }
  return out;
}

}  // namespace gl::jack
