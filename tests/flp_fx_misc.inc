// Effect decoder tests for FlpFxMisc.cpp (included by test_flp_mixer.cpp).
// All state blobs are hand-made here (layouts described in FlpFxMisc.cpp).
#include "../src/core/PluginParams.h"
#include "../src/engine/Dsp.h"
#include "../src/engine/Effects.h"
#include "../src/engine/TimeGateSlots.h"
#include "../src/packs/creative/creative.h"
#include <array>
#include "../src/packs/dynamics/dynamics.h"
#include "../src/packs/modulation/modulation.h"
#include "../src/packs/spectral/spectral.h"
#include "../src/packs/utility/utility.h"

namespace misctest {
// One envelope record (64-bit points): kind, format 3, points (dx, y), tail.
void envRec(Blob& b, int kind, const std::vector<std::pair<double, double>>& pts, int flags = 2) {
  b.i32(kind).i32(3).i32(static_cast<int32_t>(pts.size()));
  for (const auto& p : pts) {
    uint64_t u;
    std::memcpy(&u, &p.first, 8);
    b.i32(static_cast<int32_t>(u & 0xFFFFFFFFu)).i32(static_cast<int32_t>(u >> 32));
    std::memcpy(&u, &p.second, 8);
    b.i32(static_cast<int32_t>(u & 0xFFFFFFFFu)).i32(static_cast<int32_t>(u >> 32));
    b.f32(0).i32(flags);
  }
  b.i32(0).i32(-1).i32(-1).i32(-1).i32(-1);
}
void utf16(Blob& b, const std::string& s) {
  for (char ch : s) b.u8(static_cast<uint8_t>(ch)).u8(0);
}
}  // namespace misctest

