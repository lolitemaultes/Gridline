// Gridline — "plasma" pack: the keyboard drawer that slides up from the play bar: the bend and
// mod wheels in their slots, the bend range and trigger octave steppers, and a keyboard to play
// the sound with the mouse (velocity by where a key is pressed, drag across the keys to slide).
// Drawn first in the frame; while the pointer is over it, nothing under it sees the pointer
// (drawFrame hides the pointer from the rest of the editor).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include "../../app/EditorZoomUi.h"
#include "../../app/KeyGlide.h"
#include "../../app/KeyLight.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {
bool isBlackKey(int k) {
  const int pc = ((k % 12) + 12) % 12;
  return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
// Plasma's note names: middle C (60) is C3.
std::string keyName(int k) {
  static const char* const n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(n[((k % 12) + 12) % 12]) + std::to_string(k / 12 - 2);
}
// A value's leading minus as a real minus sign (a clear stroke at every size).
std::string withMinus(std::string s) {
  for (size_t i = 0; i < s.size(); ++i)
    if (s[i] == '-' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9' && (i == 0 || s[i - 1] == ' ')) s.replace(i, 1, "\xE2\x88\x92");
  return s;
}
// What the drawer's own items did this frame: one of them held (active) or under the pointer.
// Each item reports itself right after it is submitted (later calls change the "last item").
struct Took {
  bool own = false, hot = false;
};
Took* gTook = nullptr;
void tookItem() {
  if (!gTook) return;
  const bool act = ImGui::IsItemActive();
  gTook->own = gTook->own || act, gTook->hot = gTook->hot || act || ImGui::IsItemHovered();
}
// The key the drawer's keyboard sounds (one mouse: one at a time, whichever editor it is in). The
// keyboard plays it with keyglide::follow while its button is held; it stops when the button is
// let go, on Escape, when the drawer is told to close, and (a check at the start of every frame)
// as soon as the keyboard is no longer drawn: the drawer or the window closed while a key was held.
struct Sounding {
  App* a = nullptr;
  ObjectId ch = kNoId;
  const Ui* u = nullptr;  // the editor that plays it (compared only, never read)
  int note = -1;
  int frame = 0;      // the last frame its keyboard was drawn
  bool esc = false;   // Escape stopped it: silent until the button is let go
};
Sounding gSound;
void stopSounding() {
  if (gSound.note >= 0 && gSound.a && gSound.ch != kNoId) gSound.a->engine.noteOff(gSound.ch, gSound.note);
  gSound.note = -1;
}
App* gSoundApp = nullptr;  // (for the dev report: the engine's count of held live keys)
void sweepSounding(ImGuiContext*, ImGuiContextHook*) {
  if (gSound.note >= 0 && ImGui::GetFrameCount() - gSound.frame > 1) stopSounding();
  if (drawerReport() && gSoundApp) {  // dev: the key it sounds and the keys the engine holds, on change
    static int lastNote = -2, lastLive = -1;
    const int live = gSoundApp->engine.liveHeld();
    if (gSound.note != lastNote || live != lastLive) {
      lastNote = gSound.note, lastLive = live;
      std::fprintf(stderr, "PLASMA_DRAWER_SOUND frame %d note %d live %d\n", ImGui::GetFrameCount(), gSound.note, live);
    }
  }
}
void hookSweep() {
  static ImGuiContext* hooked = nullptr;
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (!ctx || hooked == ctx) return;
  ImGuiContextHook h;
  h.Type = ImGuiContextHookType_NewFramePost;
  h.Callback = sweepSounding;
  ImGui::AddContextHook(ctx, &h);
  hooked = ctx;
}
ImVec4 rectOf(ImVec2 a, ImVec2 b) {
  const ImVec2 p = edzoomui::toScreenHere(a), q = edzoomui::toScreenHere(b);
  return ImVec4(p.x, p.y, q.x, q.y);
}
}  // namespace

// ------------------------------------------------------------------ wheel
// A performance wheel in a recessed slot: a rounded drum whose grip lines roll with the value
// (spaced as on a turning cylinder), the value lit in the accent colour from the centre detent
// (bend) or from the bottom (mod). Its name sits under the slot, the value while it moves. The
// bend wheel glides back to the centre when let go; the mod wheel stays where it is put.
void wheel(Ctx& c, int k, float x, float y, float w, float h, bool spring) {
  const ParamDesc* d = pdesc(k);
  if (!d) return;
  const char* name = spring ? "Bend" : "Mod";
  layoutBox(c, x, y, w, h, spring ? "bend wheel" : "mod wheel");
  ImGui::PushID(k);
  ImGui::PushID("wheel");
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  const float in = c.s(4);  // the drum inside the slot
  const ImVec2 wa(a.x + c.s(4), a.y + in), wb(b.x - c.s(4), b.y - in);
  ImGui::SetCursorScreenPos(a);
  ImGui::InvisibleButton("##wh", c.F.sz(w, h));
  tookItem();
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  struct Back {
    double from = 0;
    float t = -1;
  };
  static std::map<std::pair<const Ui*, int>, Back> back;  // a released bend wheel on its way back, per editor
  Back& bk = back[{&c.u, k}];
  double v = val(c, k);
  if (act) bk.t = -1;
  paramItem(c, k, v, h, false, wa.y, wb.y - wa.y);
  if (spring && ImGui::IsItemDeactivated() && std::fabs(v - d->def) > 1e-9) bk.from = v, bk.t = 0;
  if (bk.t >= 0) {  // the glide back: 0.12 s, easing out
    bk.t = std::min(1.f, bk.t + ImGui::GetIO().DeltaTime / 0.12f);
    const float e = 1 - (1 - bk.t) * (1 - bk.t);
    v = bk.t >= 1 ? d->def : bk.from + (d->def - bk.from) * e;
    setv(c, k, v, true);
    if (bk.t >= 1) bk.t = -1, c.a.doc.endGesture();
  }
  const float n = static_cast<float>(std::clamp(d->toNormalized(v), 0.0, 1.0));
  const float r = std::round(c.s(5));
  // the slot: a recess in the well colour, its upper lip in shadow
  c.dl->AddRectFilled(a, b, c.P.well, r);
  c.dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + c.s(3)), IM_COL32(0, 0, 0, c.P.dark ? 60 : 26), r, ImDrawFlags_RoundCornersTop);
  // the drum: light across its middle, darker as it turns away at the top and bottom
  const float cy = std::round((wa.y + wb.y) * 0.5f), rd = std::round(c.s(3));
  skin::gradientRect(c.dl, wa, ImVec2(wb.x, cy), c.P.capBot, c.P.capTop, rd, ImDrawFlags_RoundCornersTop);
  skin::gradientRect(c.dl, ImVec2(wa.x, cy), wb, c.P.capTop, c.P.capBot, rd, ImDrawFlags_RoundCornersBottom);
  // the value: from the detent (bend) or the bottom (mod) to the value, lit
  const float R = (wb.y - wa.y) * 0.5f;
  const float vy = spring ? cy - (n - 0.5f) * 2.f * (R - c.s(2)) : wb.y - c.s(2) - n * (wb.y - wa.y - c.s(4));
  const float from = spring ? cy : wb.y - c.s(2);
  if (std::fabs(vy - from) > 0.5f)
    c.dl->AddRectFilled(ImVec2(wa.x + c.s(1.5f), std::min(vy, from)), ImVec2(wb.x - c.s(1.5f), std::max(vy, from)),
                        theme::withAlpha(c.P.accent, act ? 0.42f : 0.30f), c.s(1.5f));
  // the grip: ridges on a turning drum (closer together and fainter towards its top and bottom)
  c.dl->PushClipRect(wa, wb, true);
  {
    constexpr int kRidges = 18;
    constexpr float kPi = 3.14159265f, kTurn = 2.4f;  // radians the drum turns over the whole range
    const float phase = (n - (spring ? 0.5f : 0.f)) * kTurn;
    const float lw = std::max(1.f, std::round(c.s(1)));
    for (int i = 0; i < kRidges; ++i) {
      float th = std::fmod(phase + i * (2 * kPi / kRidges), 2 * kPi);
      if (th > kPi) th -= 2 * kPi;
      if (th < -kPi) th += 2 * kPi;
      const float cs = std::cos(th);
      if (cs < 0.12f) continue;
      const float ry = std::round(cy - R * 0.96f * std::sin(th)) + 0.5f;
      c.dl->AddLine(ImVec2(wa.x + c.s(2), ry), ImVec2(wb.x - c.s(2), ry), theme::withAlpha(c.P.frame, (c.P.dark ? 0.55f : 0.35f) * cs), lw);
      c.dl->AddLine(ImVec2(wa.x + c.s(2), ry + lw), ImVec2(wb.x - c.s(2), ry + lw), IM_COL32(255, 255, 255, static_cast<int>((c.P.dark ? 22 : 60) * cs)), lw);
    }
  }
  c.dl->PopClipRect();
  // the value mark across the drum
  c.dl->AddLine(ImVec2(wa.x + c.s(1), std::round(vy) + 0.5f), ImVec2(wb.x - c.s(1), std::round(vy) + 0.5f), act || hov ? c.P.hot : c.P.accent,
                std::max(1.5f, std::round(c.s(2))));
  if (spring) {  // the centre detent: a notch on each side of the slot
    const float t = std::max(1.f, std::round(c.s(1.2f)));
    c.dl->AddLine(ImVec2(a.x + c.s(1), cy + 0.5f), ImVec2(wa.x - c.s(0.5f), cy + 0.5f), c.P.textDim, t);
    c.dl->AddLine(ImVec2(wb.x + c.s(0.5f), cy + 0.5f), ImVec2(b.x - c.s(1), cy + 0.5f), c.P.textDim, t);
  }
  c.dl->AddRect(a, b, act ? c.P.accent : hov ? c.P.lineHi : c.P.line, r, 0, std::max(1.f, c.s(act ? 1.5f : 1.f)));
  // its name, or the value while it moves
  if (act || bk.t >= 0) {
    textMid(c, x - 8, x + w + 8, y + h + M(13), withMinus(ptext(c, k)).c_str(), c.P.accentText, T(10.5f), false);
  } else {
    textMid(c, x - 8, x + w + 8, y + h + M(13), name, hov ? c.P.text : c.P.label, T(11), false);
  }
  if (hov)
    c.a.setHint(paramLongName(k) + ": " + ptext(c, k) +
                (spring ? "  (drag up or down; it springs back to the centre \xC2\xB7 double-click: reset \xC2\xB7 right-click: menu)"
                        : "  (drag up or down; it stays where you leave it \xC2\xB7 double-click: reset \xC2\xB7 right-click: menu)"));
  if (drawerReport()) (spring ? drawerDev().pitch : drawerDev().mod) = rectOf(a, b);
  ImGui::PopID();
  ImGui::PopID();
}

