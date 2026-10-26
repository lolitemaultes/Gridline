// Gridline — Step Bass editor on the reference transistor-bass instrument's
// 700 × 600 grid and palette (slate-teal panel): program / sequence key row;
// the eight main wheels (tuning, saw / pulse, cutoff, resonance, env mod,
// decay, accent, volume); the tweak row (high pass, min decay, key follow,
// PWM depth / rate, pulse width, clicks) with the distortion block; delay
// and reverb blocks; then the sequencer: on / tempo / steps, and the 16-step
// grid (pitch, gate, slide, accent) of the selected program's sequence,
// editable (the bank text in the state's data, undoable).
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
#include "../flc_kit.h"
#include "models_widgets.h"
#include "strings.h"

namespace gl::packs::models {

using namespace strs;

namespace {

const skin::Skin& sbSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x436470, 0x3A5761, 0x2E4249, 0xE3E9EC, 0x8FB7C4, 0xC5D0D6);
    s.handle = skin::hex(0x22323A);
    s.select = skin::hex(0xF2C14E);
    s.accent2 = skin::hex(0xE87B4C);
    s.lcdBg = skin::hex(0x1F2E34);
    s.lcdText = skin::hex(0xC5D0D6);
    s.pillBg = skin::hex(0x2E4249);
    s.pillText = skin::hex(0xC5D0D6);
    return s;
  }();
  return k;
}

// The selected program's sequence, read from / written to the bank text.
struct SeqView {
  int index = 0, length = 16;
  double bpm = 125;
  int note[kSbSteps];
  bool gate[kSbSteps], accent[kSbSteps], slide[kSbSteps];
  std::string name;
};

int programSequence(const std::string& data, int prog) {
  std::istringstream in(data);
  std::string line;
  while (std::getline(in, line)) {
    int k, patch, seq;
    if (std::sscanf(line.c_str(), "prog %d %d %d", &k, &patch, &seq) == 3 && k == prog) return std::clamp(seq, 0, kSbPrograms - 1);
  }
  return prog;
}

SeqView readSeq(const std::string& data, int seqIndex) {
  SeqView v;
  v.index = seqIndex;
  for (int i = 0; i < kSbSteps; ++i) v.note[i] = 60, v.gate[i] = true, v.accent[i] = v.slide[i] = false;
  std::istringstream in(data);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string kind;
    int k = -1, len = 16;
    double bpm = 125;
    if (!(ls >> kind) || kind != "seq" || !(ls >> k >> len >> bpm) || k != seqIndex) continue;
    v.length = std::clamp(len, 1, kSbSteps);
    v.bpm = bpm;
    for (int i = 0; i < kSbSteps; ++i) {
      std::string tok;
      if (!(ls >> tok) || tok == "|") break;
      int nt = 60, g = 1, a = 0, s = 0;
      if (std::sscanf(tok.c_str(), "%d:%d:%d:%d", &nt, &g, &a, &s) == 4) v.note[i] = nt, v.gate[i] = g, v.accent[i] = a, v.slide[i] = s;
    }
    std::string rest;
    std::getline(ls, rest);
    if (rest.size() > 1) v.name = rest.substr(1);
  }
  return v;
}

std::string writeSeq(const std::string& data, const SeqView& v) {
  std::ostringstream line;
  char buf[48];
  std::snprintf(buf, sizeof buf, "seq %d %d %.6g", v.index, v.length, v.bpm);
  line << buf;
  for (int i = 0; i < kSbSteps; ++i) line << ' ' << v.note[i] << ':' << (v.gate[i] ? 1 : 0) << ':' << (v.accent[i] ? 1 : 0) << ':' << (v.slide[i] ? 1 : 0);
  line << " | " << v.name;
  std::istringstream in(data.empty() ? std::string("stepbass 1\n") : data);
  std::ostringstream out;
  std::string l;
  bool done = false;
  while (std::getline(in, l)) {
    int k = -1;
    if (std::sscanf(l.c_str(), "seq %d", &k) == 1 && k == v.index) {
      out << line.str() << '\n';
      done = true;
    } else {
      out << l << '\n';
    }
  }
  if (!done) out << line.str() << '\n';
  return out.str();
}

const char* noteName(int n) {
  static const char* nm[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return nm[((n % 12) + 12) % 12];
}

}  // namespace

