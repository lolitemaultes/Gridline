// Gridline: the piano roll's Vibrato tool (K-262). See PianoVibrato.h.
#include "PianoVibrato.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include "../core/I18n.h"
#include "../core/NoteVibrato.h"
#include "../engine/Generators.h"
#include "../engine/Program.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::pianovib {

namespace {

NoteVibrato gDefault = vib::fromPreset(vib::kPresets[1]);  // Singer
NoteVibrato gClip;
bool gHasClip = false;

// Synced rates snap to note values (a cycle's length in beats).
constexpr float kNoteValues[] = {1.0f / 64, 1.0f / 48, 1.0f / 32, 1.0f / 24, 1.0f / 16, 1.0f / 12, 1.0f / 8, 1.0f / 6, 1.0f / 4, 1.0f / 3, 1.0f / 2, 1.0f, 2.0f, 4.0f};
constexpr int kNoteValueCount = static_cast<int>(sizeof kNoteValues / sizeof kNoteValues[0]);

int nearestValue(float beats) {
  int best = 0;
  for (int i = 1; i < kNoteValueCount; ++i)
    if (std::fabs(std::log(kNoteValues[i] / beats)) < std::fabs(std::log(kNoteValues[best] / beats))) best = i;
  return best;
}

// "1/16", "1/8T", "1 beat" for a cycle of `beats` beats (a quarter note is one beat).
std::string valueName(float beats) {
  const float q = beats / 4.0f;  // of a whole note
  struct N { float v; const char* name; };
  static const N names[] = {{1.0f / 256, "1/256"}, {1.0f / 192, "1/128T"}, {1.0f / 128, "1/128"}, {1.0f / 96, "1/64T"}, {1.0f / 64, "1/64"},
                            {1.0f / 48, "1/32T"},  {1.0f / 32, "1/32"},    {1.0f / 24, "1/16T"}, {1.0f / 16, "1/16"},  {1.0f / 12, "1/8T"},
                            {1.0f / 8, "1/8"},     {1.0f / 4, "1/4"},      {1.0f / 2, "1/2"},    {1.0f, "1 bar"}};
  for (const N& n : names)
    if (std::fabs(n.v - q) < 1e-5f) return n.name;
  char b[32];
  std::snprintf(b, sizeof b, "%.2f %s", static_cast<double>(beats), i18n::tr("beats"));
  return b;
}

std::string rateText(const NoteVibrato& v) {
  if (v.sync) return valueName(v.rate);
  char b[32];
  std::snprintf(b, sizeof b, "%.1f Hz", static_cast<double>(v.rate));
  return b;
}

ImU32 alpha(ImU32 c, float a) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24); }

// The envelope's top and bottom offsets (semitones at full depth) for the vibrato's centre.
void extent(const NoteVibrato& v, double& up, double& down) {
  const double k = 2.0 * v.centre - 1.0, d = v.depth / 100.0;
  up = d * ((1.0 - std::fabs(k) / 2.0) - k / 2.0);
  down = d * (-(1.0 - std::fabs(k) / 2.0) - k / 2.0);
}

struct Geo {
  float xDelay, xIn, xOut, xDepth, yTop;
};
Geo geometry(const NoteVibrato& v, const Box& b) {
  const float w = std::max(1.0f, b.x1 - b.x0);
  double up, down;
  extent(v, up, down);
  const float scale = std::min(b.rowH, 3.0f * b.rowH / std::max(0.01f, static_cast<float>(std::max(up, -down))));
  Geo g;
  g.xDelay = b.x0 + w * v.delay;
  g.xIn = b.x0 + w * std::min(1.0f, v.delay + v.fadeIn);
  g.xOut = b.x0 + w * (1.0f - v.fadeOut);
  g.xOut = std::max(g.xOut, g.xIn);
  g.xDepth = 0.5f * (g.xIn + g.xOut);
  g.yTop = b.cy - static_cast<float>(std::max(up, 0.05)) * scale;
  return g;
}

void applyField(std::vector<Note>& out, const std::vector<size_t>& targets, const NoteVibrato& start, void (*set)(NoteVibrato&, const NoteVibrato&),
                const NoteVibrato& src) {
  for (size_t j : targets) {
    if (j >= out.size()) continue;
    NoteVibrato& v = out[j].vib;
    if (!v.active()) v = start;
    set(v, src);
    vib::clamp(v);
  }
}

