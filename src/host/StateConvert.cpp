// Gridline: moving a plugin's own state between its formats (K-78). See StateConvert.h.
#include "StateConvert.h"
#include <algorithm>
#include <cstring>
#include "../core/Plist.h"
#include "Backend.h"
#include "PluginIdentity.h"

namespace gl::host::xstate {

namespace {

uint32_t be32(const std::string& s, size_t at) {
  if (at + 4 > s.size()) return 0;
  return (static_cast<uint32_t>(static_cast<unsigned char>(s[at])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(s[at + 1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[at + 2])) << 8) | static_cast<uint32_t>(static_cast<unsigned char>(s[at + 3]));
}
void putBe32(std::string& s, uint32_t v) {
  s += static_cast<char>((v >> 24) & 0xFF);
  s += static_cast<char>((v >> 16) & 0xFF);
  s += static_cast<char>((v >> 8) & 0xFF);
  s += static_cast<char>(v & 0xFF);
}
constexpr uint32_t cc(const char (&s)[5]) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(s[0])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 8) | static_cast<uint32_t>(static_cast<unsigned char>(s[3]));
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

// JUCE's binary XML state ("VC2!", the size, then XML text).
bool isJuceXml(const std::string& s) { return s.size() >= 8 && std::memcmp(s.data(), "VC2!", 4) == 0; }

std::string xmlEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

std::string formatOf(const std::string& type) {
  const size_t c = type.find(':');
  return c == std::string::npos ? std::string() : type.substr(0, c);
}

}  // namespace

std::string fxBankChunk(uint32_t fxId, int32_t fxVersion, const std::string& chunk, bool vstwHeader) {
  std::string out;
  if (vstwHeader) {
    putBe32(out, cc("VstW"));
    putBe32(out, 8);  // header size
    putBe32(out, 1);  // version
    putBe32(out, 0);  // not bypassed
  }
  std::string body;
  putBe32(body, cc("FBCh"));
  putBe32(body, 2);  // bank format version
  putBe32(body, fxId);
  putBe32(body, static_cast<uint32_t>(fxVersion));
  putBe32(body, 1);  // programs
  body.append(128, '\0');  // current program (0) and reserved bytes
  putBe32(body, static_cast<uint32_t>(chunk.size()));
  body += chunk;
  putBe32(out, cc("CcnK"));
  putBe32(out, static_cast<uint32_t>(body.size()));
  out += body;
  return out;
}

std::string fxProgramChunk(uint32_t fxId, int32_t fxVersion, int numParams, const std::string& name, const std::string& chunk) {
  std::string body;
  putBe32(body, cc("FPCh"));
  putBe32(body, 1);  // program format version
  putBe32(body, fxId);
  putBe32(body, static_cast<uint32_t>(fxVersion));
  putBe32(body, static_cast<uint32_t>(std::max(0, numParams)));
  std::string n = name.substr(0, 27);
  n.resize(28, '\0');
  body += n;
  putBe32(body, static_cast<uint32_t>(chunk.size()));
  body += chunk;
  std::string out;
  putBe32(out, cc("CcnK"));
  putBe32(out, static_cast<uint32_t>(body.size()));
  return out + body;
}

bool fxChunkPayload(const std::string& bytes, std::string& chunk, uint32_t* fxId, int* fxCount) {
  size_t at = 0;
  if (be32(bytes, 0) == cc("VstW")) at = 8 + be32(bytes, 4);  // magic, size, then `size` bytes
  if (at + 28 > bytes.size() || be32(bytes, at) != cc("CcnK")) return false;
  const uint32_t magic = be32(bytes, at + 8);
  size_t sizeAt = 0;
  if (magic == cc("FBCh")) sizeAt = at + 28 + 128;
  else if (magic == cc("FPCh")) sizeAt = at + 28 + 28;
  else return false;
  if (sizeAt + 4 > bytes.size()) return false;
  const uint32_t n = be32(bytes, sizeAt);
  if (sizeAt + 4 + n > bytes.size()) return false;
  chunk = bytes.substr(sizeAt + 4, n);
  if (fxId) *fxId = be32(bytes, at + 16);
  if (fxCount) *fxCount = static_cast<int>(std::min<uint32_t>(be32(bytes, at + 24), 1u << 20));
  return true;
}

bool stripJucePrivateData(const std::string& bytes, std::string& state) {
  static const char kTag[] = "JUCEPrivateData";
  const size_t tl = sizeof kTag - 1;
  if (bytes.size() < tl + 8 || bytes.compare(bytes.size() - tl, tl, kTag) != 0) return false;
  uint64_t n = 0;
  for (int i = 7; i >= 0; --i) n = (n << 8) | static_cast<unsigned char>(bytes[bytes.size() - tl - 8 + static_cast<size_t>(i)]);  // little-endian
  if (n > bytes.size() - tl - 8) return false;
  state = bytes.substr(0, bytes.size() - tl - 8 - static_cast<size_t>(n));
  return true;
}

std::string auVstClassInfoXml(uint32_t type, uint32_t subtype, uint32_t manufacturer, const std::string& program, const std::string& pluginState) {
  auto num = [](uint32_t v) { return std::to_string(static_cast<int32_t>(v)); };
  std::string x =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n<dict>\n";
  x += "\t<key>ProgramNumber</key>\n\t<integer>0</integer>\n";
  if (!pluginState.empty()) x += "\t<key>jucePluginState</key>\n\t<data>" + base64Encode(pluginState) + "</data>\n";
  x += "\t<key>manufacturer</key>\n\t<integer>" + num(manufacturer) + "</integer>\n";
  x += "\t<key>name</key>\n\t<string>No name</string>\n";
  x += "\t<key>subtype</key>\n\t<integer>" + num(subtype) + "</integer>\n";
  x += "\t<key>type</key>\n\t<integer>" + num(type) + "</integer>\n";
  x += "\t<key>version</key>\n\t<integer>" + std::to_string(pluginState.empty() ? 1 : 0) + "</integer>\n";
  x += "\t<key>vstdata</key>\n\t<data>" + base64Encode(program) + "</data>\n";
  x += "</dict>\n</plist>\n";
  return x;
}

std::string auClassInfoXml(uint32_t type, uint32_t subtype, uint32_t manufacturer, const std::string& name, const std::string& pluginState) {
  auto num = [](uint32_t v) { return std::to_string(static_cast<int32_t>(v)); };
  std::string x =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n<dict>\n";
  x += "\t<key>jucePluginState</key>\n\t<data>" + base64Encode(pluginState) + "</data>\n";
  x += "\t<key>manufacturer</key>\n\t<integer>" + num(manufacturer) + "</integer>\n";
  x += "\t<key>name</key>\n\t<string>" + xmlEscape(name) + "</string>\n";
  x += "\t<key>subtype</key>\n\t<integer>" + num(subtype) + "</integer>\n";
  x += "\t<key>type</key>\n\t<integer>" + num(type) + "</integer>\n";
  x += "\t<key>version</key>\n\t<integer>0</integer>\n";
  x += "</dict>\n</plist>\n";
  return x;
}

bool unwrap(const std::string& data, Core& out) {
  out = Core{};
  std::string bytes;
  auto body = [&](size_t prefix) { return base64Decode(data.substr(prefix), bytes); };
  if (startsWith(data, "vst2c1:")) {
    if (!body(7) || bytes.empty()) return false;
    std::string inner;
    if (fxChunkPayload(bytes, inner, &out.vst2Id, &out.fxCount)) {
      out.bytes = inner;
      out.source = "VST2 chunk (bank)";
    } else {
      out.bytes = bytes;
      out.source = "VST2 chunk";
    }
  } else if (startsWith(data, "vst2p1:")) {
    if (!body(7) || bytes.size() < 4) return false;
    std::memcpy(&out.program, bytes.data(), 4);
    for (size_t i = 4; i + 4 <= bytes.size(); i += 4) {
      float v = 0;
      std::memcpy(&v, bytes.data() + i, 4);
      out.values.push_back(v);
    }
    out.paramList = true;
    out.source = "VST2 parameter values";
    out.ok = true;
    return true;
  } else if (startsWith(data, "vst3s1:")) {
    const size_t c = data.find(':', 7);
    if (!base64Decode(data.substr(7, c == std::string::npos ? std::string::npos : c - 7), bytes) || bytes.empty()) return false;
    std::string inner;
    if (fxChunkPayload(bytes, inner, &out.vst2Id, &out.fxCount)) {
      out.bytes = inner;
      out.source = "VST3 state (VST2-compatible bank)";
    } else if (stripJucePrivateData(bytes, inner)) {
      out.bytes = inner;
      out.framework = "juce";
      out.source = "VST3 state (JUCE)";
    } else {
      out.bytes = bytes;
      out.source = "VST3 component state";
    }
  } else if (startsWith(data, "au1:")) {
    if (!body(4) || bytes.empty()) return false;
    std::string inner, program;
    if (plist::topValue(bytes, "jucePluginState", inner) && !inner.empty()) {
      out.bytes = inner;
      out.framework = "juce";
      out.source = "AU class info (JUCE state)";
    } else if (plist::topValue(bytes, "vstdata", program) && fxChunkPayload(program, inner, &out.vst2Id, &out.fxCount) && !inner.empty()) {
      out.bytes = inner;  // a VST2-based unit: its VST2 program chunk
      out.source = "AU class info (VST2 program)";
    } else {
      return false;  // no state that travels
    }
  } else if (startsWith(data, "clap1:")) {
    if (!body(6) || bytes.empty()) return false;
    out.bytes = bytes;
    out.source = "CLAP state";
  } else {
    return false;
  }
  if (out.framework.empty() && isJuceXml(out.bytes)) out.framework = "juce";
  out.ok = !out.bytes.empty();
  return out.ok;
}

std::string rewrap(const Core& core, const std::string& target, const std::string& name, std::string* how, int targetParams) {
  auto say = [how](const char* s) {
    if (how) *how = s;
  };
  if (!core.ok) return {};
  const std::string f = formatOf(target), uid = target.size() > f.size() + 1 ? target.substr(f.size() + 1) : std::string();
  if (core.paramList) {
    if (f != "vst2") return {};
    std::string bytes(4, '\0');
    std::memcpy(&bytes[0], &core.program, 4);
    for (float v : core.values) bytes.append(reinterpret_cast<const char*>(&v), 4);
    say("VST2 parameter values");
    return "vst2p1:" + base64Encode(bytes);
  }
  if (core.bytes.empty()) return {};
  if (f == "vst2") {
    say("VST2 chunk");
    return "vst2c1:" + base64Encode(core.bytes);
  }
  if (f == "vst3") {
    uint32_t v2 = 0;
    if ((core.framework == "juce" || core.vst2Id != 0 || startsWith(core.source, "VST2 chunk")) && ident::vst2IdOfVst3Uid(uid, v2)) {
      // A VST3 build that replaces its VST2 reads (and writes) the VST2 bank.
      say("VST3 state (VST2-compatible bank)");
      return "vst3s1:" + base64Encode(fxBankChunk(v2, 1, core.bytes, true)) + ":";
    }
    say("VST3 component state");
    return "vst3s1:" + base64Encode(core.bytes) + ":";
  }
  if (f == "au") {
    uint32_t t = 0, s = 0, m = 0;
    if (!ident::parseAuKey(uid, t, s, m)) return {};
    if (core.framework != "juce" && core.vst2Id != 0) {  // a VST2 program: a VST2-based unit reads it under "vstdata"
      // (the bytes under "jucePluginState" too, for a JUCE unit whose VST3 replaced its VST2)
      say("AU class info (VST2 program)");
      return "au1:" + base64Encode(auVstClassInfoXml(t, s, m, fxProgramChunk(core.vst2Id, 1, core.fxCount > 0 ? core.fxCount : targetParams, "No name", core.bytes), core.bytes));
    }
    say(core.framework == "juce" ? "AU class info (JUCE state)" : "AU class info (plugin state)");
    return "au1:" + base64Encode(auClassInfoXml(t, s, m, name, core.bytes));
  }
  if (f == "clap") {
    say("CLAP state");
    return "clap1:" + base64Encode(core.bytes);
  }
  return {};
}

}  // namespace gl::host::xstate
