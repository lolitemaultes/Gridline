// Gridline — channel envelope page (J-72): the per-channel envelopes, LFOs and
// channel filter (engine/ChannelIns) of instruments that take them, edited in
// place on PluginState::ins. One page for both timing behaviours: the
// "FL timing" switch picks the reference's tick-stepped results (set by FL
// imports); off, the same settings run smooth per-sample curves.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../engine/ChannelIns.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl {

using theme::pal;

namespace {

int g_target = InsVol;  // the target the page shows (UI state)

// Text of a time knob: seconds (or beats when synced).
std::string timeText(int v, bool sync) {
  char b[32];
  const double t = insKnobTime(v, sync);
  if (sync) std::snprintf(b, sizeof b, t < 10 ? "%.2f beats" : "%.0f beats", t);
  else if (t < 1.0) std::snprintf(b, sizeof b, "%.0f ms", t * 1000.0);
  else std::snprintf(b, sizeof b, "%.2f s", t);
  return b;
}

ParamDesc rangeDesc(const char* name, double mn, double mx, double def) {
  ParamDesc d;
  d.name = name;
  d.min = mn, d.max = mx, d.def = def;
  return d;
}

// The amount range of each target (pan ±64, volume / mod X / mod Y ±128, pitch ±2400 cents).
int amountRange(int target) { return target == InsPan ? 64 : target == InsPitch ? 2400 : 128; }

// Envelope shape preview: delay, attack, hold, decay to sustain, a held part, release.
void drawShape(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const InsTarget& t, bool on) {
  const auto& P = pal();
  dl->AddRectFilled(p0, p1, P.well, 4);
  const float w = p1.x - p0.x, h = p1.y - p0.y, pad = 6;
  const bool sync = t.flags & 1;
  const double tDel = insKnobTime(t.predelay, sync), tAtt = insKnobTime(t.attack, sync), tHold = insKnobTime(t.hold, sync);
  const double tDec = insKnobTime(t.decay, sync), tRel = insKnobTime(t.release, sync);
  const double sus = t.sustain / 128.0;
  const double held = std::max(0.25, 0.25 * (tDel + tAtt + tHold + tDec + tRel));
  const double total = tDel + tAtt + tHold + tDec + held + tRel;
  auto X = [&](double s) { return p0.x + pad + static_cast<float>(s / total) * (w - 2 * pad); };
  auto Y = [&](double v) { return p1.y - pad - static_cast<float>(v) * (h - 2 * pad); };
  for (int k = 1; k < 4; ++k) dl->AddLine(ImVec2(p0.x, p0.y + h * k / 4), ImVec2(p1.x, p0.y + h * k / 4), P.line);
  const ImU32 col = on ? P.hot : P.textFaint;
  std::vector<ImVec2> pts;
  auto seg = [&](double t0, double t1, int tension, double from, double to, bool falling) {
    const int n = 24;
    for (int i = 0; i <= n; ++i) {
      const double u = static_cast<double>(i) / n;
      double v;
      if (!falling) v = from + (to - from) * insCurve(tension, u);
      else v = to + (from - to) * insCurve(tension, 1.0 - u);  // falling segments run x from 1 down
      pts.push_back(ImVec2(X(t0 + (t1 - t0) * u), Y(v)));
    }
  };
  double at = 0;
  pts.push_back(ImVec2(X(0), Y(0)));
  at += tDel;
  pts.push_back(ImVec2(X(at), Y(0)));
  seg(at, at + tAtt, t.tensionA, 0, 1, false);
  at += tAtt;
  pts.push_back(ImVec2(X(at + tHold), Y(1)));
  at += tHold;
  // Decay: level follows the curve from 1 down to the sustain level.
  {
    const int n = 24;
    for (int i = 0; i <= n; ++i) {
      const double u = static_cast<double>(i) / n;
      const double v = std::max(sus, insCurve(t.tensionD, 1.0 - u));
      pts.push_back(ImVec2(X(at + tDec * u), Y(v)));
    }
  }
  at += tDec;
  pts.push_back(ImVec2(X(at + held), Y(sus)));
  at += held;
  {
    const int n = 24;
    for (int i = 0; i <= n; ++i) {
      const double u = static_cast<double>(i) / n;
      pts.push_back(ImVec2(X(at + tRel * u), Y(sus * insCurve(t.tensionR, 1.0 - u))));
    }
  }
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, 2.0f);
  // Note-off marker.
  const float xo = X(at);
  dl->AddLine(ImVec2(xo, p0.y + 3), ImVec2(xo, p1.y - 3), theme::withAlpha(P.textDim, 0.5f), 1.0f);
}

}  // namespace