void shapeGlyph(ImDrawList* dl, vib::Shape s, ImVec2 c, float w, float h, ImU32 col) {
  ImVec2 pts[25];
  for (int i = 0; i < 25; ++i) {
    const double f = i / 24.0;
    pts[i] = ImVec2(c.x - w * 0.5f + w * static_cast<float>(f), c.y - h * 0.5f * static_cast<float>(vib::wave(s, f * 1.5)));
  }
  dl->AddPolyline(pts, 25, col, ImDrawFlags_None, 1.5f);
}

}  // namespace

const NoteVibrato& defaults() { return gDefault; }

void drawNote(ImDrawList* dl, const Note& n, const Box& b, const Time& tm, bool full, Hit::Kind hot, bool selected) {
  const NoteVibrato& v = n.vib;
  if (!v.active() || n.length <= 0 || b.x1 - b.x0 < 3) return;
  const theme::Palette& P = theme::pal();
  const float w = b.x1 - b.x0;
  const double lenSec = static_cast<double>(n.length) * tm.secPerTick;
  double up, down;
  extent(v, up, down);
  const float scale = std::min(b.rowH, 3.0f * b.rowH / std::max(0.01f, static_cast<float>(std::max(up, -down))));
  const ImU32 col = full ? kViolet : alpha(kViolet, 0.6f);
  // the envelope, shaded, with its top dashed
  const int steps = std::clamp(static_cast<int>(w / 2.0f), 8, 1200);
  if (full) {
    for (int i = 0; i < steps; ++i) {
      const float fa = static_cast<float>(i) / steps, fb = static_cast<float>(i + 1) / steps;
      const double ea = vib::envelope(v, fa), eb = vib::envelope(v, fb);
      if (ea <= 0 && eb <= 0) continue;
      const float xa = b.x0 + w * fa, xb = b.x0 + w * fb;
      const ImVec2 q[4] = {ImVec2(xa, b.cy - static_cast<float>(up * ea) * scale), ImVec2(xb, b.cy - static_cast<float>(up * eb) * scale),
                           ImVec2(xb, b.cy - static_cast<float>(down * eb) * scale), ImVec2(xa, b.cy - static_cast<float>(down * ea) * scale)};
      dl->AddConvexPolyFilled(q, 4, alpha(kViolet, selected ? 0.16f : 0.11f));
      if (i % 3 == 0) dl->AddLine(q[0], q[1], alpha(kViolet, 0.75f), 1.0f);
    }
  }
  // the wave: a dark halo under it keeps it readable on any note colour
  static std::vector<ImVec2> pts;
  pts.clear();
  const int ws = std::clamp(static_cast<int>(w / 1.5f), 8, 2400);
  for (int i = 0; i <= ws; ++i) {
    const double f = static_cast<double>(i) / ws;
    const double s = vib::value(v, f * lenSec, lenSec, tm.beatSec);
    pts.push_back(ImVec2(b.x0 + w * static_cast<float>(f), b.cy - static_cast<float>(s) * scale));
  }
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), alpha(P.surface950, full ? 0.55f : 0.35f), ImDrawFlags_None, full ? 3.2f : 2.6f);
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, full ? 1.6f : 1.2f);
  if (!full) return;
  // handles: fade in and fade out (rings), the depth (a square), the delay (a small tick at the start)
  const Geo g = geometry(v, b);
  auto ring = [&](ImVec2 c, bool on) {
    dl->AddCircleFilled(c, on ? 4.6f : 3.8f, P.text);
    dl->AddCircle(c, on ? 4.6f : 3.8f, kViolet, 0, 1.6f);
  };
  ring(ImVec2(g.xIn, g.yTop), hot == Hit::FadeIn);
  ring(ImVec2(g.xOut, g.yTop), hot == Hit::FadeOut);
  const float r = hot == Hit::Depth ? 4.2f : 3.4f;
  dl->AddRectFilled(ImVec2(g.xDepth - r, g.yTop - r), ImVec2(g.xDepth + r, g.yTop + r), kViolet, 1.0f);
  if (hot == Hit::Depth) dl->AddRect(ImVec2(g.xDepth - r - 1, g.yTop - r - 1), ImVec2(g.xDepth + r + 1, g.yTop + r + 1), P.text, 1.0f);
  if (v.delay > 0 || hot == Hit::Delay) {
    const float t = hot == Hit::Delay ? 5.0f : 3.5f;
    dl->AddLine(ImVec2(g.xDelay, b.cy - t), ImVec2(g.xDelay, b.cy + t), kViolet, 2.0f);
  }
}