// ------------------------------------------------------------------ stepper field
// A field for a whole-number parameter: its name at the left, the value, then minus and plus.
// A drag up or down on the field steps it too, as do the wheel and the keys of a dial.
bool stepField(Ctx& c, int k, float x, float y, float w, float h, const char* label, const char* shown) {
  const ParamDesc* d = pdesc(k);
  if (!d) return false;
  layoutBox(c, x, y, w, h, label);
  ImGui::PushID(k);
  ImGui::PushID("stepf");
  const int cur = vali(c, k);
  const int lo = static_cast<int>(d->min), hi = d->kind == ParamKind::Choice ? static_cast<int>(d->choices.size()) - 1 : static_cast<int>(d->max);
  const float bw = M(22), fx1 = x + w - bw * 2;
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##f", c.F.sz(fx1 - x, h));
  tookItem();
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  double v = cur;
  bool changed = paramItem(c, k, v, std::max(40.f, static_cast<float>(hi - lo) * 9.f), false);
  int stepTo = cur;
  bool anyHov = hov;
  // minus and plus
  ImVec2 pm[2];
  for (int i = 0; i < 2; ++i) {
    const float bx = fx1 + bw * i;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(c.p(bx, y));
    const bool can = i == 0 ? cur > lo : cur < hi;
    if (ImGui::InvisibleButton("##pm", c.F.sz(bw, h)) && can) stepTo = std::clamp(cur + (i == 0 ? -1 : 1), lo, hi);
    tookItem();
    pm[i] = edzoomui::toScreenHere(c.p(bx + bw * 0.5f, y + h * 0.5f));
    const bool bh = ImGui::IsItemHovered(), ba = ImGui::IsItemActive();
    anyHov = anyHov || bh;
    if (bh) c.a.setHint(paramLongName(k) + (i == 0 ? ": one step down" : ": one step up"));
    if (ba || bh) c.dl->AddRectFilled(c.p(bx + 1, y + 1), c.p(bx + bw - 1, y + h - 1), ba ? c.P.panelLo : c.P.panelHi, i == 1 ? c.s(M(3)) : 0.f,
                                      i == 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_None);
    const ImU32 gc = !can ? c.P.textFaint : bh ? c.P.text : c.P.textDim;
    const ImVec2 g = c.p(bx + bw * 0.5f, y + h * 0.5f);
    const float gs = std::round(c.s(M(4))), lw = std::max(1.f, std::round(c.s(M(1.3f))));
    const float gx = std::round(g.x), gy = std::round(g.y) + (static_cast<int>(lw) % 2 ? 0.5f : 0.f);
    c.dl->AddLine(ImVec2(gx - gs, gy), ImVec2(gx + gs, gy), gc, lw);
    if (i == 1) c.dl->AddLine(ImVec2(gx + (static_cast<int>(lw) % 2 ? 0.5f : 0.f), gy - gs), ImVec2(gx + (static_cast<int>(lw) % 2 ? 0.5f : 0.f), gy + gs), gc, lw);
    ImGui::PopID();
  }
  if (stepTo != cur) setv(c, k, stepTo), changed = true;
  // the field
  const ImVec2 a = c.p(x, y), b = c.p(x + w, y + h);
  c.dl->AddRect(a, b, act ? c.P.accent : anyHov ? c.P.lineHi : c.P.line, c.s(M(4)));
  vline(c, fx1, y + 4, y + h - 4, c.P.line);
  vline(c, fx1 + bw, y + 4, y + h - 4, c.P.line);
  labelBeside(c, x + M(10), y, h, label, hov ? c.P.text : c.P.label, T(11.5f), false);
  {  // the value in the regular face: its minus stays a clear stroke at every editor size (the mono one thins out when reduced)
    const std::string s = withMinus(shown ? std::string(shown) : ptext(c, k));
    const float sw = textW(c, s.c_str(), T(11.5f), false);
    labelBeside(c, fx1 - M(10) - sw, y, h, s.c_str(), act ? c.P.accentText : c.P.text, T(11.5f), false);
  }
  if (hov) c.a.setHint(paramLongName(k) + ": " + (shown ? std::string(shown) : ptext(c, k)) +
                       "  (drag up or down, the wheel, or minus and plus \xC2\xB7 double-click: reset \xC2\xB7 right-click: menu)");
  if (drawerReport()) {
    ImVec4& o = k == G_BendUp ? drawerDev().up : k == G_BendDown ? drawerDev().down : drawerDev().oct;
    o = ImVec4(pm[1].x, pm[1].y, pm[0].x, pm[0].y);
  }
  ImGui::PopID();
  ImGui::PopID();
  return changed;
}