void drawStepBass(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = sbSkin();
  constexpr float kW = 700, kH = 600;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  ImGui::PushID("gl.stepbass");

  // ---- header: name, program, sequence key
  skin::text(dl, F.p(14, 10), K.select, "STEP BASS", F.font(16));
  flk::text(c, 54, 50, "PROGRAM");
  flk::digi(c, SbProgram, 95, 45, 40, 19);
  flk::text(c, 150, 50, "SEQ KEY");
  flk::digi(c, SbRoot, 196, 45, 40, 19);
  {
    const int prog = ui2::ival(st, SbProgram);
    const SeqView sv = readSeq(st.data, programSequence(st.data, prog));
    char buf[80];
    std::snprintf(buf, sizeof buf, "SEQUENCE %d  %s  %d STEPS", sv.index + 1, sv.name.empty() ? "-" : sv.name.c_str(), sv.length);
    flk::lcd(c, 250, 44, 300, 21, buf);
  }

  // ---- main wheels
  flk::section(c, 6, 70, 688, 82, "VOICE");
  flk::wheel(c, SbTune, 24, 80, 50, "Tuning", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, SbWave, 128, 80, 50, "Saw-Pulse");
  flk::chk(c, SbClassicPulse, 88, 132, 36, 12, "OLD");
  flk::wheel(c, SbCutoff, 200, 80, 50, "Cutoff");
  flk::wheel(c, SbReso, 272, 80, 50, "Reso");
  flk::wheel(c, SbEnvMod, 343, 80, 50, "Env mod");
  flk::wheel(c, SbDecay, 414, 80, 50, "Decay");
  flk::wheel(c, SbAccent, 486, 80, 50, "Accent");
  flk::wheel(c, SbVolume, 598, 76, 58, "Volume");

  // ---- tweaks + distortion
  flk::section(c, 6, 160, 420, 92, "TWEAKS");
  flk::wheel(c, SbHighPass, 21, 189, 40, "High pass");
  flk::wheel(c, SbMinDecay, 75, 189, 40, "Min dec");
  flk::wheel(c, SbKeyFollow, 129, 189, 40, "Key flw");
  flk::wheel(c, SbLfoPw, 183, 189, 40, "PWM amt");
  flk::wheel(c, SbLfoRate, 237, 189, 40, "PWM rate");
  flk::wheel(c, SbPulseWidth, 291, 189, 40, "Width");
  flk::wheel(c, SbClicks, 346, 189, 40, "Clicks");
  flk::section(c, 432, 160, 262, 92, "");
  flk::chk(c, SbDistOn, 445, 166, 90, 14, "DISTORTION");
  flk::wheel(c, SbDistHp, 456, 189, 40, "High pass");
  flk::wheel(c, SbDistDrive, 511, 189, 40, "Drive");
  flk::wheel(c, SbDistTone, 566, 189, 40, "Tone");
  flk::wheel(c, SbDistVolume, 622, 189, 40, "Volume");

  // ---- delay + reverb
  flk::section(c, 6, 258, 312, 150, "");
  flk::chk(c, SbDlyOn, 14, 265, 70, 14, "DELAY");
  flk::wheel(c, SbDlyAmount, 21, 288, 40, "Amount");
  flk::wheel(c, SbDlyTime, 75, 288, 40, "Time");
  flk::wheel(c, SbDlyRightTap, 129, 288, 40, "Right tap");
  flk::chk(c, SbDlySync, 196, 300, 70, 14, "SYNC");
  flk::wheel(c, SbDlyFeedback, 21, 349, 40, "Feedback");
  flk::wheel(c, SbDlyTone, 75, 349, 40, "Tone");
  flk::wheel(c, SbDlyStereo, 129, 349, 40, "Stereo");
  flk::section(c, 324, 258, 370, 150, "");
  flk::chk(c, SbRevOn, 332, 265, 70, 14, "REVERB");
  flk::wheel(c, SbRevLowCut, 346, 288, 40, "Low cut");
  flk::wheel(c, SbRevHiCut, 401, 288, 40, "High cut");
  flk::wheel(c, SbRevPredelay, 456, 288, 40, "Predelay");
  flk::wheel(c, SbRevPreFb, 511, 288, 40, "Pre fdbk");
  flk::wheel(c, SbRevSize, 566, 288, 40, "Size");
  flk::wheel(c, SbRevDiffusion, 622, 288, 40, "Diffuse");
  flk::wheel(c, SbRevDecay, 346, 349, 40, "Decay");
  flk::wheel(c, SbRevHiDamp, 401, 349, 40, "Damp");
  flk::wheel(c, SbRevWidth, 456, 349, 40, "Width");
  flk::wheel(c, SbRevDry, 511, 349, 40, "Dry");
  flk::wheel(c, SbRevEr, 566, 349, 40, "Early");
  flk::wheel(c, SbRevWet, 622, 349, 40, "Wet");

  // ---- sequencer
  flk::section(c, 6, 414, 688, 180, "");
  flk::chk(c, SbSeqOn, 17, 425, 80, 14, "SEQUENCER");
  flk::text(c, 300, 427, "OWN TEMPO");
  flk::menu(c, SbTempoMode, 210, 422, 80, 18);
  flk::digi(c, SbBpm, 358, 421, 54, 21);
  {
    const int prog = ui2::ival(st, SbProgram);
    SeqView sv = readSeq(st.data, programSequence(st.data, prog));
    bool changed = false;
    char buf[16];
    flk::text(c, 430, 427, "STEPS");
    if (flk::btn(c, "##lenm", 470, 422, 18, 18, GL_ICON_LABEL("minus", "-"), false) && sv.length > 1) --sv.length, changed = true;
    std::snprintf(buf, sizeof buf, "%d", sv.length);
    flk::lcd(c, 490, 422, 30, 18, buf);
    if (flk::btn(c, "##lenp", 522, 422, 18, 18, GL_ICON_LABEL("plus", "+"), false) && sv.length < 16) ++sv.length, changed = true;
    flk::text(c, 12, 471, "PITCH", K.textDim, 7);
    flk::text(c, 12, 492, "GATE", K.textDim, 7);
    flk::text(c, 12, 513, "SLIDE", K.textDim, 7);
    flk::text(c, 12, 534, "ACCENT", K.textDim, 7);
    // J-68: the step the sequencer is playing (published by the engine).
    int playing = -1;
    if (t.isChannel())
      if (auto rt = a.builder.channelRuntime(t.owner); rt && rt->gen) playing = rt->gen->displayStep();
    if (playing >= 0 && playing < 16) {
      const float x = 58.0f + 36.0f * playing;
      dl->AddRectFilled(F.p(x - 1, 462), F.p(x + 35, 572), theme::withAlpha(K.select, 0.16f), 3 * F.s);
      dl->AddRect(F.p(x - 1, 462), F.p(x + 35, 572), theme::withAlpha(K.select, 0.8f), 3 * F.s, 0, 1.2f);
    }
    for (int i = 0; i < 16; ++i) {
      const float x = 58.0f + 36.0f * i;
      ImGui::PushID(i);
      // Pitch cell: wheel to change by a semitone, click = up, right-click = down.
      std::snprintf(buf, sizeof buf, "%s%d", noteName(sv.note[i]), sv.note[i] / 12 - 1);
      flk::lcd(c, x, 466, 34, 17, buf);
      ImGui::SetCursorScreenPos(F.p(x, 466));
      ImGui::InvisibleButton("##p", F.sz(34, 17));
      if (ImGui::IsItemHovered()) {
        const float wv = ImGui::GetIO().MouseWheel;
        if (wv != 0) sv.note[i] = std::clamp(sv.note[i] + (wv > 0 ? 1 : -1), 0, 127), changed = true;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) sv.note[i] = std::max(0, sv.note[i] - 1), changed = true;
        a.setHint("Step pitch: wheel or click / right-click to change by a semitone");
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) sv.note[i] = std::min(127, sv.note[i] + 1), changed = true;
      bool* rows[3] = {sv.gate, sv.slide, sv.accent};
      static const char* ids[3] = {"##g", "##s", "##a"};
      for (int r = 0; r < 3; ++r)
        if (flk::btn(c, ids[r], x + 2, 487.0f + 21.0f * r, 31, 19, "", rows[r][i])) rows[r][i] = !rows[r][i], changed = true;
      skin::ledDot(dl, F.p(x + 17, 560), 4 * F.s, i < sv.length, K);
      ImGui::PopID();
    }
    if (changed) {
      PluginState ns = st;
      ns.data = writeSeq(st.data, sv);
      setPluginState(a, t, ns, "edit bass sequence");
    }
  }
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::models