void drawChannelEnvelope(App& a, ObjectId id) {
  const Channel* c = a.doc.song().channel(id);
  if (!c) return;
  const auto& P = pal();
  InsSettings st;
  if (!parseIns(c->plugin.ins, st)) {
    st = InsSettings{};
    st.timing = InsTiming::Smooth;  // new envelopes in Gridline: smooth timing
  }
  // Edits rewrite the channel's envelope text (one undo step per gesture).
  auto edit = [&](const char* label, uint64_t key, const std::function<void(InsSettings&)>& fn) {
    a.doc.editChannel(
        id, label,
        [&](Channel& ch) {
          InsSettings s;
          if (!parseIns(ch.plugin.ins, s)) {
            s = InsSettings{};
            s.timing = InsTiming::Smooth;
          }
          fn(s);
          ch.plugin.ins = formatIns(s);
        },
        key ? (0x494E5300000000ull ^ (static_cast<uint64_t>(id) << 12) ^ key) : 0);
  };

  // Header: target and timing.
  {
    const char* targets[] = {i18n::tr("Panning"), i18n::tr("Volume"), i18n::tr("Mod X"), i18n::tr("Mod Y"), i18n::tr("Pitch")};
    const char* hints[] = {i18n::tr("Panning LFO (the panning envelope is kept but does not move the voice)"),
                           i18n::tr("Volume envelope and tremolo"), i18n::tr("Mod X: filter cutoff envelope and LFO"),
                           i18n::tr("Mod Y: filter resonance envelope and LFO"), i18n::tr("Pitch envelope and vibrato (cents)")};
    ui::Segmented("##instarget", targets, InsTargets, &g_target, 72, 0, hints);
    ImGui::SameLine(0, 16);
    if (ui::TextButton("FL timing", st.classic(), 96,
                       "On: envelopes and LFOs step once per song tick with FL Studio's exact results (FL imports switch it on). "
                       "Off: smooth per-sample curves."))
      edit("switch envelope timing", 0, [](InsSettings& s) { s.timing = s.classic() ? InsTiming::Smooth : InsTiming::Classic; });
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const int k = std::clamp(g_target, 0, InsTargets - 1);
  const InsTarget& t = st.t[k];
  const float cell = 62;

  // A knob cell (knob, value text, name) over one integer field of the target.
  auto knob = [&](const char* key, int value, const ParamDesc& d, bool bipolar, const std::string& text, const std::function<void(InsTarget&, int)>& set) {
    ImGui::PushID(key);
    ImGui::BeginGroup();
    const float x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + (cell - 28) * 0.5f);
    double v = value;
    bool rel = false;
    if (ui::Knob("k", &v, d, 14, nullptr, bipolar, &rel)) {
      const int nv = static_cast<int>(std::lround(d.clamp(v)));
      edit("change channel envelope", std::hash<std::string>{}(key) + static_cast<uint64_t>(k),
           [&](InsSettings& s) { set(s.t[k], nv); });
    }
    if (rel) a.doc.endGesture();
    ImGui::PushFont(a.fontMono, 10.0f);
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(P.text), "%s", text.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, 11.0f);
    ts = ImGui::CalcTextSize(d.name.c_str());
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(P.textDim), "%s", d.name.c_str());
    ImGui::PopFont();
    ImGui::SetCursorPosX(x);
    ImGui::Dummy(ImVec2(cell, 0));
    ImGui::EndGroup();
    ImGui::PopID();
    ImGui::SameLine(0, 0);
  };
  char buf[48];
  auto pct = [&](double v) {
    std::snprintf(buf, sizeof buf, "%.0f%%", v);
    return std::string(buf);
  };

  // ---- Envelope.
  ImGui::BeginGroup();
  ui::Label("ENVELOPE");
  ImGui::SameLine();
  if (ui::TextButton(t.on ? "On" : "Off", t.on, 44, "Switch this target's envelope on or off"))
    edit("toggle channel envelope", 0, [&](InsSettings& s) { s.t[k].on = !s.t[k].on; });
  ImGui::SameLine();
  if (ui::TextButton("Tempo", t.flags & 1, 56, "Envelope times in beats (tempo-synced) instead of seconds"))
    edit("toggle envelope tempo sync", 0, [&](InsSettings& s) { s.t[k].flags ^= 1; });
  {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz(cell * 7, 74);
    ImGui::Dummy(sz);
    drawShape(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + sz.x, p0.y + sz.y), t, t.on);
  }
  const bool es = t.flags & 1;
  knob("del", t.predelay, rangeDesc(i18n::tr("Delay"), 100, 65536, 100), false, timeText(t.predelay, es), [](InsTarget& x, int v) { x.predelay = v; });
  knob("att", t.attack, rangeDesc(i18n::tr("Attack"), 100, 65536, 20000), false, timeText(t.attack, es), [](InsTarget& x, int v) { x.attack = v; });
  knob("hold", t.hold, rangeDesc(i18n::tr("Hold"), 100, 65536, 20000), false, timeText(t.hold, es), [](InsTarget& x, int v) { x.hold = v; });
  knob("dec", t.decay, rangeDesc(i18n::tr("Decay"), 100, 65536, 30000), false, timeText(t.decay, es), [](InsTarget& x, int v) { x.decay = v; });
  knob("sus", t.sustain, rangeDesc(i18n::tr("Sustain"), 0, 128, 50), false, pct(t.sustain * 100.0 / 128.0), [](InsTarget& x, int v) { x.sustain = v; });
  knob("rel", t.release, rangeDesc(i18n::tr("Release"), 100, 65536, 20000), false, timeText(t.release, es), [](InsTarget& x, int v) { x.release = v; });
  if (k != InsVol) {
    const int r = amountRange(k);
    std::string amt;
    if (k == InsPitch) {
      std::snprintf(buf, sizeof buf, "%+d ct", t.amount);
      amt = buf;
    } else {
      amt = pct(t.amount * 100.0 / r);
    }
    knob("amt", t.amount, rangeDesc(i18n::tr("Amount"), -r, r, 0), true, amt, [](InsTarget& x, int v) { x.amount = v; });
  }
  ImGui::NewLine();
  knob("ta", t.tensionA, rangeDesc(i18n::tr("Att. curve"), -128, 128, 0), true, pct(t.tensionA * 100.0 / 128.0), [](InsTarget& x, int v) { x.tensionA = v; });
  knob("td", t.tensionD, rangeDesc(i18n::tr("Dec. curve"), -128, 128, 0), true, pct(t.tensionD * 100.0 / 128.0), [](InsTarget& x, int v) { x.tensionD = v; });
  knob("tr", t.tensionR, rangeDesc(i18n::tr("Rel. curve"), -128, 128, 0), true, pct(t.tensionR * 100.0 / 128.0), [](InsTarget& x, int v) { x.tensionR = v; });
  ImGui::NewLine();
  ImGui::EndGroup();

  // ---- LFO.
  ImGui::SameLine(0, 18);
  ImGui::BeginGroup();
  ui::Label("LFO");
  ImGui::SameLine();
  if (ui::TextButton("Tempo##lfo", t.flags & 2, 56, "LFO times in beats (tempo-synced) instead of seconds"))
    edit("toggle LFO tempo sync", 0, [&](InsSettings& s) { s.t[k].flags ^= 2; });
  ImGui::SameLine();
  if (ui::TextButton("Global", t.flags & 0x20, 56, "One LFO phase locked to the song for every note, instead of restarting with each note"))
    edit("toggle global LFO", 0, [&](InsSettings& s) { s.t[k].flags ^= 0x20; });
  {
    const char* shapes[] = {i18n::tr("Sine"), i18n::tr("Triangle"), i18n::tr("Pulse")};
    int sh = std::clamp(t.lfoShape, 0, 2);
    if (ui::Segmented("##lfoshape", shapes, 3, &sh, 52)) edit("change LFO shape", 0, [&](InsSettings& s) { s.t[k].lfoShape = sh; });
  }
  const bool ls = t.flags & 2;
  {
    const int r = amountRange(k);
    std::string amt;
    if (k == InsPitch) {
      std::snprintf(buf, sizeof buf, "%+d ct", t.lfoAmount);
      amt = buf;
    } else {
      amt = pct(t.lfoAmount * 100.0 / r);
    }
    knob("lamt", t.lfoAmount, rangeDesc(i18n::tr("Amount"), -r, r, 0), true, amt, [](InsTarget& x, int v) { x.lfoAmount = v; });
  }
  {
    const double per = insKnobTime(t.lfoSpeed, ls);
    if (ls) std::snprintf(buf, sizeof buf, "%.2f beats", per);
    else std::snprintf(buf, sizeof buf, "%.2f Hz", per > 0 ? 1.0 / per : 0.0);
    knob("lspd", t.lfoSpeed, rangeDesc(i18n::tr("Period"), 200, 65536, 32950), false, buf, [](InsTarget& x, int v) { x.lfoSpeed = v; });
  }
  ImGui::NewLine();
  knob("ldel", t.lfoDelay, rangeDesc(i18n::tr("Delay"), 100, 65536, 100), false, timeText(t.lfoDelay, ls), [](InsTarget& x, int v) { x.lfoDelay = v; });
  knob("latt", t.lfoAttack, rangeDesc(i18n::tr("Attack"), 100, 65536, 20000), false, timeText(t.lfoAttack, ls), [](InsTarget& x, int v) { x.lfoAttack = v; });
  ImGui::NewLine();
  ImGui::EndGroup();

  // ---- Filter (mod X / mod Y knobs).
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ImGui::Separator();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ui::Label("FILTER");
  ImGui::SameLine();
  {
    const char* types[] = {i18n::tr("Fast LP"), i18n::tr("LP"), i18n::tr("BP"), i18n::tr("HP"), i18n::tr("Notch"), i18n::tr("LP x2"), i18n::tr("SVF LP"), i18n::tr("SVF x2")};
    int ty = std::clamp(st.type, 0, 7);
    if (ui::Segmented("##filtype", types, 8, &ty, 56)) edit("change channel filter type", 0, [&](InsSettings& s) { s.type = ty; });
  }
  auto fknob = [&](const char* key, int value, const ParamDesc& d, const std::function<void(InsSettings&, int)>& set) {
    ImGui::PushID(key);
    ImGui::BeginGroup();
    const float x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + (cell - 28) * 0.5f);
    double v = value;
    bool rel = false;
    if (ui::Knob("k", &v, d, 14, nullptr, false, &rel)) {
      const int nv = static_cast<int>(std::lround(d.clamp(v)));
      edit("change channel filter", std::hash<std::string>{}(key), [&](InsSettings& s) { set(s, nv); });
    }
    if (rel) a.doc.endGesture();
    const std::string txt = pct(value * 100.0 / 256.0);
    ImGui::PushFont(a.fontMono, 10.0f);
    ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(P.text), "%s", txt.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, 11.0f);
    ts = ImGui::CalcTextSize(d.name.c_str());
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(P.textDim), "%s", d.name.c_str());
    ImGui::PopFont();
    ImGui::SetCursorPosX(x);
    ImGui::Dummy(ImVec2(cell, 0));
    ImGui::EndGroup();
    ImGui::PopID();
    ImGui::SameLine(0, 0);
  };
  fknob("cut", st.cutoff, rangeDesc(i18n::tr("Cutoff"), 0, 256, 256), [](InsSettings& s, int v) { s.cutoff = v; });
  fknob("res", st.reso, rangeDesc(i18n::tr("Resonance"), 0, 256, 0), [](InsSettings& s, int v) { s.reso = v; });
  ImGui::NewLine();

  // ---- Velocity / key tracking: how far a note's velocity or key from the
  // middle value moves its panning and filter (mod X / mod Y).
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ui::Label("TRACKING");
  {
    auto tknob = [&](const char* key, int row, int col, const ParamDesc& d, const std::string& txt) {
      ImGui::PushID(key);
      ImGui::BeginGroup();
      const float x = ImGui::GetCursorPosX();
      ImGui::SetCursorPosX(x + (cell - 28) * 0.5f);
      double v = st.track[row][col];
      bool rel = false;
      if (ui::Knob("k", &v, d, 14, nullptr, col > 0, &rel)) {
        const int nv = static_cast<int>(std::lround(d.clamp(v)));
        edit("change channel tracking", std::hash<std::string>{}(key) + 77, [&](InsSettings& s) { s.track[row][col] = nv; });
      }
      if (rel) a.doc.endGesture();
      ImGui::PushFont(a.fontMono, 10.0f);
      ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
      ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
      ImGui::TextColored(theme::vec(P.text), "%s", txt.c_str());
      ImGui::PopFont();
      ImGui::PushFont(nullptr, 11.0f);
      ts = ImGui::CalcTextSize(d.name.c_str());
      ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
      ImGui::TextColored(theme::vec(P.textDim), "%s", d.name.c_str());
      ImGui::PopFont();
      ImGui::SetCursorPosX(x);
      ImGui::Dummy(ImVec2(cell, 0));
      ImGui::EndGroup();
      ImGui::PopID();
      ImGui::SameLine(0, 0);
    };
    static const char* kNotes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::snprintf(buf, sizeof buf, "%d", st.track[0][0]);
    tknob("vmid", 0, 0, rangeDesc(i18n::tr("Vel. middle"), 0, 128, 100), buf);
    tknob("vx", 0, 2, rangeDesc(i18n::tr("Vel. → X"), -128, 128, 0), pct(st.track[0][2] * 100.0 / 128.0));
    tknob("vy", 0, 3, rangeDesc(i18n::tr("Vel. → Y"), -128, 128, 0), pct(st.track[0][3] * 100.0 / 128.0));
    tknob("vp", 0, 1, rangeDesc(i18n::tr("Vel. → pan"), -128, 128, 0), pct(st.track[0][1] * 100.0 / 128.0));
    {
      const int m = std::clamp(st.track[1][0], 0, 131);
      std::snprintf(buf, sizeof buf, "%s%d", kNotes[m % 12], m / 12 - 1);  // Gridline names MIDI 60 C4
    }
    tknob("kmid", 1, 0, rangeDesc(i18n::tr("Key middle"), 0, 131, 60), buf);
    tknob("kx", 1, 2, rangeDesc(i18n::tr("Key → X"), -128, 128, 0), pct(st.track[1][2] * 100.0 / 128.0));
    tknob("ky", 1, 3, rangeDesc(i18n::tr("Key → Y"), -128, 128, 0), pct(st.track[1][3] * 100.0 / 128.0));
    tknob("kp", 1, 1, rangeDesc(i18n::tr("Key → pan"), -128, 128, 0), pct(st.track[1][1] * 100.0 / 128.0));
    ImGui::NewLine();
  }
  ImGui::PushFont(nullptr, 11.0f);
  ImGui::TextColored(theme::vec(P.textFaint), "%s",
                     st.classic() ? i18n::tr("FL timing: these settings play as FL Studio plays them (stepped once per tick).")
                                  : i18n::tr("Smooth timing: per-sample envelopes and LFOs. Switch FL timing on for FL Studio's results."));
  ImGui::PopFont();
}

}  // namespace gl