// The field's background (drawn before stepField so the field's own marks sit on it).
namespace {
void fieldBg(const Ctx& c, float x, float y, float w, float h) { c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.field, c.s(M(4))); }
}  // namespace

// ------------------------------------------------------------------ keyboard
// Whole octaves from C to C, as many as fit at a comfortable key width, centred on middle C.
// Above the keys a thin strip shows the layers' key zones (when a layer plays only part of the
// keyboard) and the trigger octave (while the arpeggiator or the phrases use it).
void keyboard(Ctx& c, float x, float y, float w, float h) {
  constexpr float kZoneH = 9;
  const int oct = std::clamp(static_cast<int>(w / (7 * 21.f)), 2, 7);
  const int lo = std::max(0, 60 - 12 * (oct / 2)), hi = std::min(127, lo + 12 * oct);
  const int whites = 7 * oct + 1;
  const float ky = y + kZoneH;
  const ImVec2 A = c.p(x, ky), B = c.p(x + w, y + h);
  const float KW = (B.x - A.x) / whites;
  auto edge = [&](int i) { return std::round(A.x + KW * i); };
  // each key's rectangle on screen
  struct Key {
    int k;
    ImVec2 a, b;
  };
  Key keys[128];
  int nKeys = 0, wi = 0;
  const float bh = std::round((B.y - A.y) * 0.62f), bwid = std::max(3.f, std::round(KW * 0.58f));
  for (int k = lo; k <= hi; ++k) {
    if (!isBlackKey(k)) {
      keys[nKeys++] = {k, ImVec2(edge(wi), std::round(A.y)), ImVec2(edge(wi + 1), std::round(B.y))};
      ++wi;
    }
  }
  const int nWhite = nKeys;
  wi = 0;
  for (int k = lo; k <= hi; ++k) {
    if (!isBlackKey(k)) {
      ++wi;
      continue;
    }
    // a black key sits on the line between two white keys, nudged as on a real keyboard
    const int pc = k % 12;
    const float nudge = pc == 1 ? -0.10f : pc == 3 ? 0.10f : pc == 6 ? -0.12f : pc == 8 ? 0.f : 0.12f;
    const float cx = edge(wi) + nudge * KW;
    const float l = std::round(cx - bwid * 0.5f);
    keys[nKeys++] = {k, ImVec2(l, std::round(A.y)), ImVec2(l + bwid, std::round(A.y) + bh)};
  }
  // the pointer
  const ImVec2 m = ImGui::GetIO().MousePos;
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##kb", c.F.sz(w, h));
  tookItem();
  const bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
  int hit = -1;
  float vel = 0.8f;
  if (hov || act) {
    for (int i = nKeys - 1; i >= 0 && hit < 0; --i) {  // black keys first (on top)
      const Key& q = keys[i];
      const bool inY = isBlackKey(q.k) ? (m.y >= q.a.y && m.y < q.b.y) : true;
      if (m.x >= q.a.x && m.x < q.b.x && inY) {
        hit = q.k;
        // velocity by where the key is pressed: softer at its back, harder at its front
        const float t = std::clamp((m.y - q.a.y) / std::max(1.f, q.b.y - q.a.y), 0.f, 1.f);
        vel = std::round((0.25f + 0.75f * t) * 127.f) / 127.f;
      }
    }
  }
  // a press sounds the key under the pointer; a slide onto the next key stops the old one and
  // starts the new one (off the keys the last one keeps sounding); letting go stops it
  hookSweep();
  gSoundApp = &c.a;
  const ObjectId ch = c.t.isChannel() ? c.t.owner : kNoId;
  if (act && (gSound.u != &c.u || gSound.ch != ch)) {  // this keyboard takes the mouse's key over
    stopSounding();
    gSound = Sounding{&c.a, ch, &c.u, -1, 0, false};
  }
  if (gSound.u == &c.u) {
    gSound.frame = ImGui::GetFrameCount();
    if (act && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) stopSounding(), gSound.esc = true;
    if (!act) gSound.esc = false;
    const int was = gSound.note;
    if (ch == kNoId) gSound.note = act && !gSound.esc && c.u.drawer ? (hit >= 0 ? hit : gSound.note) : -1;  // (shown, not sounded)
    else keyglide::follow(c.a, ch, act && !gSound.esc && c.u.drawer, hit, gSound.note, std::clamp(vel, 0.05f, 1.f));
    if (gSound.note >= 0 && gSound.note != was) {
      c.u.heldVel = vel;
      if (drawerReport()) drawerDev().lastKey = gSound.note, drawerDev().lastVel = vel;
    }
  }
  c.u.heldKey = gSound.u == &c.u ? gSound.note : -1;
  const keylight::ChannelLights* lit = c.t.isChannel() ? keylight::lights(c.a, c.t.owner) : nullptr;
  auto level = [&](int k) { return k == c.u.heldKey ? 1.f : (lit ? lit->key(k) : 0.f); };
  // the bed under the keys shows as the seams between them
  const ImU32 seam = theme::mix(c.P.keyWhite, c.P.keyBlack, 0.42f);
  c.dl->AddRectFilled(ImVec2(A.x, std::round(A.y)), ImVec2(edge(whites), std::round(B.y)), seam, c.s(3), ImDrawFlags_RoundCornersBottom);
  const float rk = std::round(c.s(2.5f)), lip = std::max(2.f, std::round(c.s(4)));
  MonoText mono;
  for (int i = 0; i < nWhite; ++i) {
    const Key& q = keys[i];
    const float lv = level(q.k);
    const bool down = lv > 0.5f;
    const ImVec2 a(q.a.x + (i > 0 ? 1.f : 0.f), q.a.y), b = q.b;
    ImU32 top = c.P.keyWhite, bot = theme::mix(c.P.keyWhite, c.P.keyBlack, 0.08f);
    if (hit == q.k && !act) top = theme::mix(top, c.P.accent, 0.10f), bot = theme::mix(bot, c.P.accent, 0.10f);
    if (lv > 0.004f) top = theme::mix(top, c.P.accent, 0.85f * lv), bot = theme::mix(bot, c.P.accent, 0.85f * lv);
    skin::gradientRect(c.dl, a, ImVec2(b.x, b.y - (down ? 0.f : lip)), top, bot, rk, down ? ImDrawFlags_RoundCornersBottom : ImDrawFlags_None);
    if (!down)  // the key's front edge
      c.dl->AddRectFilled(ImVec2(a.x, b.y - lip), b, theme::mix(bot, c.P.keyBlack, 0.16f), rk, ImDrawFlags_RoundCornersBottom);
    else  // pressed: the key sits lower, in the shadow of its neighbours
      c.dl->AddRectFilled(a, ImVec2(b.x, a.y + std::max(1.f, std::round(c.s(2)))), IM_COL32(0, 0, 0, 40));
    if (q.k % 12 == 0 && b.x - a.x >= c.s(12)) {  // the octave's name on its C
      const std::string s = keyName(q.k);
      textMid(c, (a.x - c.F.o.x) / c.F.s, (b.x - c.F.o.x) / c.F.s, (b.y - c.F.o.y) / c.F.s - lip / c.F.s - M(8), s.c_str(),
              theme::mix(down ? top : c.P.keyWhite, c.P.keyBlack, 0.55f), T(9), false);
    }
  }
  for (int i = nWhite; i < nKeys; ++i) {
    const Key& q = keys[i];
    const float lv = level(q.k);
    ImU32 top = skin::lighten(c.P.keyBlack, 0.16f), bot = c.P.keyBlack, face = skin::lighten(c.P.keyBlack, 0.07f);
    if (hit == q.k && !act) top = theme::mix(top, c.P.accent, 0.18f), face = theme::mix(face, c.P.accent, 0.18f);
    if (lv > 0.004f) top = theme::mix(top, c.P.accent, 0.9f * lv), bot = theme::mix(bot, c.P.accent, 0.75f * lv), face = theme::mix(face, c.P.accent, 0.9f * lv);
    const float rb = std::round(c.s(1.5f));
    c.dl->AddRectFilled(ImVec2(q.a.x + 1, q.a.y), ImVec2(q.b.x + 1, q.b.y + 1), IM_COL32(0, 0, 0, 50), rb, ImDrawFlags_RoundCornersBottom);  // its shadow
    skin::gradientRect(c.dl, q.a, q.b, top, bot, rb, ImDrawFlags_RoundCornersBottom);
    // its top face, the slope to its front below it
    const float fi = std::max(1.f, std::round(c.s(1.5f))), front = std::max(2.f, std::round(c.s(lv > 0.5f ? 2.f : 5.f)));
    c.dl->AddRectFilled(ImVec2(q.a.x + fi, q.a.y), ImVec2(q.b.x - fi, q.b.y - front), face, rb, ImDrawFlags_RoundCornersBottom);
  }
  // the strip above: the layers' key zones, the trigger octave
  {
    auto keyL = [&](int k) {
      for (int i = 0; i < nKeys; ++i)
        if (keys[i].k == k) return keys[i].a.x;
      return k < lo ? A.x : edge(whites);
    };
    auto keyR = [&](int k) {
      for (int i = 0; i < nKeys; ++i)
        if (keys[i].k == k) return keys[i].b.x;
      return k < lo ? A.x : edge(whites);
    };
    const float sy = c.p(0, y).y, th = std::max(2.f, std::round(c.s(2.5f)));
    int row = 0;
    for (int i = 0; i < 4; ++i) {
      const bool noise = i == 3;
      if (val(c, noise ? noiseP(N_Enable) : oscP(i, O_Enable)) < 0.5) continue;
      const int zl = vali(c, noise ? tailP(T_NKeyMin) : oscP(i, O_KeyMin)), zh = vali(c, noise ? tailP(T_NKeyMax) : oscP(i, O_KeyMax));
      if (zl <= 0 && zh >= 127) continue;  // the whole keyboard: nothing to show
      const float x0 = keyL(std::max(zl, lo)), x1 = keyR(std::min(zh, hi));
      if (x1 <= x0) continue;
      const float ry = std::round(sy + c.s(1) + row * (th + std::max(1.f, std::round(c.s(1)))));
      c.dl->AddRectFilled(ImVec2(x0 + 1, ry), ImVec2(x1 - 1, ry + th), partCol(c, noise ? 4 : i), th * 0.5f);
      ++row;
    }
    const int trig = vali(c, G_TrigOctave);
    if (trig > 0 && (val(c, G_ArpOn) >= 0.5 || val(c, G_ClipOn) >= 0.5)) {
      const int t0 = (trig - 1) * 12, t1 = t0 + 11;
      if (t1 >= lo && t0 <= hi) {
        const float x0 = keyL(std::max(t0, lo)), x1 = keyR(std::min(t1, hi)), by = std::round(A.y) - std::max(2.f, std::round(c.s(2.5f))) + 0.5f;
        c.dl->AddLine(ImVec2(x0 + 2, by), ImVec2(x1 - 2, by), c.P.textDim, 1.f);
        c.dl->AddLine(ImVec2(x0 + 2.5f, by), ImVec2(x0 + 2.5f, by + std::round(c.s(2))), c.P.textDim, 1.f);
        c.dl->AddLine(ImVec2(x1 - 2.5f, by), ImVec2(x1 - 2.5f, by + std::round(c.s(2))), c.P.textDim, 1.f);
      }
    }
  }
  if (hov) {
    if (hit >= 0)
      c.a.setHint(keyName(hit) + ": click to play, drag across the keys to slide from one to the next (lower on a key plays louder)");
    else
      c.a.setHint("Click to play, drag across the keys to slide from one to the next (lower on a key plays louder)");
  }
  if (drawerReport()) drawerDev().keys = rectOf(c.p(x, ky), c.p(x + w, y + h));
}