Hit::Kind hitTest(const Note& n, const Box& b, ImVec2 m) {
  const float half = std::max(b.rowH * 0.5f, 6.0f);
  if (n.vib.active()) {
    const Geo g = geometry(n.vib, b);
    auto near = [&](float x, float y) { return std::fabs(m.x - x) <= 6.0f && std::fabs(m.y - y) <= 6.0f; };
    if (near(g.xDepth, g.yTop)) return Hit::Depth;
    if (near(g.xIn, g.yTop)) return Hit::FadeIn;
    if (near(g.xOut, g.yTop)) return Hit::FadeOut;
    if (near(g.xDelay, b.cy) && (n.vib.delay > 0 || std::fabs(g.xDelay - g.xIn) > 8)) return Hit::Delay;
    const float top = std::min(g.yTop - 4, b.cy - half);
    if (m.x >= b.x0 && m.x < b.x1 && m.y >= top && m.y <= b.cy + std::max(half, b.cy - top)) return Hit::Body;
    return Hit::None;
  }
  if (m.x >= b.x0 && m.x < b.x1 && m.y >= b.cy - half && m.y <= b.cy + half) return Hit::Body;
  return Hit::None;
}

bool dragTo(Drag& d, ImVec2 m, const Box& b, bool fine, bool snap, std::vector<Note>& out, std::string& readout) {
  if (d.targets.empty() || d.targets[0] >= d.base.size()) return false;
  const float dx = m.x - d.start.x, dy = d.start.y - m.y;
  if (d.axis == 0 && std::fabs(dx) < 3 && std::fabs(dy) < 3) return false;
  out = d.base;
  const Note& n0 = d.base[d.targets[0]];
  NoteVibrato start = gDefault;  // a note without vibrato starts from the strip's, at no depth
  if (!n0.vib.active()) start.depth = 0;
  const NoteVibrato v0 = n0.vib.active() ? n0.vib : start;
  const float w = std::max(1.0f, b.x1 - b.x0);
  const float f = std::clamp((m.x - b.x0) / w, 0.0f, 1.0f);
  const float curveDy = std::clamp(dy / 60.0f, -1.0f, 1.0f);
  NoteVibrato src = v0;
  char buf[160];
  const int count = static_cast<int>(d.targets.size());
  const std::string notes = count > 1 ? " \xC2\xB7 " + i18n::plural(count, "%d note", "%d notes") : std::string();
  switch (d.hit.kind) {
    case Hit::Body:
    case Hit::Depth: {
      if (d.hit.kind == Hit::Depth) d.axis = 1;
      if (d.axis == 0) d.axis = std::fabs(dy) >= std::fabs(dx) ? 1 : 2;
      if (d.axis == 1) {
        double depth = v0.depth + dy / std::max(4.0f, b.rowH) * 100.0 * (fine ? 0.2 : 1.0);
        depth = snap ? std::round(depth / 25.0) * 25.0 : std::round(depth);
        src.depth = static_cast<float>(std::clamp(depth, 0.0, static_cast<double>(vib::kMaxDepth)));
        applyField(out, d.targets, start, [](NoteVibrato& v, const NoteVibrato& s) { v.depth = s.depth; }, src);
        if (src.depth <= 0)
          for (size_t j : d.targets)
            if (j < out.size()) out[j].vib = NoteVibrato{};
      } else {
        const double factor = std::pow(2.0, dx / (fine ? 600.0 : 200.0));
        if (v0.sync) {
          src.rate = kNoteValues[nearestValue(static_cast<float>(v0.rate / factor))];
        } else {
          src.rate = static_cast<float>(std::round(std::clamp(v0.rate * factor, static_cast<double>(vib::kMinRateHz), static_cast<double>(vib::kMaxRateHz)) * 10.0) / 10.0);
        }
        if (src.depth <= 0) src.depth = gDefault.depth;  // sideways on a plain note: the default depth
        applyField(out, d.targets, src, [](NoteVibrato& v, const NoteVibrato& s) { v.rate = s.rate, v.sync = s.sync; }, src);
      }
      std::snprintf(buf, sizeof buf, "%s %.0f ct \xC2\xB7 %s %s", i18n::tr("Depth"), static_cast<double>(src.depth), i18n::tr("rate"), rateText(src).c_str());
      break;
    }
    case Hit::FadeIn:
      src.fadeIn = std::clamp(f - v0.delay, 0.0f, 1.0f - v0.delay);
      src.fadeInCurve = std::clamp(v0.fadeInCurve + curveDy, -1.0f, 1.0f);
      if (std::fabs(dy) < 4) src.fadeInCurve = v0.fadeInCurve;
      applyField(out, d.targets, start, [](NoteVibrato& v, const NoteVibrato& s) { v.fadeIn = s.fadeIn, v.fadeInCurve = s.fadeInCurve; }, src);
      std::snprintf(buf, sizeof buf, "%s %.0f%% \xC2\xB7 %s %+.0f%%", i18n::tr("Fade in"), static_cast<double>(src.fadeIn) * 100, i18n::tr("curve"),
                    static_cast<double>(src.fadeInCurve) * 100);
      break;
    case Hit::FadeOut:
      src.fadeOut = std::clamp(1.0f - f, 0.0f, 1.0f);
      src.fadeOutCurve = std::clamp(v0.fadeOutCurve + curveDy, -1.0f, 1.0f);
      if (std::fabs(dy) < 4) src.fadeOutCurve = v0.fadeOutCurve;
      applyField(out, d.targets, start, [](NoteVibrato& v, const NoteVibrato& s) { v.fadeOut = s.fadeOut, v.fadeOutCurve = s.fadeOutCurve; }, src);
      std::snprintf(buf, sizeof buf, "%s %.0f%% \xC2\xB7 %s %+.0f%%", i18n::tr("Fade out"), static_cast<double>(src.fadeOut) * 100, i18n::tr("curve"),
                    static_cast<double>(src.fadeOutCurve) * 100);
      break;
    case Hit::Delay:
      src.delay = std::clamp(f, 0.0f, 0.95f);
      applyField(out, d.targets, start, [](NoteVibrato& v, const NoteVibrato& s) { v.delay = s.delay; }, src);
      std::snprintf(buf, sizeof buf, "%s %.0f%%", i18n::tr("Delay"), static_cast<double>(src.delay) * 100);
      break;
    default: return false;
  }
  readout = buf + notes;
  return true;
}

