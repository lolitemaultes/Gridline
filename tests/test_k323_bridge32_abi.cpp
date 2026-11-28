// Gridline — K-323 unit tests: bitness-independent Sandbox SHM ABI invariants,
// synthetic PE32 / PE32+ / ARM64 / corrupt header classification, bridge32 path
// resolution & fallback error formatting, and ScanResult cache persistence.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

#include "Check.h"
#include "core/Platform.h"
#include "host/PluginScan.h"
#include "host/sandbox/Sandbox.h"

namespace fs = std::filesystem;
using namespace gl;
using namespace gl::host;

namespace {

void put16(std::vector<unsigned char>& b, size_t at, uint16_t v) {
  b[at] = static_cast<unsigned char>(v & 0xFFu);
  b[at + 1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
}

void put32(std::vector<unsigned char>& b, size_t at, uint32_t v) {
  for (int k = 0; k < 4; ++k) b[at + static_cast<size_t>(k)] = static_cast<unsigned char>((v >> (8 * k)) & 0xFFu);
}

fs::path writeSyntheticPe(const fs::path& p, uint16_t machine, uint16_t optionalMagic) {
  std::vector<unsigned char> b(0x200, 0);
  b[0] = 'M';
  b[1] = 'Z';
  put32(b, 0x3C, 0x80);
  b[0x80] = 'P';
  b[0x81] = 'E';
  b[0x82] = 0;
  b[0x83] = 0;
  put16(b, 0x84, machine);
  put16(b, 0x86, 1);
  put16(b, 0x94, optionalMagic == 0x20B ? 240 : 224);
  put16(b, 0x96, 0x2022);
  put16(b, 0x98, optionalMagic);
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return p;
}

void testSandboxAbiInvariants() {
  using namespace gl::host::shm;

  // Trivially copyable wire sub-structs (no pointers, no virtuals, no std::string).
  static_assert(std::is_trivially_copyable_v<ShmParamChange>, "ShmParamChange must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<ShmNoteEvent>, "ShmNoteEvent must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<ShmTransport>, "ShmTransport must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<ShmMidiOut>, "ShmMidiOut must be trivially copyable");

  // Exact sizes & alignments identical between 32-bit x86 (i686) and 64-bit x86_64/ARM64.
  expect(sizeof(ShmParamChange) == 24 && alignof(ShmParamChange) == 8, "abi: ShmParamChange is 24 bytes, 8-byte aligned");
  expect(sizeof(ShmNoteEvent) == 48 && alignof(ShmNoteEvent) == 8, "abi: ShmNoteEvent is 48 bytes, 8-byte aligned");
  expect(sizeof(ShmTransport) == 88 && alignof(ShmTransport) == 8, "abi: ShmTransport is 88 bytes, 8-byte aligned");
  expect(sizeof(ShmMidiOut) == 8 && alignof(ShmMidiOut) == 4, "abi: ShmMidiOut is 8 bytes, 4-byte aligned");
  expect(sizeof(ShmBlock) == 250048 && alignof(ShmBlock) == 8, "abi: ShmBlock is 250048 bytes, 8-byte aligned");

  // Every 8-byte field sits on an 8-byte boundary (eliminating i686 vs x86_64 alignment drift).
  expect(offsetof(ShmParamChange, value) % 8 == 0, "abi: ShmParamChange::value 8-byte aligned");
  expect(offsetof(ShmNoteEvent, tuning) % 8 == 0, "abi: ShmNoteEvent::tuning 8-byte aligned");
  expect(offsetof(ShmTransport, sampleRate) % 8 == 0 && offsetof(ShmTransport, projectSamples) % 8 == 0 &&
             offsetof(ShmTransport, systemNanos) % 8 == 0,
         "abi: ShmTransport 64-bit fields 8-byte aligned");
  expect(offsetof(ShmBlock, transport) == 32 && offsetof(ShmBlock, params) == 120 && offsetof(ShmBlock, events) == 49272 &&
             offsetof(ShmBlock, in) == 98424 && offsetof(ShmBlock, out) == 114808 && offsetof(ShmBlock, midiOut) == 245952,
         "abi: ShmBlock key offsets match wire specification");

  // Round-trip conversions preserve every field faithfully.
  Transport t;
  t.sampleRate = 96000.0;
  t.tempo = 132.5;
  t.projectQn = 12.75;
  t.barQn = 12.0;
  t.projectSamples = 123456789012LL;
  t.continuousSamples = 987654321098LL;
  t.barNumber = 42;
  t.tsNum = 7;
  t.tsDen = 8;
  t.playing = true;
  t.recording = false;
  t.looping = true;
  t.loopStartQn = 4.0;
  t.loopEndQn = 16.0;
  t.changed = true;
  t.offline = true;
  t.systemNanos = 555666777888LL;
  const Transport t2 = fromShm(toShm(t));
  expect(t2.sampleRate == t.sampleRate && t2.tempo == t.tempo && t2.projectQn == t.projectQn && t2.barQn == t.barQn &&
             t2.projectSamples == t.projectSamples && t2.continuousSamples == t.continuousSamples && t2.barNumber == t.barNumber &&
             t2.tsNum == t.tsNum && t2.tsDen == t.tsDen && t2.playing == t.playing && t2.recording == t.recording &&
             t2.looping == t.looping && t2.loopStartQn == t.loopStartQn && t2.loopEndQn == t.loopEndQn && t2.changed == t.changed &&
             t2.offline == t.offline && t2.systemNanos == t.systemNanos,
         "abi: ShmTransport round-trip preserves all fields");

  NoteEvent ne;
  ne.type = NoteEvent::Expression;
  ne.expr = NoteEvent::ExprBrightness;
  ne.value = 0.72f;
  ne.offset = 17;
  ne.noteId = 9001;
  ne.key = 64;
  ne.channel = 3;
  ne.velocity = 0.88f;
  ne.tuning = -0.35;
  ne.midi[0] = 0xB3;
  ne.midi[1] = 74;
  ne.midi[2] = 99;
  ne.hasProps = true;
  ne.pan = -0.25f;
  ne.modX = 0.3f;
  ne.modY = 0.8f;
  const NoteEvent ne2 = fromShm(toShm(ne));
  expect(ne2.type == ne.type && ne2.expr == ne.expr && ne2.value == ne.value && ne2.offset == ne.offset && ne2.noteId == ne.noteId &&
             ne2.key == ne.key && ne2.channel == ne.channel && ne2.velocity == ne.velocity && ne2.tuning == ne.tuning &&
             ne2.midi[0] == ne.midi[0] && ne2.midi[1] == ne.midi[1] && ne2.midi[2] == ne.midi[2] && ne2.hasProps == ne.hasProps &&
             ne2.pan == ne.pan && ne2.modX == ne.modX && ne2.modY == ne.modY,
         "abi: ShmNoteEvent round-trip preserves all fields");

  MidiOut mo{23, {0x90, 60, 120}};
  const MidiOut mo2 = fromShm(toShm(mo));
  expect(mo2.offset == 23 && mo2.data[0] == 0x90 && mo2.data[1] == 60 && mo2.data[2] == 120,
         "abi: ShmMidiOut round-trip preserves all fields");
}

void testPeClassification() {
  const fs::path dir = fs::temp_directory_path() / "gl_k323_pe_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  const fs::path pX86 = writeSyntheticPe(dir / "plug32.dll", 0x014C, 0x010B);
  const fs::path pX64 = writeSyntheticPe(dir / "plug64.dll", 0x8664, 0x020B);
  const fs::path pArm = writeSyntheticPe(dir / "plugArm.dll", 0xAA64, 0x020B);
  const fs::path pBadMagic = writeSyntheticPe(dir / "badmagic.dll", 0x014C, 0x020B);
  const fs::path pText = dir / "notape.dll";
  std::ofstream(pText, std::ios::binary) << "plain text, definitely not a PE file";
  const fs::path pTrunc = dir / "trunc.dll";
  std::ofstream(pTrunc, std::ios::binary) << "MZ\x00\x00";

  expect(peArch(pX86.u8string()) == PeArch::X86 && std::string(peArchName(PeArch::X86)) == "x86", "pe: PE32 (0x014C/0x10B) -> X86");
  expect(peArch(pX64.u8string()) == PeArch::X64 && std::string(peArchName(PeArch::X64)) == "x64", "pe: PE32+ (0x8664/0x20B) -> X64");
  expect(peArch(pArm.u8string()) == PeArch::Arm64 && std::string(peArchName(PeArch::Arm64)) == "Arm64", "pe: ARM64 (0xAA64/0x20B) -> Arm64");
  expect(peArch(pBadMagic.u8string()) == PeArch::Unknown, "pe: mismatched machine/magic -> Unknown");
  expect(peArch(pText.u8string()) == PeArch::Unknown, "pe: plain text -> Unknown");
  expect(peArch(pTrunc.u8string()) == PeArch::Unknown, "pe: truncated MZ header -> Unknown");
  expect(peArch((dir / "does_not_exist.dll").u8string()) == PeArch::Unknown, "pe: missing file -> Unknown");

  // Single-file VST2 & VST3 detection.
  expect(needsBridge32ForPath("vst2", pX86.u8string()), "bridge32: 32-bit VST2 DLL needs bridge32");
  expect(!needsBridge32ForPath("vst2", pX64.u8string()), "bridge32: 64-bit VST2 DLL does not need bridge32");
  expect(!needsBridge32ForPath("clap", pX86.u8string()), "bridge32: CLAP is not routed to bridge32");

  // VST3 bundle with only Contents/x86-win -> needsBridge32.
  const fs::path bundle32 = dir / "Legacy32.vst3";
  writeSyntheticPe(bundle32 / "Contents" / "x86-win" / "Legacy32.vst3", 0x014C, 0x010B);
  expect(needsBridge32ForPath("vst3", bundle32.u8string()), "bridge32: x86-win-only VST3 bundle needs bridge32");

  // VST3 bundle with both x86-win and x86_64-win -> native 64-bit preferred, no bridge32.
  const fs::path bundleBoth = dir / "DualArch.vst3";
  writeSyntheticPe(bundleBoth / "Contents" / "x86-win" / "DualArch.vst3", 0x014C, 0x010B);
  writeSyntheticPe(bundleBoth / "Contents" / "x86_64-win" / "DualArch.vst3", 0x8664, 0x020B);
  expect(!needsBridge32ForPath("vst3", bundleBoth.u8string()), "bridge32: dual-arch VST3 bundle uses native 64-bit");

  fs::remove_all(dir, ec);
}

void testBridge32PathsAndErrors() {
  platform::setEnv("GRIDLINE_BRIDGE32_EXE", "");
  const std::string winPath = bridge32Executable("/opt/win/gridline-app.exe");
  expect(winPath.find("gridline-bridge32.exe") != std::string::npos, "bridge32: .exe main resolves sibling gridline-bridge32.exe");

  const std::string unixPath = bridge32Executable("/usr/local/bin/gridline-app");
  expect(unixPath.find("gridline-bridge32") != std::string::npos, "bridge32: unix main resolves sibling gridline-bridge32");

  const std::string macErr = bridge32UnavailableError(unixPath, true);
  expect(macErr == "32-bit plugins are not supported on macOS 10.15+", "bridge32: macOS error message is exact and informative");

  const std::string winErr = bridge32UnavailableError("C:/Gridline/gridline-bridge32.exe", false);
  expect(winErr.find("gridline-bridge32.exe") != std::string::npos && winErr.find("C:/Gridline/gridline-bridge32.exe") != std::string::npos,
         "bridge32: Windows missing-helper error names gridline-bridge32.exe and path");

  // ScanResult JSON persistence preserves needsBridge32 flag.
  ScanResult r;
  r.format = "vst2";
  r.bundle = "C:/VstPlugins/OldSynth32.dll";
  r.ok = true;
  r.needsBridge32 = true;
  const Json j = toJson(r);
  ScanResult r2;
  expect(fromJson(j, r2) && r2.needsBridge32 && r2.bundle == r.bundle, "cache: ScanResult needsBridge32 round-trips through JSON");

  ScanResult r64;
  r64.format = "vst2";
  r64.bundle = "C:/VstPlugins/Modern64.dll";
  r64.ok = true;
  r64.needsBridge32 = false;
  const Json j64 = toJson(r64);
  ScanResult r64b;
  expect(j64.find("needsBridge32") == nullptr && fromJson(j64, r64b) && !r64b.needsBridge32,
         "cache: ScanResult omits needsBridge32 when false");
}

}  // namespace

int main() {
  testSandboxAbiInvariants();
  testPeClassification();
  testBridge32PathsAndErrors();
  return finish();
}