// ------------------------------------------------------------------ the Keyboard button's glyph
// Four white keys (three lines between them) and the two black keys of C D E F, snapped to whole
// pixels so it stays crisp at every editor size; the outline as heavy as Plasma's other glyphs.
void pianoGlyph(const Ctx& c, ImVec2 at, ImU32 col) {
  // laid out in device pixels (a reduced or magnified editor is scaled after it is drawn), then
  // mapped back, so every edge lands on a whole pixel on screen
  const float z = std::max(0.1f, edzoomui::currentZoom());
  const ImVec2 o = edzoomui::toScreenHere(ImVec2(0, 0));
  auto back = [&](float x, float y) { return ImVec2((x - o.x) / z, (y - o.y) / z); };
  auto fill = [&](float x0, float y0, float x1, float y1, float r = 0, ImDrawFlags fl = 0) { c.dl->AddRectFilled(back(x0, y0), back(x1, y1), col, r / z, fl); };
  const ImVec2 q = edzoomui::toScreenHere(at);
  const float lw = std::max(1.f, std::round(c.s(M(1.25f)) * z));
  const float kw = std::max(2.f, std::round(c.s(M(3.6f)) * z));  // a white key's width, between the lines (it follows the text's size)
  const float gw = kw * 4 + lw * 3, gh = std::max(5.f, std::round(kw * 2.5f));
  const float x0 = std::round(q.x - (gw + lw * 2) * 0.5f), y0 = std::round(q.y - (gh + lw * 2) * 0.5f);
  const float x1 = x0 + gw + lw * 2, y1 = y0 + gh + lw * 2;
  const float rr = std::max(1.f, std::round(c.s(M(1.5f)) * z));
  // the outline, as four filled sides so its weight is exact
  fill(x0, y0, x1, y0 + lw, rr, ImDrawFlags_RoundCornersTop);
  fill(x0, y1 - lw, x1, y1, rr, ImDrawFlags_RoundCornersBottom);
  fill(x0, y0, x0 + lw, y1);
  fill(x1 - lw, y0, x1, y1);
  const float bkH = std::round(gh * 0.58f), bkW = kw >= 3 ? std::max(lw + 2, std::round(kw * 0.62f)) : lw + 1;
  for (int i = 1; i < 4; ++i) {
    const float lx = x0 + lw + kw * i + lw * (i - 1);  // the line between white keys i - 1 and i
    const bool black = i < 3;                          // C# and D#; none between E and F
    fill(lx, black ? y0 + lw + bkH : y0 + lw, lx + lw, y1 - lw);
    if (black) {
      const float bl = std::round(lx + lw * 0.5f - bkW * 0.5f);
      fill(bl, y0 + lw, bl + bkW, y0 + lw + bkH);
    }
  }
}