void wheelRate(std::vector<Note>& ns, const std::vector<size_t>& targets, float wheel) {
  for (size_t j : targets) {
    if (j >= ns.size() || !ns[j].vib.active()) continue;
    NoteVibrato& v = ns[j].vib;
    const int dir = wheel > 0 ? 1 : -1;
    if (v.sync) {
      const int k = std::clamp(nearestValue(v.rate) - dir, 0, kNoteValueCount - 1);
      v.rate = kNoteValues[k];
    } else {
      v.rate = static_cast<float>(std::round(std::clamp(v.rate * std::pow(1.06, dir), static_cast<double>(vib::kMinRateHz), static_cast<double>(vib::kMaxRateHz)) * 10.0) / 10.0);
    }
    vib::clamp(v);
  }
}

void readoutTip(const std::string& text) {
  const ImVec2 m = ImGui::GetIO().MousePos;
  ImGui::SetNextWindowPos(ImVec2(m.x + 18, m.y - 34));
  ImGui::SetNextWindowBgAlpha(0.96f);
  ImGui::Begin("##vibreadout", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_Tooltip);
  // the numbers in violet, the words in the text colour
  const theme::Palette& P = theme::pal();
  bool first = true;
  size_t at = 0;
  while (at < text.size()) {
    size_t e = at;
    const bool num = std::isdigit(static_cast<unsigned char>(text[at])) || ((text[at] == '+' || text[at] == '-') && at + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[at + 1])));
    while (e < text.size() && (num ? text[e] != ' ' : !std::isdigit(static_cast<unsigned char>(text[e])) && !((text[e] == '+' || text[e] == '-') && e + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[e + 1]))))) ++e;
    if (!first) ImGui::SameLine(0, 0);
    first = false;
    const std::string piece = text.substr(at, e - at);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(num ? kViolet : P.text), "%s", piece.c_str());
    at = e;
  }
  ImGui::End();
}

