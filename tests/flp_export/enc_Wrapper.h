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
  // J-171: an origin wrapper keeps every record and the framing of its state;
  // only the plugin's own state is replaced.
  {
    auto rec = [](gl::flp::ByteWriter& w, uint32_t id, const Bytes& d) { w.u32(id), w.u64(d.size()), w.bytes(d); };
    auto recs = [](const Bytes& b) {
      std::vector<std::pair<uint32_t, Bytes>> v;
      for (size_t p = 4; p + 12 <= b.size();) {
        const size_t n = gl::flp::getU32(b, p + 4);
        if (n > b.size() - p - 12) break;
        v.push_back({gl::flp::getU32(b, p), Bytes(b.begin() + static_cast<std::ptrdiff_t>(p + 12), b.begin() + static_cast<std::ptrdiff_t>(p + 12 + n))});
        p += 12 + n;
      }
      return v;
    };
    auto sameExcept53 = [&](const Bytes& a, const Bytes& b) {
      const auto x = recs(a), y = recs(b);
      if (x.size() != y.size() || gl::flp::getU32(a, 0) != gl::flp::getU32(b, 0)) return false;
      for (size_t i = 0; i < x.size(); ++i)
        if (x[i].first != y[i].first || (x[i].first != 53 && x[i].second != y[i].second)) return false;
      return true;
    };
    auto state53 = [&](const Bytes& b) {
      for (const auto& [id, d] : recs(b))
        if (id == 53) return d;
      return Bytes{};
    };
    const std::string plistA = "<plist>A</plist>", plistB = "<plist>state B, longer</plist>";
    // AU instrument: u32 word, size, u32 word, class info, trailer.
    gl::flp::ByteWriter st;
    st.u32(7), st.u32(static_cast<uint32_t>(plistA.size())), st.u32(9), st.bytes(plistA.data(), plistA.size()), st.u32(0xABCD);
    gl::flp::ByteWriter w;
    w.u32(12);
    rec(w, 1, Bytes(20, 0x11));
    rec(w, 30, Bytes(16, 0x22));
    rec(w, 50, Bytes{10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    rec(w, 51, Bytes(4, 0));
    rec(w, 52, Bytes{'d', 'c', 'b', 'A', 'u', 'm', 'u', 'a', 'x', 't', 's', 'T', 0, 0, 0, 0});  // Abcd / aumu / Tstx
    rec(w, 54, Bytes{'T', 'e', 's', 't'});
    rec(w, 53, st.b);
    const Bytes origin = w.b;
    gl::PluginState g;
    g.type = "au:aumu/Tstx/Abcd";
    g.data = "au1:" + host::base64Encode(plistA);
    enc::Result r = enc::encode(g, "Fruity Wrapper", &origin, ctx());
    expect(r.quality == enc::Result::Verbatim && r.state == origin, "Wrapper: an unchanged AU origin is written verbatim");
    g.data = "au1:" + host::base64Encode(plistB);
    r = enc::encode(g, "Fruity Wrapper", &origin, ctx());
    gl::flp::ByteWriter want;
    want.u32(7), want.u32(static_cast<uint32_t>(plistB.size())), want.u32(9), want.bytes(plistB.data(), plistB.size()), want.u32(0xABCD);
    expect(r.quality == enc::Result::Exact && sameExcept53(origin, r.state) && state53(r.state) == want.b,
           "Wrapper: a changed AU state keeps the origin's records and state framing");
    // VST2 chunk: reserved bytes and a trailer after the chunk.
    gl::flp::ByteWriter v;
    v.i32(-9), v.u8(5), v.i32(-2), v.u32(3), v.u32(0x01020304), v.u32(0x05060708), v.bytes("abc", 3), v.u32(0x77);
    gl::flp::ByteWriter w2;
    w2.u32(10);
    rec(w2, 1, Bytes(20, 0x33));
    rec(w2, 50, Bytes{4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    rec(w2, 51, Bytes{0x44, 0x33, 0x22, 0x11});
    rec(w2, 54, Bytes{'S', 'y', 'n'});
    rec(w2, 55, Bytes{'/', 'x'});
    rec(w2, 53, v.b);
    const Bytes o2 = w2.b;
    g.type = "vst2:11223344";
    g.data = "vst2c1:" + host::base64Encode("new chunk");
    r = enc::encode(g, "Fruity Wrapper", &o2, ctx());
    gl::flp::ByteWriter want2;
    want2.i32(-9), want2.u8(5), want2.i32(-2), want2.u32(9), want2.u32(0x01020304), want2.u32(0x05060708), want2.bytes("new chunk", 9), want2.u32(0x77);
    expect(r.quality == enc::Result::Exact && sameExcept53(o2, r.state) && state53(r.state) == want2.b,
           "Wrapper: a changed VST2 chunk keeps the origin's records and state framing");
    // Native instruments that are not installed here: the kind says instrument.
    auto kind = [&](const Bytes& b) {
      for (const auto& [id, d] : recs(b))
        if (id == 50 && d.size() >= 4) return gl::flp::getU32(d, 0);
      return 99u;
    };
    gl::PluginState au;
    au.type = "au:aumu/Tstx/Abcd";
    au.data = "au1:" + host::base64Encode(plistA);
    r = enc::encode(au, "", nullptr, ctx());
    expect(r.quality == enc::Result::Exact && kind(r.state) == 10, "Wrapper: an AU music device is written as an instrument");
    gl::PluginState v3;
    v3.type = "vst3:0123456789ABCDEF0011223344556677";
    v3.data = host::encodeState("component", "controller");
    enc::EncodeContext ch = ctx();
    ch.channel = true;
    expect(kind(enc::encode(v3, "", nullptr, ch).state) == 8 && kind(enc::encode(v3, "", nullptr, ctx()).state) == 7,
           "Wrapper: a VST3 plugin on a channel is written as an instrument, in a mixer slot as an effect");
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