// ------------------------------------------------------------------ the drawer
namespace {
// The drawer's handle: a short rounded bar at the middle of its top edge (a click closes it).
void handle(Ctx& c, float dw, float y) {
  const float hw = 44, hy = y + 4;
  ImGui::SetCursorScreenPos(c.p(dw * 0.5f - hw * 0.5f - 8, y));
  ImGui::InvisibleButton("##handle", c.F.sz(hw + 16, 12));
  tookItem();
  const bool hv = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked()) c.u.drawer = false;
  if (hv) c.a.setHint("Close the keyboard drawer (Alt+K)");
  c.dl->AddRectFilled(c.p(dw * 0.5f - hw * 0.5f, hy), c.p(dw * 0.5f + hw * 0.5f, hy + 4), hv ? c.P.textDim : c.P.lineHi, c.s(2));
}
// The header's right side: the key being played and its velocity, or how to play.
void playedText(Ctx& c, float rx, float cy) {
  MonoText mono;
  char b[64];
  if (c.u.heldKey >= 0) {
    std::snprintf(b, sizeof b, "%s  vel %d", keyName(c.u.heldKey).c_str(), static_cast<int>(std::lround(c.u.heldVel * 127.f)));
    textR(c, rx, cy - T(11) * 0.68f, b, c.P.accentText, T(11), false);
  }
}
void caps(Ctx& c, float x, float cy, const char* s) { capsText(c, x, cy - T(10) * 0.62f, s, c.P.textFaint, T(10), M(0.8f)); }
}  // namespace