bool menuBody(App& a, std::vector<Note>& ns, const std::vector<size_t>& targets, const char*& label) {
  (void)a;
  if (targets.empty() || targets[0] >= ns.size()) return false;
  const theme::Palette& P = theme::pal();
  NoteVibrato& v0 = ns[targets[0]].vib;
  const NoteVibrato cur = v0.active() ? v0 : gDefault;
  bool changed = false;
  // The heading: whose vibrato.
  {
    char b[96];
    if (targets.size() > 1) std::snprintf(b, sizeof b, "%s", i18n::plural(static_cast<long>(targets.size()), "VIBRATO, %d SELECTED NOTE", "VIBRATO, %d SELECTED NOTES").c_str());
    else std::snprintf(b, sizeof b, "%s", i18n::tr("VIBRATO"));
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.82f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.textFaint), "%s", b);
    ImGui::PopFont();
  }
  auto every = [&](auto&& fn) {
    for (size_t j : targets)
      if (j < ns.size()) {
        NoteVibrato& v = ns[j].vib;
        if (!v.active()) v = cur;
        fn(v);
        vib::clamp(v);
      }
    changed = true;
  };
  // Shapes: a row of glyph buttons.
  {
    const float bs = ImGui::GetFrameHeight() + 6;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int s = 0; s < static_cast<int>(vib::Shape::Count); ++s) {
      if (s) ImGui::SameLine(0, 4);
      ImGui::PushID(s);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##shape", ImVec2(bs, ImGui::GetFrameHeight()))) {
        every([s](NoteVibrato& v) { v.shape = static_cast<uint8_t>(s); });
        label = "vibrato shape";
      }
      const bool on = cur.shape == s, hov = ImGui::IsItemHovered();
      dl->AddRectFilled(p, ImVec2(p.x + bs, p.y + ImGui::GetFrameHeight()), on ? alpha(kViolet, 0.18f) : hov ? P.surface600 : P.surface700, 3.0f);
      if (on) dl->AddRect(p, ImVec2(p.x + bs, p.y + ImGui::GetFrameHeight()), kViolet, 3.0f);
      shapeGlyph(dl, static_cast<vib::Shape>(s), ImVec2(p.x + bs * 0.5f, p.y + ImGui::GetFrameHeight() * 0.5f), bs - 12, ImGui::GetFrameHeight() - 10,
                 on ? kViolet : P.textDim);
      if (hov) ui::Tooltip(i18n::tr(vib::shapeName(static_cast<vib::Shape>(s))));
      ImGui::PopID();
    }
  }
  if (ImGui::MenuItem(i18n::tr("Rate in Hz"), nullptr, !cur.sync) && cur.sync) {
    every([](NoteVibrato& v) {
      if (v.sync) v.rate = 5.5f, v.rateEnd = 0, v.sync = false;
    });
    label = "vibrato rate in Hz";
  }
  if (ImGui::MenuItem(i18n::tr("Rate synced to the beat"), nullptr, cur.sync) && !cur.sync) {
    every([](NoteVibrato& v) {
      if (!v.sync) v.rate = 0.25f, v.rateEnd = 0, v.sync = true;
    });
    label = "vibrato rate synced";
  }
  {
    const char* centres[] = {i18n::tr("around the pitch"), i18n::tr("only above"), i18n::tr("only below")};
    const float cv[] = {0.5f, 0.0f, 1.0f};
    int ci = cur.centre <= 0.01f ? 1 : cur.centre >= 0.99f ? 2 : 0;
    if (ImGui::BeginMenu(i18n::tr("Centre"))) {
      for (int k = 0; k < 3; ++k)
        if (ImGui::MenuItem(centres[k], nullptr, ci == k)) {
          every([&](NoteVibrato& v) { v.centre = cv[k]; });
          label = "vibrato centre";
        }
      ImGui::EndMenu();
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(centres[ci]).x - 18);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.textFaint), "%s", centres[ci]);
  }
  {
    const int sw = cur.rateEnd <= 0 ? 0 : cur.rateEnd > cur.rate ? 1 : 2;
    const char* sweeps[] = {i18n::tr("off"), i18n::tr("speeds up"), i18n::tr("slows down")};
    if (ImGui::BeginMenu(i18n::tr("Rate sweep"))) {
      for (int k = 0; k < 3; ++k)
        if (ImGui::MenuItem(sweeps[k], nullptr, sw == k)) {
          every([k](NoteVibrato& v) {
            if (k == 0) v.rateEnd = 0;
            else if (v.sync) v.rateEnd = v.rate * (k == 1 ? 0.5f : 2.0f);  // synced: a cycle's length
            else v.rateEnd = v.rate * (k == 1 ? 1.6f : 0.6f);
          });
          label = "vibrato rate sweep";
        }
      ImGui::EndMenu();
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(sweeps[sw]).x - 18);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.textFaint), "%s", sweeps[sw]);
  }
  if (ImGui::BeginMenu(i18n::tr("Presets"))) {
    for (int k = 0; k < vib::kPresetCount; ++k) {
      const vib::Preset& pr = vib::kPresets[k];
      const bool on = v0.active() && v0.depth == pr.depth && v0.rate == pr.rate && !v0.sync && v0.shape == pr.shape;
      if (ImGui::MenuItem(i18n::tr(pr.name), nullptr, on)) {
        every([&](NoteVibrato& v) {
          const float c = v.centre;
          v = vib::fromPreset(pr);
          v.centre = c;
        });
        label = "vibrato preset";
      }
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Copy vibrato"), nullptr, false, v0.active())) {
    gClip = v0;
    gHasClip = true;
  }
  if (ImGui::MenuItem(targets.size() > 1 ? i18n::tr("Paste vibrato onto the selection") : i18n::tr("Paste vibrato"), nullptr, false, gHasClip)) {
    for (size_t j : targets)
      if (j < ns.size()) ns[j].vib = gClip;
    changed = true;
    label = "paste vibrato";
  }
  if (ImGui::MenuItem(i18n::tr("Natural variation per note"), nullptr, false, v0.active() || targets.size() > 1)) {
    std::mt19937 rng(static_cast<uint32_t>(ns[targets[0]].start * 31 + ns[targets[0]].pitch));
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (size_t j : targets)
      if (j < ns.size()) {
        NoteVibrato& v = ns[j].vib;
        if (!v.active()) v = cur;
        v.depth *= 1.0f + 0.15f * u(rng);
        v.rate = v.sync ? v.rate : v.rate * (1.0f + 0.06f * u(rng));
        v.phase = 0.5f + 0.5f * u(rng);
        v.delay = std::clamp(v.delay + 0.05f * u(rng), 0.0f, 0.95f);
        vib::clamp(v);
      }
    changed = true;
    label = "vibrato variation";
  }
  ui::Hint("Each selected note's depth, rate, start and delay move a little, as a player's would");
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Remove vibrato"), nullptr, false, v0.active() || targets.size() > 1)) {
    for (size_t j : targets)
      if (j < ns.size()) ns[j].vib = NoteVibrato{};
    changed = true;
    label = "remove vibrato";
  }
  return changed;
}

