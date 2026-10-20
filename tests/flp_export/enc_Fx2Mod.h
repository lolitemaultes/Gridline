// Gridline — FLP export tests: Fx2Mod plugin encoders (FlpEncFx2Mod.cpp).
#pragma once
#include <functional>
#include "enc_common.h"
#include "packs/modulation/modulation_dsp.h"
#include "packs/utility/utility.h"

namespace enctest {

// The encoder's defaults with some bytes changed, decoded: a non-default
// Gridline state of `flName` (false when it does not decode).
inline bool synthState(const std::string& flName, const std::string& glType, const std::function<void(Bytes&)>& edit, gl::PluginState& out) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  if (!e || !e->defaults) return false;
  Bytes b = e->defaults();
  edit(b);
  gl::flp::PluginMapping m;
  if (!enc::decodeState(flName, b, m, ctx()) || m.state.type != glType) return false;
  out = m.state;
  return true;
}

// Defaults decode, a changed state round-trips natively, and FL's shipped
// states encode exactly (`nativeWant` < 0: every decoded one).
inline void encCase(const char* label, const std::string& flName, const std::string& glType, const std::function<void(Bytes&)>& edit,
                    int nativeWant = -1, bool patch = true) {
  std::string l = label;
  expect(defaultsDecode(flName, glType), (l + ": defaults decode").c_str());
  gl::PluginState g;
  expect(synthState(flName, glType, edit, g) && nativeRoundTrip(flName, g), (l + ": a changed state encodes back exactly").c_str());
  if (flInstalled()) {
    Stats s = corpusRoundTrip(flName);
    const int want = nativeWant < 0 ? s.decoded : nativeWant;
    expect(s.decoded > 0 && s.nativeExact >= want, (l + ": shipped states encode exactly").c_str());
    if (patch) expect(s.patchExact == s.decoded, (l + ": unchanged shipped states are written verbatim").c_str());
  }
}