bool drawer(Ctx& c) {
  const float dw = drawerW(c.u);
  const float dh = drawerH(c.u);
  c.u.devDrawerDrawn = dh;
  if (dh < 1) return false;
  const float y = kPlayY - dh, h = kDrawerH;  // the drawer's own layout, slid down by what is not yet out
  drawerDev().rect = ImVec4(c.p(0, y).x, c.p(0, y).y, c.p(dw, kPlayY).x, c.p(dw, kPlayY).y);
  // a soft shadow above its top edge: it slides over the view
  for (int i = 0; i < 6; ++i) c.dl->AddRectFilled(c.p(0, y - 6 + i), c.p(dw, y - 5 + i), IM_COL32(0, 0, 0, 8 + i * 8));
  ImGui::PushClipRect(c.p(0, y), c.p(dw, kPlayY), true);
  panelRect(c, 0, y, dw, dh, barBg(c));
  c.dl->AddRectFilled(c.p(0, y), c.p(dw, y + 1.5f), c.P.lineHi);  // its edge
  Took took;
  gTook = &took;
  const float hx = 20;
  char octTxt[16];
  std::snprintf(octTxt, sizeof octTxt, "%s", ptext(c, G_TrigOctave).c_str());
  // a header line (the handle, the name, the note played), then the wheels, the steppers, the keyboard
  handle(c, dw, y);
  caps(c, hx, y + 15, "Keyboard");
  playedText(c, dw - 20, y + 15);
  const float by = y + 28, bb = y + h - 12;  // the body
  wellLook(c, hx, by, 104, bb - by);
  wheel(c, G_PitchWheel, hx + 15, by + 7, 30, bb - by - 31, true);
  wheel(c, G_ModWheel, hx + 59, by + 7, 30, bb - by - 31, false);
  const float fx = hx + 104 + 16, fw = 196, fh = 24, gap = (bb - by - fh * 3) * 0.5f;
  for (int i = 0; i < 3; ++i) fieldBg(c, fx, by + (fh + gap) * i, fw, fh);
  stepField(c, G_BendUp, fx, by, fw, fh, "Bend up");
  stepField(c, G_BendDown, fx, by + fh + gap, fw, fh, "Bend down");
  stepField(c, G_TrigOctave, fx, by + (fh + gap) * 2, fw, fh, "Trigger octave", octTxt);
  if (ImGui::IsMouseHoveringRect(c.p(fx, by + (fh + gap) * 2), c.p(fx + fw - M(44), by + (fh + gap) * 2 + fh)))
    c.a.setHint("Trigger octave: its twelve keys pick the arpeggiator pattern or phrase while playing (drag, the wheel, or minus and plus)");
  keyboard(c, fx + fw + 16, by - 9, dw - (fx + fw + 16) - 20, bb - by + 9);
  if (drawerReport()) drawerDev().gap = rectOf(c.p(fx + fw + 8, by + 30), c.p(fx + fw + 8, by + 30));
  // the drawer owns the pointer over all of it (and drawFrame hides it from what lies under it)
  ImGui::SetCursorScreenPos(c.p(0, y));
  ImGui::InvisibleButton("##drawerbg", c.F.sz(dw, dh));
  tookItem();
  gTook = nullptr;
  ImGui::PopClipRect();
  if (drawerReport()) {
    DrawerDev& dd = drawerDev();
    dd.hot = took.hot, dd.pop = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), dd.at = edzoomui::toScreenHere(ImGui::GetIO().MousePos), dd.screen = rectOf(c.p(0, y), c.p(dw, kPlayY));
  }
  return took.own;
}

}  // namespace gl::packs::plasma::ui
