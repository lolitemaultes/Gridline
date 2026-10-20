// Gridline — FLP import fixes found by the J-34 mapping audit: regression
// tests on synthetic projects (no FL content) and, when FL Studio is
// installed, on its shipped projects.
#pragma once
#include <filesystem>
#include <fstream>
#include "enc_common.h"
#include "core/Params.h"
#include "core/flp/FlpWriter.h"

namespace importfix {

// Imports an in-memory FLP through a temporary file.
inline bool importBytes(const gl::flp::Bytes& b, gl::Song& out, const char* tag) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path p = fs::temp_directory_path(ec) / (std::string("gl_import_fix_") + tag + ".flp");
  {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  }
  gl::flp::Report rep;
  std::string err;
  bool ok = gl::importFlp(p.string(), out, rep, &err, "/nonexistent");
  fs::remove(p, ec);
  return ok;
}

inline gl::flp::Bytes insertFlags(uint32_t flags) {
  gl::flp::ByteWriter w;
  w.f32(0);
  w.u32(flags);
  w.u32(0);
  return w.b;
}

}  // namespace importfix

inline void testImportFixes() {
  using namespace gl::flp;
  using gl::Song;
  // --- Synthetic project: FL 25 channel routing (104), dock / thread bits
  // (236), a level-0 route whose send level is automated (227).
  {
    EventWriter w;
    w.ascii(199, "25.2.0.5000");
    w.dword(156, 140000);
    // channel 0: sampler routed to insert 2 through 104 only (FL 24.2.99+)
    w.word(64, 0);
    w.byte(21, 0);
    w.text(203, "Kick");
    w.word(104, 2);
    // channel 1: plugin channel routed to insert 1 through 104
    w.word(64, 1);
    w.byte(21, 2);
    w.text(203, "Lead");
    w.word(104, 1);
    // channel 2: automation clip driving the send level 1 → 3 (0x1F40 + 3)
    w.word(64, 2);
    w.byte(21, 5);
    w.text(203, "Send");
    {
      ByteWriter t;
      t.u16(0), t.u16(2), t.u32(0), t.u32(((0x2000u + 1 * 64) << 16) | (0x1F40u + 3)), t.u32(8), t.u32(0x1D5);
      w.data(227, t.b);
    }
    // mixer: master, 1 (dock middle, routes to master + 3), 2 (dock right), 3 (dock left, no threads)
    const uint32_t on = 0x4 | 0x8;
    const uint32_t flags[4] = {on, on | 0x40, on | 0x80, on | 0x10};
    for (int i = 0; i < 4; ++i) {
      if (i == 1) w.text(204, "Bus");
      w.data(236, importfix::insertFlags(flags[i]));
      Bytes route(4, 0);
      if (i == 1) route[0] = 1, route[3] = 1;
      if (i == 2 || i == 3) route[0] = 1;
      if (i > 0) w.data(235, route);
      w.dword(147, 0xFFFFFFFFu);
    }
    {
      ByteWriter r;  // 225: route level 1 → 3 = 0 (FL ≤ 21 layout: kind 31, id 64 + 3)
      r.u32(0), r.u8(64 + 3), r.u8(31), r.u16((128 + 1) << 6), r.i32(0);
      r.u32(0), r.u8(192), r.u8(31), r.u16((128 + 0) << 6), r.i32(12800);  // master volume (sets the base)
      w.data(225, r.b);
    }
    Song s;
    const bool ok = importfix::importBytes(w.file(0, 3, 96), s, "synthetic");
    expect(ok && s.mixer.size() == 4 && s.channels.size() == 2, "import fix: synthetic project imports");
    if (ok && s.mixer.size() == 4 && s.channels.size() == 2) {
      expect(s.channels[0].mixerTrack == s.mixer[2].id && s.channels[1].mixerTrack == s.mixer[1].id,
             "import fix: FL 25 channels route through event 104 (all channel types)");
      expect(s.mixer[1].dock == 0 && s.mixer[2].dock == 1 && s.mixer[3].dock == -1 && s.mixer[0].dock == 0,
             "import fix: insert dock side from flags 0x40 / 0x80");
      expect(!s.mixer[3].threaded && s.mixer[1].threaded, "import fix: insert flag 0x10 = threaded processing off");
      const gl::Send* send = nullptr;
      for (const auto& sd : s.mixer[1].sends)
        if (sd.to == s.mixer[3].id) send = &sd;
      expect(send && !send->sidechain && send->level == 0.0f, "import fix: automated level-0 route is a send, not a sidechain");
      expect(s.automations.size() == 1 && s.automations[0].owner == s.mixer[1].id && s.automations[0].index == gl::kSendBase + 3,
             "import fix: send-level automation resolves to the send");
    }
  }
  // --- FL 25 mixer ids (0x7000 + insert × 64 + slot) pass 0x8000 for inserts ≥ 64:
  // an automated level-0 send 65 → 66 stays a send.
  {
    EventWriter w;
    w.ascii(199, "25.2.0.5000");
    w.dword(156, 140000);
    w.word(64, 0);
    w.byte(21, 5);
    w.text(203, "Send");
    {
      ByteWriter t;
      t.u16(0), t.u16(0), t.u32(0), t.u32(((0x7000u + 65 * 64) << 16) | (0x1F40u + 66)), t.u32(8), t.u32(0x1D5);
      w.data(227, t.b);
    }
    const int n = 68;  // master, 1..66, FL's "current" track
    w.word(103, n);
    for (int i = 0; i < n; ++i) {
      if (i == 65) w.text(204, "Vox");
      w.data(236, importfix::insertFlags(0x4C));
      Bytes route(static_cast<size_t>(n), 0);
      route[0] = 1;
      if (i == 65) route[66] = 1;
      if (i > 0) w.data(235, route);
      w.dword(147, 0xFFFFFFFFu);
    }
    {
      ByteWriter r;  // FL 25 layout: kind 32 route levels, master base 448
      r.u32(0), r.u8(192), r.u8(31), r.u16((448 + 0) << 6), r.i32(12800);
      r.u32(0), r.u8(66), r.u8(32), r.u16((448 + 65) << 6), r.i32(0);
      w.data(225, r.b);
    }
    Song s;
    const bool ok = importfix::importBytes(w.file(0, 1, 96), s, "fl25ids");
    const gl::Send* send = nullptr;
    if (ok && s.mixer.size() > 66)
      for (const auto& sd : s.mixer[65].sends)
        if (sd.to == s.mixer[66].id) send = &sd;
    expect(send && !send->sidechain && s.automations.size() == 1 && s.automations[0].owner == s.mixer[65].id &&
               s.automations[0].index == gl::kSendBase + 66,
           "import fix: FL 25 send automation on inserts ≥ 64 (target high word ≥ 0x8000)");
  }
  if (!enctest::flInstalled()) return;
  // --- Shipped projects.
  const std::string demos = gl::flp::defaultFactoryData() + "/Data/Demo projects/Demo songs/";
  {
    Song s;
    Report rep;
    std::string err;
    if (gl::importFlp(demos + "Astes - Bien Duro.flp", s, rep, &err)) {
      int routed = 0, docked = 0;
      for (const auto& c : s.channels) routed += c.mixerTrack != s.masterId();
      for (const auto& m : s.mixer) docked += m.dock != 0;
      expect(routed > 100, "import fix (Bien Duro, FL 25): channels keep their inserts (event 104)");
      expect(docked > 0, "import fix (Bien Duro): dock sides imported");
    }
  }
  {
    Song s;
    Report rep;
    std::string err;
    if (gl::importFlp(demos + "Atthar Feat Mehran Abbasi & Elise Chantelle - Right Night feeling.flp", s, rep, &err)) {
      int sendAuto = 0;
      for (const auto& a : s.automations) sendAuto += a.owner != gl::kNoId && a.index >= gl::kSendBase && a.index < gl::kSlotBase;
      expect(sendAuto >= 7, "import fix (Atthar): send-level automation clips resolve");
    }
  }
}