bool toolbarStrip(App& a, std::vector<Note>* ns, const std::vector<size_t>& selected, const char*& label) {
  (void)a;
  const theme::Palette& P = theme::pal();
  const float fh = ImGui::GetFrameHeight();
  bool changed = false;
  NoteVibrato before = gDefault;
  ImGui::AlignTextToFramePadding();
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.86f);
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kViolet), "%s", i18n::tr("VIBRATO"));
  ImGui::PopFont();
  ImGui::SameLine(0, 6);
  // the preset (Custom when the values are the user's own)
  int pi = -1;
  for (int k = 0; k < vib::kPresetCount; ++k) {
    const vib::Preset& pr = vib::kPresets[k];
    if (gDefault.depth == pr.depth && gDefault.rate == pr.rate && !gDefault.sync && gDefault.shape == pr.shape) pi = k;
  }
  ImGui::SetNextItemWidth(fh * 4.4f);
  if (ImGui::BeginCombo("##vibpreset", pi >= 0 ? i18n::tr(vib::kPresets[pi].name) : i18n::tr("Custom"))) {
    for (int k = 0; k < vib::kPresetCount; ++k)
      if (ImGui::Selectable(i18n::tr(vib::kPresets[k].name), k == pi)) gDefault = vib::fromPreset(vib::kPresets[k]), label = "vibrato preset";
    ImGui::EndCombo();
  }
  ui::Hint("The vibrato a note gets when you drag on it with the Vibrato tool (and the selected notes' vibrato)");
  ImGui::SameLine(0, 4);
  ImGui::SetNextItemWidth(fh * 2.6f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(P.field));
  if (ImGui::DragFloat("##vibdepth", &gDefault.depth, 1.0f, 1.0f, vib::kMaxDepth, "%.0f ct", ImGuiSliderFlags_AlwaysClamp)) label = "vibrato depth";
  ui::Hint("Depth in cents (100 = a semitone each way)");
  ImGui::SameLine(0, 4);
  ImGui::SetNextItemWidth(fh * 2.8f);
  if (gDefault.sync) {
    int k = nearestValue(gDefault.rate);
    const std::string cur = valueName(kNoteValues[k]);
    if (ImGui::BeginCombo("##vibvalue", cur.c_str(), ImGuiComboFlags_NoArrowButton)) {
      for (int i = 0; i < kNoteValueCount; ++i)
        if (ImGui::Selectable(valueName(kNoteValues[i]).c_str(), i == k)) gDefault.rate = kNoteValues[i], label = "vibrato rate";
      ImGui::EndCombo();
    }
    ui::Hint("One cycle's length, synced to the beat");
  } else {
    if (ImGui::DragFloat("##vibrate", &gDefault.rate, 0.05f, vib::kMinRateHz, vib::kMaxRateHz, "%.1f Hz", ImGuiSliderFlags_AlwaysClamp)) label = "vibrato rate";
    ui::Hint("Rate in cycles a second");
  }
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 4);
  if (ui::TextButton(i18n::tr("Sync"), gDefault.sync, 0.0f, "Rate synced to the beat (follows the tempo)")) {
    gDefault.sync = !gDefault.sync;
    gDefault.rate = gDefault.sync ? 0.25f : 5.5f;
    gDefault.rateEnd = 0;
    label = "vibrato sync";
  }
  vib::clamp(gDefault);
  if (gDefault != before && ns) {
    for (size_t j : selected)
      if (j < ns->size() && (*ns)[j].vib.active()) {
        NoteVibrato& v = (*ns)[j].vib;
        if (gDefault.depth != before.depth) v.depth = gDefault.depth;
        if (gDefault.rate != before.rate || gDefault.sync != before.sync) v.rate = gDefault.rate, v.sync = gDefault.sync, v.rateEnd = 0;
        if (gDefault.shape != before.shape || gDefault.delay != before.delay || gDefault.fadeIn != before.fadeIn) {
          const float c = v.centre;
          v = gDefault;
          v.centre = c;
        }
        changed = true;
      }
  }
  return changed;
}

