// Gridline — FLP import, J-22 group "strings": state parsers of the string
// instruments (exposed for the tests: each reports how many bytes it walked).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gl::flp::j22 {

// Two-string physical model ("SSLF" "1rkS" + 114 tag floats + 128-byte name).
struct TwinStringState {
  std::vector<float> tags;  // 114 tag values (knobs 0..1, combos / digit wheels raw)
  std::string name;
  size_t consumed = 0;      // bytes walked
};
bool parseTwinString(const uint8_t* p, size_t n, TwinStringState& out);

// Plucked string (5 × i32).
struct PluckerState {
  int32_t v[5] = {128, 64, 1, 0, 1};
  size_t consumed = 0;
};
bool parsePlucker(const uint8_t* p, size_t n, PluckerState& out);

// Transistor-style bass line: 12 header words (size checks), u16 version,
// u32 sequence root key ×2, u8 sequencer flag, 128 program entries, 256 patch
// records (each program's patch and its stored original), 128 sequences of
// 32 steps, a 24-byte trailer.
struct StepBassState {
  struct Program {
    uint16_t a = 0;
    uint8_t b = 0;
    std::string name;
    uint32_t patch = 0, sequence = 0, c = 0;
  };
  struct Patch {
    std::string name;
    float v[100] = {};
  };
  struct Step {
    // 21 bytes: u8 lead (1), u8 b1 (0), u8 accent, i32 MIDI note, u8 gate,
    // f64 (0.5), f32 (0.5), u8 slide.
    uint8_t lead = 1, b1 = 0, accent = 0;
    int32_t note = 60;
    uint8_t gate = 1;
    double d = 0.5;
    float x = 0.5f;
    uint8_t slide = 0;
  };
  struct Sequence {
    uint16_t pre = 0, b = 0;
    std::string name;
    uint32_t length = 16;
    Step steps[32];
    double bpm = 125;
    uint8_t tail[9] = {};
  };
  uint32_t head[12] = {};
  uint16_t version = 0;
  uint32_t root = 60, root2 = 60;
  uint8_t flag = 0;
  std::vector<Program> programs;   // 128
  std::vector<Patch> patches;      // 256
  std::vector<Sequence> sequences; // 128
  uint32_t trailer[6] = {};        // u32 mode, 3 × u32, f32 bits, u32
  size_t consumed = 0;
};
bool parseStepBass(const uint8_t* p, size_t n, StepBassState& out);
// Gridline's text form of a parsed bank (PluginState::data of gl.stepbass).
std::string stepBassData(const StepBassState& s);

}  // namespace gl::flp::j22
