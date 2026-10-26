// Gridline - J-129 look painters (internal to Look*.cpp): drawing helpers and
// the per-direction painter interface. Each direction is a Painter subclass
// with its own tokens; the base class holds token-driven defaults so a
// direction only overrides the elements whose form differs.
#pragma once
#include <algorithm>
#include <cmath>
#include "../../third_party/imgui/imgui.h"
#include "Look.h"
#include "Theme.h"

namespace gl::look {

constexpr float kPi = 3.14159265358979f;
// Knob sweep (screen radians, y down): 7:30 o'clock .. 4:30 o'clock over the top.
constexpr float kA0 = kPi * 0.75f, kA1 = kPi * 2.25f;
inline float angleOf(float norm) { return kA0 + (kA1 - kA0) * std::clamp(norm, 0.0f, 1.0f); }

// ---------------------------------------------------------------- colour
ImU32 rgb(uint32_t rrggbb, float a = 1.0f);
ImU32 alpha(ImU32 c, float a);      // replaces alpha (0..1 of the colour's own alpha)
ImU32 mix(ImU32 a, ImU32 b, float t);
ImU32 lighten(ImU32 c, float t);
ImU32 darken(ImU32 c, float t);
float luma(ImU32 c);

// ---------------------------------------------------------------- shapes
int segs(float r, float sweep = 2 * kPi);
// Vertical gradient over the vertices drawn since `start` (any shape).
void shadeV(ImDrawList* dl, int start, float y0, float y1, ImU32 top, ImU32 bottom);
void shadeH(ImDrawList* dl, int start, float x0, float x1, ImU32 left, ImU32 right);
// Gradient from a (c0) to b (c1) over the vertices drawn since `start`.
void shadeAlong(ImDrawList* dl, int start, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1);
void gradRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 top, ImU32 bottom, float r = 0, ImDrawFlags f = 0);
void gradCircle(ImDrawList* dl, ImVec2 c, float r, ImU32 top, ImU32 bottom);
// Soft drop shadow under a rounded rect: `spread` px of falloff, `dy` offset.
void shadowRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, float spread, float dy, float strength);
void shadowCircle(ImDrawList* dl, ImVec2 c, float r, float spread, float dy, float strength);
// Inner shadow along the top edge of a rounded rect (recessed look).
void innerShadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, float depth, float strength);
void innerShadowCircle(ImDrawList* dl, ImVec2 c, float r, float depth, float strength);
// Fill between a polyline and baseY: ct at the curve, cb at the base (no seams).
void fillUnder(ImDrawList* dl, const ImVec2* pts, int n, float baseY, ImU32 ct, ImU32 cb);
// Arc helpers (a0 < a1).
void arc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float th, bool roundCaps = true);
// Arc whose colour runs from c0 at a0 to c1 at a1.
void arcGrad(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 c0, ImU32 c1, float th);
// Glowing stroke (bloom layers then the core).
void glowArc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float th, float glow);
void glowLine(ImDrawList* dl, const ImVec2* pts, int n, ImU32 col, float th, float glow);
void glowDot(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float glow);
// Text (centred in a box / at a point), with the look fonts.
enum class Font { Body, Bold, Mono, MonoBold };
ImFont* font(Font f);
ImVec2 textSize(Font f, float size, const char* s);
void text(ImDrawList* dl, Font f, float size, ImVec2 pos, ImU32 col, const char* s);
void textIn(ImDrawList* dl, Font f, float size, ImVec2 p0, ImVec2 p1, ImU32 col, const char* s, int align = 1);  // 0 left 1 centre 2 right
// Spaced small caps (label style): `tracking` px between letters.
void caps(ImDrawList* dl, Font f, float size, ImVec2 p0, ImVec2 p1, ImU32 col, const char* s, float tracking, int align = 1);
inline ImVec2 snap(ImVec2 p) { return ImVec2(std::floor(p.x) + 0.5f, std::floor(p.y) + 0.5f); }