inline void testEncFx2Mod() {
  using gl::flp::putI32;
  using gl::flp::putU8;
  encCase("Spreader", "Spreader", "gl.spread", [](Bytes& b) {
    putI32(b, 4, 7300);     // spread
    putI32(b, 8, -2500);    // separation
    putI32(b, 12, 400000);  // low split
    putI32(b, 16, 1);       // mono low band
    putU8(b, 29, 0);        // post monitor off
  });
  encCase("X-Y Controller", "Fruity X-Y Controller", "gl.xyctl", [](Bytes& b) {
    putI32(b, 4, 10000);
    putI32(b, 16, -64);
    putI32(b, 20, 50000);
    putI32(b, 36, 1300);
  });
  encCase("X-Y-Z Controller", "Fruity X-Y-Z Controller", "gl.xyzctl", [](Bytes& b) {
    putI32(b, 4, 1000);
    putI32(b, 12, 60000);
    putI32(b, 40, 1024);
  });
  encCase("Formula Controller", "Fruity Formula Controller", "gl.formulactl", [](Bytes& b) {
    putI32(b, 8, 40000);
    // formula "a*b": rebuilt after the three knobs
    b.resize(16);
    const std::string f = "a*b";
    b.push_back(static_cast<uint8_t>(f.size()));
    b.insert(b.end(), f.begin(), f.end());
    b.push_back(0);
  });
  encCase("Send", "Fruity Send", "gl.send", [](Bytes& b) {
    putI32(b, 0, 40);
    putI32(b, 4, 200);
    putI32(b, 12, 3);  // target: fourth insert
  });
  encCase("NoteBook", "Fruity NoteBook", "gl.notes", [](Bytes& b) { putI32(b, 8, 1); });
  {
    // NoteBook text: two pages of plain text with a brace and a tab.
    gl::PluginState g = gl::defaultPluginState("gl.notes");
    std::vector<std::string> pages(gl::packs::utility::kNotePages);
    pages[0] = "Verse {A}\n\tchorus twice";
    pages[2] = "Mix notes: bass -3 dB";
    g.data = gl::packs::utility::encodeNotes(pages);
    g.params[gl::packs::utility::NtPage] = 3;
    expect(nativeRoundTrip("Fruity NoteBook", g), "NoteBook: pages of text encode back exactly");
  }
  {
    expect(defaultsDecode("Fruity HTML NoteBook", "gl.notes"), "HTML NoteBook: defaults decode");
    gl::PluginState g = gl::defaultPluginState("gl.notes");
    std::vector<std::string> pages(gl::packs::utility::kNotePages);
    pages[0] = "https://example.org/notes";
    g.data = gl::packs::utility::encodeNotes(pages);
    expect(nativeRoundTrip("Fruity HTML NoteBook", g), "HTML NoteBook: the address encodes back exactly");
    if (flInstalled()) {
      Stats s = corpusRoundTrip("Fruity HTML NoteBook");
      expect(s.decoded > 0 && s.nativeExact == s.decoded, "HTML NoteBook: shipped states encode exactly");
    }
  }
  encCase("Transporter", "Transporter", "gl.hitloop", [](Bytes& b) { putI32(b, 4, 6400), putI32(b, 4 + 4 * 3, 2500); });  // J-45: Hit Looper
  // dB Meter: no state at all.
  {
    gl::flp::PluginMapping m;
    expect(enc::decodeState("Fruity dB Meter", {}, m, ctx()) && m.state.type == "gl.meter", "dB Meter: decodes without a state");
    enc::Result r = enc::encode(m.state, "", nullptr, ctx());
    expect(r.flName == "Fruity dB Meter" && r.quality == enc::Result::Exact && !r.hasState && r.mismatched.empty(),
           "dB Meter: a native meter is written without a state, nothing lost");
  }
  // Edison: recording settings on an empty editor; shipped states (one
  // 94 KB state the decoder does not recognise keeps only 4 KB: not exact).
  encCase("Edison", "Edison", "gl.edison", [](Bytes& b) {
    const size_t at = b.size() - 10 * 4;
    putI32(b, at, 3);       // record on play
    putI32(b, at + 8, 3);   // maximum time choice
    putI32(b, at + 16, 1);  // follow host
  }, 11);
  // Patcher: an effect chain with a parallel branch, and an instrument.
  {
    namespace md = gl::packs::modulation;
    expect(defaultsDecode("Patcher", "gl.patcherfx") && defaultsDecode("Patcher", "gl.patcher"), "Patcher: defaults decode (both versions)");
    md::PatchGraph fx;
    md::PatchNode a, b2, c;
    gl::flp::PluginMapping m;
    gl::flp::ByteWriter w;
    w.i32(-40), w.i32(300);
    enc::decodeState("Fruity Balance", w.b, m, ctx());
    a.plugin = m.state;
    gl::PluginState sp;
    synthState("Spreader", "gl.spread", [](Bytes& x) { putI32(x, 4, 9000); }, sp);
    b2.plugin = sp;
    b2.mix = 0.5f;
    c.plugin = sp;
    c.parallel = true;
    fx.nodes = {a, b2, c};
    gl::PluginState g = gl::defaultPluginState("gl.patcherfx");
    g.data = md::serializePatchGraph(fx);
    // Gridline's own graphs omit empty "data" fields: compare through a decode.
    enc::Result r = enc::encode(g, "", nullptr, ctx());
    gl::flp::PluginMapping back;
    const bool ok = r.flName == "Patcher" && enc::decodeState("Patcher", r.state, back, ctx()) && back.state.type == "gl.patcherfx";
    md::PatchGraph got = ok ? md::parsePatchGraph(back.state.data) : md::PatchGraph{};
    bool same = got.nodes.size() == 3;
    for (size_t k = 0; same && k < 3; ++k)
      same = got.nodes[k].plugin == fx.nodes[k].plugin && got.nodes[k].parallel == fx.nodes[k].parallel && got.nodes[k].mix == fx.nodes[k].mix;
    if (std::getenv("GL_DEBUG_PATCH")) {
      std::printf("# patch nodes %zu ok %d\n", got.nodes.size(), ok ? 1 : 0);
      for (size_t k = 0; k < got.nodes.size() && k < 3; ++k)
        std::printf("#  %s par %d mix %g same %d\n", got.nodes[k].plugin.type.c_str(), got.nodes[k].parallel ? 1 : 0, static_cast<double>(got.nodes[k].mix),
                    got.nodes[k].plugin == fx.nodes[k].plugin ? 1 : 0);
    }
    expect(same, "Patcher: an effect chain with a parallel node encodes back (plugins, order, parallel, mix)");
    md::PatchGraph inst;
    md::PatchNode s;
    s.plugin = sp;
    inst.nodes = {s};
    gl::PluginState gi = gl::defaultPluginState("gl.patcher");
    gi.data = md::serializePatchGraph(inst);
    r = enc::encode(gi, "", nullptr, ctx());
    expect(r.flName == "Patcher" && r.quality == enc::Result::Exact, "Patcher: an instrument patch encodes back exactly");
    if (flInstalled()) {
      // Inner plugins go through their own encoders: only patches whose every
      // plugin encodes exactly can come back exact.
      Stats st = corpusRoundTrip("Patcher");
      int innerExact = 0;  // patches whose every plugin encodes exactly on its own
      for (const Bytes& pb : corpusStates("Patcher")) {
        gl::flp::PluginMapping pm;
        if (!enc::decodeState("Patcher", pb, pm, ctx())) continue;
        bool all = true;
        for (const auto& n : md::parsePatchGraph(pm.state.data).nodes)
          all = all && enc::encode(n.plugin, "", nullptr, ctx()).quality == enc::Result::Exact;
        innerExact += all;
        if (all && std::getenv("GL_DEBUG_PATCH")) {
          enc::Result rr = enc::encode(pm.state, "", nullptr, ctx());
          if (rr.quality != enc::Result::Exact) {
            gl::flp::PluginMapping back2;
            enc::decodeState("Patcher", rr.state, back2, ctx());
            size_t d = 0;
            while (d < pm.state.data.size() && d < back2.state.data.size() && pm.state.data[d] == back2.state.data[d]) ++d;
            std::printf("# patch approx: mism %zu data@%zu [%s] vs [%s]\n", rr.mismatched.size(), d, pm.state.data.substr(d > 40 ? d - 40 : 0, 120).c_str(),
                        back2.state.data.substr(d > 40 ? d - 40 : 0, 120).c_str());
          }
        }
      }
      expect(st.decoded > 0 && st.nativeExact >= innerExact, "Patcher: every shipped patch whose plugins all encode exactly encodes exactly");
      expect(st.patchExact == st.decoded, "Patcher: unchanged shipped patches are written verbatim");
    }
  }
}

}  // namespace enctest
