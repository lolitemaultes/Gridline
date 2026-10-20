// Gridline — FLP export tests: FxK3 plugin encoders (FlpEncFxK3.cpp).
#pragma once
#include <functional>
#include <string>
#include "enc_FxTime.h"
#include "core/PluginParams.h"
#include "engine/TimeGateSlots.h"
#include "packs/creative/creative.h"

namespace enctest {

// The decoded defaults with Gridline-side edits (parameters / data): encodes
// back exactly through the encoder's write path.
inline void fxbGlEdit(const char* flName, const char* glType, const std::function<void(gl::PluginState&)>& edit, const char* what) {
  const enc::Encoder* e = enc::findEncoder(flName, glType);
  gl::flp::PluginMapping m;
  const bool ok = e && e->defaults && enc::decodeState(flName, e->defaults(), m, ctx());
  if (ok) edit(m.state);
  expect(ok && nativeRoundTrip(flName, m.state), (std::string(flName) + ": " + what).c_str());
}

inline void testEncFxK3() {
  using gl::flp::putI32;
  using gl::flp::putU8;
  fxbPlugin("Fruity Fast Dist", "gl.fastdist", [](Bytes& b) {
    putI32(b, 0, 160);
    putI32(b, 4, 4);
    putI32(b, 8, 1);
    putI32(b, 12, 70);
  });
  fxbPlugin("Fruity Flangus", "gl.swirl", [](Bytes& b) {
    putI32(b, 0, 7);
    putI32(b, 4, 900);
    putI32(b, 20, 64);
    putI32(b, 28, -50);
  });
  fxbPlugin("Fruity PanOMatic", "gl.autopan", [](Bytes& b) {
    putI32(b, 0, 60);
    putI32(b, 8, 2);
    putI32(b, 12, 1);
    putI32(b, 16, 100);
    putI32(b, 20, 40000);
  });
  // Fruity Chorus: one shipped mixer preset is an old float state whose
  // values fall between the integer knob steps (not representable).
  fxbPlugin("Fruity Chorus", "gl.chorus", [](Bytes& b) {
    putI32(b, 4 + 4 * 0, 300);
    putI32(b, 4 + 4 * 3, 4000);
    putI32(b, 4 + 4 * 7, 700);
    putI32(b, 4 + 4 * 9, 60);
    putI32(b, 4 + 4 * 11, 1024);
  }, 1);
  fxbPlugin("Fruity WaveShaper", "gl.waveshaper", [](Bytes& b) {
    putI32(b, 4, 150);
    putU8(b, 16, 1);
    putI32(b, 18, 2);
  });
  fxbGlEdit("Fruity WaveShaper", "gl.waveshaper", [](gl::PluginState& s) {
    s.data = "WS1|0,0,0,0 0.25,0.6,0.4,0 0.5,0.7,0,2 1,1,-0.3,0";
    s.params[gl::pp::WsHq] = 1;
  }, "a drawn curve encodes back exactly");
  fxbPlugin("Gross Beat", "gl.timegate", [](Bytes& b) {
    putI32(b, 4, 5);       // time slot 6
    putI32(b, 8, 12);      // volume slot 13
    putI32(b, 12, 128);    // time mix
    putI32(b, 4 + 4 * 6, -100);
    putU8(b, 4 + 36 + 4, 1);  // remove DC
  });
  fxbGlEdit("Gross Beat", "gl.timegate", [](gl::PluginState& s) {
    gl::tg::Slot sl;
    sl.setName("Test gate");
    sl.env.add(0, 1.f);
    sl.env.add(1, 0.f, 0, gl::menv::Hold);
    sl.env.add(2.5, 0.5f, 0.4f, gl::menv::SingleCurve);
    sl.env.add(4, 1.f, 0, gl::menv::Hold);
    sl.trig = 3, sl.pos = 0, sl.force = 1, sl.oneShot = true;
    // Replaced in place (the decoder lists every slot in bank order).
    const size_t p = s.data.find("\nV5 "), e = p == std::string::npos ? p : s.data.find('\n', p + 1);
    if (e != std::string::npos) s.data.replace(p + 1, e - p - 1, gl::tg::serialize('V', 4, sl));
  }, "an edited slot encodes back exactly");
  // Love Philter: everything is in the compressed stream (written whole).
  {
    namespace cr = gl::packs::creative;
    expect(defaultsDecode("Fruity Love Philter", "gl.octafilter"), "Love Philter: defaults decode");
    fxbGlEdit("Fruity Love Philter", "gl.octafilter", [](gl::PluginState& s) {
      s.params[cr::OfMaster] = 100 / 128.f;
      s.params[cr::OfPattern] = 3;
      s.params[cr::unitParam(1, cr::UEnabled)] = 1;
      s.params[cr::unitParam(1, cr::UCut)] = 77 / 256.f;
      s.params[cr::unitParam(1, cr::UType)] = 4;
      s.params[cr::lfoParam(1, cr::TCut, cr::LSpeed)] = 20000 / 65536.f;
      cr::OEnv e;
      e.add(0, 0.2f), e.add(1, 0.9f, 0.5f, gl::menv::SingleCurve), e.add(2, 0.2f);
      e.prepare();
      std::string d = cr::replaceLine(s.data, cr::envKey(1, cr::TCut, 2), cr::envLine(1, cr::TCut, 2, e, 0, 2));
      s.data = cr::withInfo(d, "Line one\nLine two");
    }, "edited units, patterns and info encode back exactly");
    if (flInstalled()) {
      Stats st = corpusRoundTrip("Fruity Love Philter");
      expect(st.decoded > 0 && st.nativeExact == st.decoded, "Love Philter: shipped states encode exactly");
      expect(st.patchExact == st.decoded, "Love Philter: unchanged states are written verbatim");
      int moved = 0, exact = 0;  // a moved global on the shipped state (no fields: the write path)
      for (const Bytes& b : corpusStates("Fruity Love Philter", 20)) {
        gl::flp::PluginMapping m;
        if (!enc::decodeState("Fruity Love Philter", b, m, ctx())) continue;
        m.state.params[cr::OfMaster] = 90 / 128.f;
        ++moved;
        enc::Result r = enc::encode(m.state, "Fruity Love Philter", &b, ctx());
        exact += r.quality == enc::Result::Exact;
      }
      expect(moved > 0 && exact == moved, "Love Philter: a changed level on a shipped state encodes exactly");
    }
  }
}

}  // namespace enctest