// ---------------------------------------------------------------- tokens
struct Tokens {
  // surfaces
  ImU32 panelTop, panelBot, panelLine;
  ImU32 section, sectionLine, sectionTitle;
  ImU32 well, wellLine, grid, gridStrong;
  ImU32 face, faceHi, faceLo, faceLine;  // raised faces (buttons, caps)
  ImU32 field, fieldText;
  // type
  ImU32 text, textDim, textFaint;
  // roles
  ImU32 value, valueHi;  // value arcs / fills
  ImU32 mod;             // modulation
  ImU32 hot;             // lit glyphs / on states
  ImU32 led;             // LEDs
  ImU32 danger;
  ImU32 meterLow, meterMid, meterHigh;
  ImU32 rail;
  // form
  float radius = 4, radiusSm = 3, radiusLg = 6;
  float shadow = 1;  // 0..1 shadow strength
  float glow = 0;    // 0..1 bloom strength
  float line = 1;    // hairline width
};

class Painter {
 public:
  explicit Painter(const Tokens& t) : tk(t) {}
  virtual ~Painter() = default;
  Tokens tk;

  virtual void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k);
  virtual void slider(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Slider& s);
  virtual void fader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Fader& f);
  virtual void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m);
  virtual void grNeedle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float grDb, float inDb, float outDb);
  virtual void led(ImDrawList* dl, ImVec2 c, float r, bool on, float level, ImU32 role, int state);
  virtual void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, int state, ImU32 role);
  virtual void iconButton(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int icon, int state, ImU32 role);
  virtual void toggle(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, int state, ImU32 role);
  virtual void check(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, int state, ImU32 role);
  virtual void segmented(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover, int state);
  virtual void tabs(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* labels, int n, int sel, int hover);
  virtual void dropdown(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, int state);
  virtual void menu(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* const* items, int n, int hover, int checked);
  virtual void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, int state, int caret, bool lcd);
  virtual void stepper(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, int state, int hoverSide);
  virtual void stepPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on, bool alt, float vel, bool playing, int state, ImU32 tint);
  virtual void pad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, float glow, int state, ImU32 tint);
  virtual void panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
  virtual void section(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title);
  virtual void header(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* title, const char* sub);
  virtual void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
  virtual void caption(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text);
  virtual void tooltip(ImDrawList* dl, ImVec2 pos, const char* title, const char* value);
  virtual void scrollbar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float pos, float size, int state);
  virtual void label(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, bool dim);
  virtual void valueText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, int state);
  virtual void graphGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int cols, int rows);
  virtual void curve(ImDrawList* dl, const ImVec2* pts, int n, float baseY, ImU32 col, bool fill);
  virtual void envelope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ImVec2* pts, int n, int sel, float playX, float playY);
  virtual void lfo(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int shape, float phase, float depth);
  virtual void scope(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* s, int n);
  virtual void spectrum(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* db, int n, float peakLine);
  virtual void waveform(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* mn, const float* mx, int n, float playPos, float selA, float selB);
  virtual void wavetable(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int frames, float current, float morph);
  virtual void xyPad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float x, float y, float mx, float my, int state);
  virtual void keyboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int lo, int hi, const bool* held, int rangeLo, int rangeHi);
  virtual void modChip(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, float live, int state);
  virtual void modAmount(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float amount, float live, int state);

  // Override hooks (see Look.h); defaults ignore the hint.
  virtual void hookPanel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) { (void)hint, panel(dl, p0, p1); }
  virtual void hookSection(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) { (void)hint, section(dl, p0, p1, nullptr); }
  virtual void hookWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 hint) { (void)hint, well(dl, p0, p1); }
  virtual void hookRaised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint, int state) { (void)hint, raised(dl, p0, p1, std::min(r, tk.radius), state); }
  virtual void hookInset(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 hint) { (void)hint, sunken(dl, p0, p1, std::min(r, tk.radiusSm), tk.field); }
  virtual ImU32 hookTint(ImU32 fill) { return fill; }
  virtual void hookFace(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state, ImU32 role, bool rest);

  // Building blocks the directions share.
  virtual void raised(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, int state);   // button face
  virtual void sunken(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 fill);  // recessed field
  ImU32 roleOr(ImU32 role, ImU32 def) const { return role ? role : def; }
  ImU32 meterCol(float v) const;  // colour of a meter at level v (0..1)
};

// The painter of a direction (never null; Current maps to the shared skin today).
Painter& painter(Dir d);
Painter* makeGraphite();
Painter* makeHairline();
Painter* makeLumen();
Painter* makeSlate();
Painter* makeClassic();

}  // namespace gl::look