static void testFxMisc() {
  using namespace misctest;
  namespace pp = gl::pp;
  flp::PluginMapping m;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> keep;  // blobs for the truncation test

  // NoteBook 2: version, current page, {page, length (u8; 0xFF + u32), UTF-16}, -1, flag.
  {
    Blob b;
    b.i32(0).i32(2).i32(1).u8(8);
    utf16(b, "Hi\rthere");
    b.i32(2).u8(3);
    utf16(b, "two");
    // J-39: a 200-character page is one length byte (0xC8), 300 is 0xFF + u32
    b.i32(3).u8(200);
    utf16(b, std::string(200, 'a'));
    b.i32(4).u8(0xFF).i32(300);
    utf16(b, std::string(300, 'b'));
    b.i32(-1).u8(1);
    expect(decodeFx("Fruity NoteBook 2", b.b, m) && m.glType == "gl.notes", "notebook: decodes");
    keep.push_back({"Fruity NoteBook 2", b.b});
    auto pages = gl::packs::utility::decodeNotes(m.state.data);
    expect(pages[0] == "Hi\nthere" && pages[1] == "two", "notebook: pages, CR line ends");
    expect(pages[2] == std::string(200, 'a') && pages[3] == std::string(300, 'b'), "notebook: one-byte and 0xFF + u32 lengths");
    expect(near(param(m, gl::packs::utility::NtPage), 2, 1e-6), "notebook: current page");
  }

  // Effector: "SSLF" chunk ending with 12 normalized floats.
  {
    Blob b;
    b.b = {'S', 'S', 'L', 'F'};
    b.i32(8 + 48).b.insert(b.b.end(), {'1', '0', 'X', 'F', 0, 0, 0, 0});
    for (float v : {7.0f / 11.0f, 0.0f, 0.5f, 0.25f, 0.75f, 0.0f, 0.0f, 1.0f, 0.5f, 0.0f, 0.5f, 0.5f}) b.f32(v);
    namespace dy = gl::packs::dynamics;
    expect(decodeFx("Effector", b.b, m) && m.glType == "gl.effector", "effector: decodes");
    keep.push_back({"Effector", b.b});
    // J-26: the reference engine keeps the twelve stored values.
    expect(param(m, dy::EfModel) == 1 && near(param(m, dy::EfRawKind), 7.0 / 11.0, 1e-6) && near(param(m, dy::EfRawMix), 0.5, 1e-6) &&
               near(param(m, dy::EfRawX), 0.25, 1e-6) && m.exact,
           "effector: reference engine, kind (panner), mix, X");
    expect(near(param(m, dy::EfRawModX), 1.0, 1e-6) && near(param(m, dy::EfRawModY), 0.5, 1e-6) && near(param(m, dy::EfRawIn), 0.5, 1e-6),
           "effector: modulation amounts and input stored as is");
    expect(m.param && m.param(3) == dy::EfRawX && m.value && near(m.value(2, 1.0), 1.0, 1e-9), "effector: automation mapping");
  }

  // Gross Beat: version 8 header (9 params), options, 36 time + 36 volume slots, MIDI byte.
  {
    Blob b;
    b.u8(3).u8(0).u8(8).u8(0);
    b.i32(1).i32(2).i32(256).i32(128).i32(1765).i32(5000).i32(-128).i32(1).i32(65536);
    b.i32(0).u8(1).u8(0).u8(1).u8(1).u8(2);  // page, DC on, compensation off, HQ, previews, click reduction high
    for (int s = 0; s < 72; ++s) {
      std::string name = s == 1 ? "Stop" : "";
      b.u8(static_cast<uint8_t>(name.size()));
      for (char ch : name) b.u8(static_cast<uint8_t>(ch));
      b.i32(s == 1 ? 3 : 5).i32(s == 1 ? 0 : 5).i32(s == 1 ? 1 : 0).u8(0).u8(s == 1 ? 1 : 0).u8(0);
      if (s == 1)
        envRec(b, 3, {{0, 1.0}, {2, 1.0}, {2, 0.5}}, 0);  // no delay for 2 beats, then 4 beats back
      else if (s == 36 + 2)
        envRec(b, 3, {{0, 1.0}, {1, 0.0}, {3, 0.0}}, 0);
      else
        envRec(b, 3, {{0, 1.0}, {4, 1.0}}, 2);
    }
    expect(decodeFx("Gross Beat", b.b, m) && m.glType == "gl.timegate", "gross beat: decodes");
    keep.push_back({"Gross Beat", b.b});
    expect(near(param(m, pp::TgTimeSlot), 2, 1e-6) && near(param(m, pp::TgVolSlot), 3, 1e-6), "gross beat: selected slots (0-based → Gridline 1..)");
    expect(near(param(m, pp::TgTimeMix), 1, 1e-6) && near(param(m, pp::TgVolMix), 0.5, 1e-6) && near(param(m, pp::TgTension), -0.5, 1e-6) &&
               near(param(m, pp::TgHold), 1, 1e-6),
           "gross beat: mixes, tension, hold");
    expect(near(param(m, pp::TgVolAttack), 0.002, 1e-4) && near(param(m, pp::TgVolRelease), 0.0428, 1e-3) && near(param(m, pp::TgScratch), 0, 1e-9),
           "gross beat: smoothing times (501^x - 1 ms) and scratch centre");
    expect(near(param(m, pp::TgDcRemove), 1, 1e-9) && near(param(m, pp::TgCompensate), 0, 1e-9) && near(param(m, pp::TgDeclick), 2, 1e-9) &&
               near(param(m, pp::TgMidiTrigger), 1, 1e-9),
           "gross beat: options (DC, compensation, click reduction, MIDI trigger)");
    auto bank = std::make_unique<gl::tg::Bank>();
    gl::tg::factory(*bank);
    gl::tg::parse(m.state.data.c_str(), *bank);
    const auto& ts = bank->time[1];
    expect(ts.env.n == 3 && near(ts.env.x[1], 2, 1e-6) && near(ts.env.y[1], 1, 1e-6) && near(ts.env.y[2], 0.5, 1e-6) && std::string(ts.name) == "Stop",
           "gross beat: time slot points (x in beats, y = 1 live)");
    expect(ts.trig == 3 && ts.pos == 0 && ts.force == 1 && ts.keyHeld && !ts.link && !ts.oneShot, "gross beat: per-slot options");
    const auto& vs = bank->vol[2];
    expect(vs.env.n == 3 && near(vs.eval(2.0), 0, 1e-6) && near(vs.eval(0.4), 0.6, 1e-3), "gross beat: volume slot");
    expect(bank->time[5].env.mode[1] == gl::menv::Hold, "gross beat: point modes kept (hold)");
    expect(m.value && near(m.value(0, 3.0 / 35.0), 4, 1e-9), "gross beat: slot automation value");
  }

  // Love Philter: version 6 state (J-42: v2..5 store an i32-counted RTF info text), zlib (stored blocks) around the full
  // sequential layout: globals, options, info text, 8 units x 8 targets.
  {
    Blob raw;
    for (int32_t v : {104, 128, 128, 4096, 8192, 2, 256, 512}) raw.i32(v);
    raw.i32(0).f32(0.9685f).i32(-1).i32(1).i32(2);
    raw.u8(1).u8(1).u8(0).u8(0).i32(0);
    // Info text: 7-bit varint character count + UTF-16LE, CR = line break (J-34: the layout of every shipped state).
    const std::string info = "Hello\rWorld";
    raw.u8(static_cast<uint8_t>(info.size()));
    for (char ch : info) raw.u8(static_cast<uint8_t>(ch)).u8(0);
    auto rec = [&](int kind, const std::vector<std::array<double, 3>>& pts, int ls = -1, int le = -1, int en = 0) {  // {dx, y, mode}
      raw.i32(kind).i32(3).i32(static_cast<int32_t>(pts.size()));
      for (const auto& p : pts) {
        uint64_t u;
        std::memcpy(&u, &p[0], 8);
        raw.i32(static_cast<int32_t>(u & 0xFFFFFFFFu)).i32(static_cast<int32_t>(u >> 32));
        std::memcpy(&u, &p[1], 8);
        raw.i32(static_cast<int32_t>(u & 0xFFFFFFFFu)).i32(static_cast<int32_t>(u >> 32));
        raw.f32(0).i32(static_cast<int32_t>(p[2]));
      }
      raw.i32(1).i32(en).i32(-1).i32(ls).i32(le);  // flags, enabled (J-42), sustain, loop
    };
    for (int u = 0; u < 8; ++u) {
      std::vector<int32_t> knobs = {128, 128, 128, 128, 0, 0, 128, 128, 0, 128, 128, 0};
      if (u == 0) knobs = {128, 200, 100, 128, 0, 0, 128, 128, 32, 64, 128, 128};
      if (u == 1) knobs[10] = 0;
      for (int32_t k : knobs) raw.i32(k);
      raw.i32(1).i32(u == 0 ? 2 : u == 1 ? 7 : 0);
      raw.u8(0).u8(u < 2 ? 1 : 0).u8(0);
      rec(3, {{0, 0, 0}, {1, 0.75, 0}});
      raw.u8(0);
      for (int t = 0; t < 8; ++t) {
        raw.i32(1).f32(0).f32(1).f32(0);
        for (int k = 0; k < 10; ++k) {
          if (u == 0 && t == 2 && k == 1)
            rec(1, {{0, 0.5, 0}, {1, 1.0, 0}, {1, 0.25, 2}}, 0, 2, -1);
          else
            rec(1, {{0, 1.0, 0}});
          raw.i32(128).i32(128).i32(0).i32(128);
        }
        rec(2, {{0, 1.0, 0}});
        for (int32_t v : {40000, 0, 0, 0, 0}) raw.i32(v);
        for (int q = 0; q < 3; ++q) {
          bool add = t == 0 || t == 2 || t == 3;
          if (u == 0 && t == 2 && q == 0)
            rec(3, {{0, 0, 0}, {1, 1, 0}});
          else
            rec(3, {{0, add ? 0.5 : 1.0, 0}, {1, add ? 0.5 : 1.0, 0}});
        }
      }
    }
    // zlib wrapper with stored blocks + Adler-32.
    Blob b;
    b.u8(6).u8(0).u8(0).u8(0);
    b.u8(0x78).u8(0x01);
    size_t pos = 0;
    while (pos < raw.b.size()) {
      size_t len = std::min<size_t>(65535, raw.b.size() - pos);
      bool last = pos + len == raw.b.size();
      b.u8(last ? 1 : 0).u8(static_cast<uint8_t>(len & 0xFF)).u8(static_cast<uint8_t>(len >> 8));
      b.u8(static_cast<uint8_t>(~len & 0xFF)).u8(static_cast<uint8_t>((~len >> 8) & 0xFF));
      b.b.insert(b.b.end(), raw.b.begin() + static_cast<long>(pos), raw.b.begin() + static_cast<long>(pos + len));
      pos += len;
    }
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t c : raw.b) s1 = (s1 + c) % 65521, s2 = (s2 + s1) % 65521;
    uint32_t ad = (s2 << 16) | s1;
    b.u8(static_cast<uint8_t>(ad >> 24)).u8(static_cast<uint8_t>(ad >> 16)).u8(static_cast<uint8_t>(ad >> 8)).u8(static_cast<uint8_t>(ad));
    namespace cr = gl::packs::creative;
    expect(decodeFx("Fruity Love Philter", b.b, m) && m.glType == "gl.octafilter", "love philter: decodes into Octa Filter");
    keep.push_back({"Fruity Love Philter", b.b});
    expect(near(param(m, cr::OfMaster), 104.0 / 128, 1e-6) && near(param(m, cr::OfPattern), 1, 1e-9) && near(param(m, cr::OfModX), 0.25, 1e-9) &&
               near(param(m, cr::OfHqEnvelopes), 1, 1e-9) && near(param(m, cr::OfOversample), 2, 1e-9) && near(param(m, cr::OfSmooth), 1, 1e-9) &&
               near(param(m, cr::OfCenter), 1, 1e-9),
           "love philter: globals and options");
    expect(near(param(m, cr::unitParam(0, cr::UType)), 2, 1e-9) && near(param(m, cr::unitParam(0, cr::UCut)), 200.0 / 256, 1e-6) &&
               near(param(m, cr::unitParam(0, cr::URes)), 100.0 / 256, 1e-6) && near(param(m, cr::unitParam(0, cr::UPan)), 0.25, 1e-6) &&
               near(param(m, cr::unitParam(0, cr::UOut)), 0.5, 1e-6) && near(param(m, cr::unitParam(0, cr::UNext)), 1, 1e-6),
           "love philter: unit 1 knobs, type");
    expect(near(param(m, cr::unitParam(1, cr::UEnabled)), 1, 1e-9) && near(param(m, cr::unitParam(1, cr::UType)), 7, 1e-9) &&
               near(param(m, cr::unitParam(1, cr::UIn)), 0, 1e-9) && near(param(m, cr::unitParam(2, cr::UEnabled)), 0, 1e-9),
           "love philter: all eight units (unit 2 series all-pass)");
    auto ob = std::make_unique<cr::OctaBank>();
    cr::parse(m.state.data.c_str(), *ob);
    const cr::OEnv* pat = ob->env(0, cr::TCut, 1);
    const cr::OEnv* mx = ob->env(0, cr::TCut, cr::KMapX);
    expect(pat && pat->n == 3 && pat->mode[2] == gl::menv::Hold && ob->loopS[0][cr::TCut][1] == 0 && ob->loopE[0][cr::TCut][1] == 2 &&
               !ob->env(0, cr::TCut, 0) && !ob->env(0, cr::TVol, 1),
           "love philter: drawn pattern kept (modes, loop), default patterns dropped");
    expect(mx && near(mx->eval(0.5), 0.5, 1e-6) && !ob->env(0, cr::TRes, cr::KMapX), "love philter: X map kept, flat maps dropped");
    expect(cr::infoText(m.state.data) == "Hello\nWorld", "love philter: info text (UTF-16, CR line breaks)");
    expect(m.param && m.param(9) == cr::unitParam(0, cr::UCut) && m.value && near(m.value(9, 0.5), 0.5, 1e-9) &&
               m.param(8 + 12 + 10) == cr::unitParam(1, cr::UIn),
           "love philter: automation (globals, unit knobs)");
  }

  // Vocodex (version 0x00080003): 13 curve records, 20 tags, 7 ints (hold
  // mode, order, unison - 1, band count, native rate, modulator / carrier
  // input), 8 flag bytes, 13 pitch-follow bytes, carrier tone name, held keys.
  {
    Blob b;
    b.u8(3).u8(0).u8(8).u8(0);
    for (int k = 0; k < 13; ++k) envRec(b, 3, k == 0 ? std::vector<std::pair<double, double>>{{0, 0.25}, {1, 0.75}}
                                                     : std::vector<std::pair<double, double>>{{0, 0.5}, {1, 0.5}});
    for (int32_t v : {1000, 150, 12800, 55000, 0, 65536, 0, 0, 1666, 1115, 1000, 3500, 0, 0, 349, 50, 2000, -600, 250, 128, 1, 3, 0, 31, 0, 1, -1})
      b.i32(v);
    for (int v : {0, 1, 1, 1, 1, 1, 0, 1}) b.u8(static_cast<uint8_t>(v));
    for (int k = 0; k < 13; ++k) b.u8(k == 3 || k == 4 ? 1 : 0);
    b.u8(3).u8('P').u8('W').u8('M');
    b.i32(2).i32(60).i32(67);
    namespace sp = gl::packs::spectral;
    expect(decodeFx("Vocodex", b.b, m) && m.glType == "gl.vocoder", "vocodex: decodes");
    keep.push_back({"Vocodex", b.b});
    expect(near(param(m, sp::VcWetLevel), 0, 1e-6) && near(param(m, sp::VcModLevel), 0, 1e-6) && near(param(m, sp::VcCarLevel), -60, 1e-6) &&
               near(param(m, sp::VxCarLp), sp::kVxOffHigh, 1e-3) && near(param(m, sp::VxNoiseHp), sp::kVxOffLow, 1e-3),
           "vocodex: wet, pass-through levels, filters off at their ends");
    expect(near(param(m, sp::VxBands), 31, 1e-6) && near(param(m, sp::VcFormant), -6, 1e-6) && near(param(m, sp::VcModSource), 1, 1e-6) &&
               param(m, sp::VxOrder) == 2 && param(m, sp::VxHoldMode) == 1 && param(m, sp::VxCarSource) == 0 &&
               near(param(m, sp::VxAttCurve), 2, 1e-9) && near(param(m, sp::VxFlatness), 0.5, 1e-6) && param(m, sp::VxTone) == 2,
           "vocodex: band count, order, hold mode, modulator pitch, inputs, curve, tone family");
    sp::VxData d;
    expect(sp::vxParse(m.state.data, d) && d.keys.size() == 2 && d.keys[1] == 67 && d.pf[3] && !d.pf[0] &&
               near(sp::vxEval(d.maps[0], 0.5), 0.5, 1e-9) && near(d.maps[0][0].y, 0.25, 1e-9) && !m.exact,
           "vocodex: curves, pitch-follow flags and held keys in the state data (synth carrier: approximate)");
  }

  // Control Surface: version, chunks {id, u64 size, payload}.
  {
    Blob b;
    b.i32(1);
    auto chunk = [&](int id, const std::vector<uint8_t>& p) {
      b.i32(id).i32(static_cast<int32_t>(p.size())).i32(0);
      b.b.insert(b.b.end(), p.begin(), p.end());
    };
    auto w = [](const std::string& s) {
      std::vector<uint8_t> v;
      for (char ch : s) v.push_back(static_cast<uint8_t>(ch)), v.push_back(0);
      return v;
    };
    chunk(2003, {1, 0, 0, 0});
    chunk(2100, std::vector<uint8_t>(32, 0));
    chunk(2103, w("Drive"));
    Blob val;
    val.f32(0.75f).i32(0).i32(0);
    chunk(2102, val.b);
    chunk(2105, w("control TQuickKnob\r\n  Caption = Drive\r\nend\r\n"));
    chunk(2100, {3, 0, 0, 0});
    chunk(2103, w("Go"));
    chunk(2105, w("control TQuickBtn\r\n  StayDown = True\r\nend\r\n"));
    chunk(2101, {});
    namespace md = gl::packs::modulation;
    expect(decodeFx("Control Surface", b.b, m) && m.glType == "gl.surface", "control surface: decodes");
    keep.push_back({"Control Surface", b.b});
    expect(m.state.data == "0 Drive\n" + std::to_string(md::SkToggle) + " Go\n" && near(param(m, 0), 0.75, 1e-6), "control surface: controls and values");
  }

  // Truncated / corrupted states never crash a decoder.
  {
    int runs = 0;
    for (const auto& [name, blob] : keep)
      for (size_t n = 0; n <= blob.size(); n += 1 + blob.size() / 97) {
        std::vector<uint8_t> cut(blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(n));
        decodeFx(name, cut, m);
        if (n > 8) cut[n / 2] ^= 0x5A;
        decodeFx(name, cut, m);
        ++runs;
      }
    expect(runs > 100 && keep.size() == 6, "misc decoders: truncated and corrupted states are handled");
  }

  // Wave Candy: display only.
  expect(decodeFx("Wave Candy", {9, 0, 0, 0}, m) && m.glType == "gl.scope" && !m.exact, "wave candy → scope (truncated state: defaults)");

  // Vocoder sidechain modulator (VcModSource, added for Vocodex): silent
  // sidechain → silence; noisy sidechain → the carrier comes through.
  {
    namespace sp = gl::packs::spectral;
    auto run = [](float scAmp) {
      auto fx = gl::createEffect("gl.vocoder");
      gl::PluginState st = gl::defaultPluginState("gl.vocoder");
      st.params[sp::VcModSource] = 1;
      gl::ProcessContext ctx;
      ctx.sampleRate = 48000;
      std::vector<float> L(256), R(256), sl(256), sr(256);
      gl::dsp::Noise nz;
      double e = 0;
      for (int blk = 0; blk < 100; ++blk) {
        for (size_t i = 0; i < 256; ++i) {
          double t = static_cast<double>(blk * 256 + static_cast<int>(i)) / 48000.0;
          L[i] = R[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265 * 220 * t));
          sl[i] = sr[i] = scAmp * nz.next();
        }
        gl::SideIo io;
        io.scL = sl.data(), io.scR = sr.data();
        fx->setSideIo(io);
        fx->process(st, ctx, L.data(), R.data(), 256);
        if (blk >= 50)
          for (size_t i = 0; i < 256; ++i) e += static_cast<double>(L[i]) * L[i];
      }
      return e;
    };
    double quiet = run(0.0f), loud = run(0.5f);
    expect(std::isfinite(loud) && quiet < 1e-6 && loud > 1e-3, "vocoder: sidechain modulator drives the carrier");
  }
}