std::string toolHint(App& a, ObjectId channel) {
  std::string h = i18n::tr("Drag up: depth \xC2\xB7 sideways or wheel: rate \xC2\xB7 handles: fade in and out \xC2\xB7 right-click: shape, presets");
  if (auto rt = a.builder.channelRuntime(channel); rt && rt->gen && !rt->gen->supportsGlide())
    h += std::string(" \xC2\xB7 ") + i18n::tr("this instrument has no per-note pitch: the vibrato goes out as channel pitch bend, shared by notes that overlap");
  return h;
}

std::string describe(const NoteVibrato& v) {
  if (!v.active()) return std::string();
  char b[160];
  std::snprintf(b, sizeof b, "%s %.0f ct, %s", i18n::tr("Vibrato"), static_cast<double>(v.depth), rateText(v).c_str());
  std::string s = b;
  if (v.delay > 0) s += ", " + std::string(i18n::tr("delay")) + " " + std::to_string(static_cast<int>(std::lround(v.delay * 100))) + "%";
  if (v.fadeIn > 0) s += ", " + std::string(i18n::tr("fade in")) + " " + std::to_string(static_cast<int>(std::lround(v.fadeIn * 100))) + "%";
  if (v.fadeOut > 0) s += ", " + std::string(i18n::tr("fade out")) + " " + std::to_string(static_cast<int>(std::lround(v.fadeOut * 100))) + "%";
  return s;
}

}  // namespace gl::pianovib
