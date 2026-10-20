// Gridline — FLP export tests: FxMisc plugin encoders (FlpEncFxMisc.cpp).
#pragma once
#include <string>
#include <vector>
#include "enc_FxK3.h"
#include "packs/spectral/spectral.h"
#include "packs/utility/utility.h"

namespace enctest {

inline void testEncFxMisc() {
  using gl::flp::putF32;
  using gl::flp::putI32;
  using gl::flp::putU8;
  namespace ut = gl::packs::utility;
  namespace sp = gl::packs::spectral;

  // NoteBook 2: pages rewritten from the data (J-39: every shipped notebook,
  // page lengths read as u8 / 0xFF + u32).
  fxbPlugin("Fruity NoteBook 2", "gl.notes", [](Bytes& b) { putI32(b, 4, 3); });
  fxbGlEdit("Fruity NoteBook 2", "gl.notes", [](gl::PluginState& s) {
    std::vector<std::string> pages(ut::kNotePages);
    pages[0] = "Intro\nverse \xC3\xA9t\xC3\xA9";
    pages[4] = "Mix notes";
    s.data = ut::encodeNotes(pages);
    s.params[ut::NtPage] = 5;
  }, "edited pages encode back exactly");

  // Effector: 12 normalized floats at the end of the chunk.
  fxbPlugin("Effector", "gl.effector", [](Bytes& b) {
    const size_t at = b.size() - 48;
    putF32(b, at + 0, 3.f / 11.f);   // phaser
    putF32(b, at + 8, 0.75f);        // mix
    putF32(b, at + 20, 4.f / 9.f);   // mod rate
    putF32(b, at + 44, 0.25f);       // output
  });

  // Wave Candy: title (data) and the main block.
  fxbPlugin("Wave Candy", "gl.scope", [](Bytes& b) {
    putI32(b, 4, 1);  // hold
  });
  fxbGlEdit("Wave Candy", "gl.scope", [](gl::PluginState& s) {
    s.data = "GLSCOPE1\ntitle=Bus meter\n";
    s.params[ut::ScMode] = ut::SvSpectrum;
    s.params[ut::ScRange] = 120;
    s.params[ut::ScFade] = 1;
    s.params[ut::ScFadeDelay] = 800;
  }, "a titled spectrum view encodes back exactly");

  // Control Surface: only existing controls' values are writable (controls
  // cannot be created), so native exact holds for the empty surface only.
  expect(defaultsDecode("Control Surface", "gl.surface"), "Control Surface: defaults decode");
  if (flInstalled()) {
    Stats s = corpusRoundTrip("Control Surface");
    expect(s.decoded > 0 && s.patchExact == s.decoded, "Control Surface: unchanged states are written verbatim");
    int moved = 0, exact = 0;
    for (const Bytes& b : corpusStates("Control Surface", 40)) {
      gl::flp::PluginMapping m;
      if (!enc::decodeState("Control Surface", b, m, ctx()) || m.state.data.empty()) continue;
      // the first control whose value the panel stores
      const enc::Encoder* e = enc::findEncoder("Control Surface", "gl.surface");
      const std::vector<enc::Field> f = e->fields(b);
      if (f.empty()) continue;
      const size_t k = static_cast<size_t>(f.front().gl);
      m.state.params[k] = m.state.params[k] > 0.5f ? 0.25f : 0.75f;
      ++moved;
      enc::Result r = enc::encode(m.state, "Control Surface", &b, ctx());
      exact += r.quality == enc::Result::Exact;
    }
    expect(moved > 0 && exact == moved, "Control Surface: a moved control value on a shipped panel encodes exactly");
  }

  // Vocodex: tags, options, curves / keys (data) and the carrier tone.
  fxbPlugin("Vocodex", "gl.vocoder", [](Bytes& b) {
    const size_t tags = b.size() - (4 + 4) - 1 - 13 - 8 - 28 - 80;  // from the end of a fresh state
    putI32(b, tags + 4 * 0, 1500);   // wet
    putI32(b, tags + 4 * 11, 5000);  // release
    putI32(b, tags + 4 * 17, -300);  // formant
    putI32(b, tags + 80 + 12, 30);   // bands
    putU8(b, tags + 108, 1);         // L-R encoding
  });
  fxbGlEdit("Vocodex", "gl.vocoder", [](gl::PluginState& s) {
    sp::VxData d;
    sp::vxParse(s.data, d);
    d.maps[sp::VmGain] = {{0, 0.25, 0}, {0.5, 0.75, 0.3}, {0.5, 0.5, 0}};
    d.pf[sp::VmPan] = true;
    d.keys = {48, 55, 64};
    s.data = sp::vxSerialize(d);
    s.params[sp::VxTone] = 2;
  }, "edited curves, keys and tone encode back exactly");
}

}  // namespace enctest
