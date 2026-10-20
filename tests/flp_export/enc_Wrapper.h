// Gridline — FLP export tests: Wrapper plugin encoders (FlpEncWrapper.cpp).
#pragma once
#include "enc_common.h"
#include "core/flp/FlpWrapper.h"
#include "host/HostedPlugins.h"

namespace enctest {

// A hosted placeholder (plugin not installed here) written natively through
// the wrapper decodes back to the same type and saved state.
inline bool hostedRoundTrip(const std::string& type, const std::string& data, gl::flp::WrapperInfo* info = nullptr) {
  gl::PluginState g;
  g.type = type;
  g.data = data;
  enc::Result r = enc::encode(g, "", nullptr, ctx());
  if (r.flName != "Fruity Wrapper" || r.quality != enc::Result::Exact) return false;
  if (info && !gl::flp::parseWrapper(r.state.data(), r.state.size(), *info)) return false;
  return true;
}

inline void testEncWrapper() {
  namespace host = gl::host;
  const std::string floats = std::string("\xFF\xFF\xFF\xFF", 4) + std::string("\x00\x00\x40\x3F\x00\x00\x80\x3F", 8);
  gl::flp::WrapperInfo w;
  expect(hostedRoundTrip("vst2:47544332", "vst2c1:" + host::base64Encode("GTC2 chunk bytes"), &w) && w.format == gl::flp::WrapperInfo::Vst2 &&
             w.vst2Id == 0x47544332,
         "Wrapper: VST2 chunk state + unique id round-trip");
  expect(hostedRoundTrip("vst2:0BADCAFE", "vst2p1:" + host::base64Encode(floats)), "Wrapper: VST2 parameter list round-trips");
  expect(hostedRoundTrip("vst3:0123456789ABCDEF0011223344556677", host::encodeState("component", "controller"), &w) &&
             w.vst3Uid == "0123456789ABCDEF0011223344556677",
         "Wrapper: VST3 class id + component / controller state round-trip");
  expect(hostedRoundTrip("au:aufx/dely/appl", "au1:" + host::base64Encode("<plist/>"), &w) && w.auKey == "aufx/dely/appl",
         "Wrapper: AU component + class info round-trip");
  expect(hostedRoundTrip("au:aumu/0x00000001/Abcd", "au1:" + host::base64Encode("x")), "Wrapper: AU codes that are not text round-trip");
  expect(hostedRoundTrip("vst3:00000000000000000000000000000001", ""), "Wrapper: a plugin without a saved state round-trips");
  {
    gl::PluginState g;
    g.type = "clap:com.example.synth";
    g.data = "clap1:" + host::base64Encode("x");
    expect(enc::encode(g, "", nullptr, ctx()).flName != "Fruity Wrapper", "Wrapper: CLAP plugins are not written as a wrapper");
  }
  // An imported placeholder keeps its name / vendor when its state changes.
  {
    gl::PluginState g;
    g.type = "vst2:1A2B3C4D";
    g.data = "vst2c1:" + host::base64Encode("old");
    enc::Result r = enc::encode(g, "", nullptr, ctx());
    Bytes origin = r.state;
    // add identity text as FL writes it
    gl::flp::ByteWriter x;
    for (auto [id, s] : {std::pair<uint32_t, std::string>{54, "Some Synth"}, {56, "Some Vendor"}}) {
      x.u32(id);
      x.u64(s.size());
      x.bytes(s.data(), s.size());
    }
    origin.insert(origin.end(), x.b.begin(), x.b.end());
    g.data = "vst2c1:" + host::base64Encode("new state");
    r = enc::encode(g, "Fruity Wrapper", &origin, ctx());
    gl::flp::WrapperInfo wi;
    expect(r.quality == enc::Result::Exact && gl::flp::parseWrapper(r.state.data(), r.state.size(), wi) && wi.name == "Some Synth" &&
               wi.vendor == "Some Vendor" && wi.data == g.data,
           "Wrapper: a changed state keeps the origin's plugin name and vendor");
  }
  if (flInstalled()) {
    // FL's shipped wrapper states are FL's own effects loaded as VST plugins.
    Stats s = corpusRoundTrip("Fruity Wrapper");
    // Natively (no origin) the FL plugin to wrap is picked by type: the two
    // Reeverb (v1) render-test states pick Reeverb 2, whose laws cannot hit
    // their high cut / damping values. With their origin they are exact.
    expect(s.decoded > 0 && s.nativeExact >= s.decoded - 2, "Wrapper: shipped FL-plugin wrapper states encode exactly");
    expect(s.patchExact == s.decoded, "Wrapper: unchanged shipped states are written verbatim");
  }
}

}  // namespace enctest
