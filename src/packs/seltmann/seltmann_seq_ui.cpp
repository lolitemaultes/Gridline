// Gridline — "seltmann" pack: the sequence lane (J-149). After a drop the
// regions play in order, A B C D ...; the lane shows every step as a block as
// long as it plays, in its region's colour. Drag a block to a new place (a
// caret shows where it lands; Alt+Left / Alt+Right move the selected one);
// right-click a block, a slice in the waveform or a pad to split its region
// (1/2, 1/3, ... : the first piece keeps the letter, later letters move on) or
// to delete it (it joins its neighbour, never past the chopped region). Every
// edit is one undo step through the plugin state. J-185: the same menu pitches the
// region and starts the sequence on a cell; the pitch readouts on the pads and in the waveform.
// J-214: the sequence is its own layer: a block's right edge sets that step's own length (the
// region stays; the next step follows right after it; the loop fills up to the bar line with a
// rest). The cuts between regions are resized in the waveform.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "seltmann_ui.h"

namespace gl::packs::seltmann::sui {

using theme::pal;

namespace {
const int kSplitParts[7] = {2, 3, 4, 5, 6, 7, 8};
}

// ------------------------------------------------------------------ region edits
void markEdited(Model& m) {
  if (m.preset.empty() && m.pattern.name == "Straight") m.pattern.name = kEditedName;
}

void splitRegionUi(App& a, const PluginTarget& t, Ed& e, int region, int parts) {
  Model nm = e.m;
  if (!splitRegion(nm, region, parts)) {
    a.notify(i18n::tr("This region is too short to split that many times"), true);
    return;
  }
  char lab[64];
  std::snprintf(lab, sizeof lab, "%s %s 1/%d", i18n::tr("Split"), regionName(region).c_str(), parts);
  commit(a, t, e, nm, lab);
  e.selSlice = region;
  e.selStep = -1;
}

void deleteRegionUi(App& a, const PluginTarget& t, Ed& e, int region) {
  Model nm = e.m;
  if (!mergeRegion(nm, region)) return;
  char lab[64];
  std::snprintf(lab, sizeof lab, "%s %s", i18n::tr("Delete region"), regionName(region).c_str());
  commit(a, t, e, nm, lab);
  e.selSlice = std::clamp(region == 0 ? 0 : region - 1, 0, std::max(0, static_cast<int>(nm.slices.size()) - 1));
  e.selStep = -1;
}

// ------------------------------------------------------------------ J-185 slice pitch, start here
void slicePitchUi(App& a, const PluginTarget& t, Ed& e, int slice, float semis, uint64_t mergeKey) {
  Model nm = e.m;
  if (!setSlicePitch(nm, slice, semis)) return;
  commit(a, t, e, nm, i18n::N_("Slice pitch"), mergeKey);
}

void stepPitchUi(App& a, const PluginTarget& t, Ed& e, int step, float semis, uint64_t mergeKey) {
  Model nm = e.m;
  if (!setStepPitch(nm, step, semis)) return;
  commit(a, t, e, nm, i18n::N_("Step pitch"), mergeKey);
}

namespace {
// The wheel over the hovered item: the new pitch from `from` (whole semitones; Shift: 0.1), or NAN
// when no notch landed. `who` tells one item from another (a run of notches on one is one undo step).
float wheelNotches(Ed& e, int who, float from) {
  if (!ImGui::IsItemHovered()) return NAN;
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
  const ImGuiIO& io = ImGui::GetIO();
  float w = io.MouseWheel;
  if (w == 0 && io.KeyShift) w = io.MouseWheelH;  // (Shift turns the wheel sideways on some systems)
  if (w == 0) return NAN;
  e.wheelAcc += w;
  const int notches = static_cast<int>(e.wheelAcc);
  if (notches == 0) return NAN;
  e.wheelAcc -= static_cast<float>(notches);
  // J-208: a run of notches on one item is one undo step; a pause (or another item) starts the next
  const double now = ImGui::GetTime();
  if (e.wheelSlice != who || e.wheelLast < 0 || now - e.wheelLast > 0.6) ++e.gesture;
  e.wheelSlice = who;
  e.wheelLast = now;
  const float step = io.KeyShift ? 0.1f : 1.0f;
  float nv = from + step * static_cast<float>(notches);
  if (!io.KeyShift) nv = std::round(nv);  // plain notches land on whole semitones
  return std::clamp(nv, -kSlicePitchRange, kSlicePitchRange);
}
}  // namespace

void wheelPitch(App& a, const PluginTarget& t, Ed& e, int slice) {
  if (slice < 0 || slice >= static_cast<int>(e.m.slices.size())) return;
  const float nv = wheelNotches(e, slice, e.m.slices[static_cast<size_t>(slice)].pitch);
  if (std::isfinite(nv)) slicePitchUi(a, t, e, slice, nv, gestureKey(0x5E185A0000000000ull, e, slice));
}

void wheelStepPitch(App& a, const PluginTarget& t, Ed& e, int step) {
  if (step < 0 || step >= static_cast<int>(e.m.pattern.steps.size())) return;
  const float nv = wheelNotches(e, 0x100000 + step, e.m.pattern.steps[static_cast<size_t>(step)].pitch);
  if (std::isfinite(nv)) stepPitchUi(a, t, e, step, nv, gestureKey(0x5E213A0000000000ull, e, step));
}

std::string pitchText(float v) {
  if (std::fabs(v) < 0.005f) return "0";
  char b[16];
  if (std::fabs(v - std::round(v)) < 0.005f) std::snprintf(b, sizeof b, "%+d", static_cast<int>(std::lround(v)));
  else if (std::fabs(v * 10 - std::round(v * 10)) < 0.05f) std::snprintf(b, sizeof b, "%+.1f", static_cast<double>(v));
  else std::snprintf(b, sizeof b, "%+.2f", static_cast<double>(v));
  return b;
}

float pitchChip(App& a, const PluginTarget& t, Ed& e, const char* id, ImVec2 p0, float h, float maxW, int slice, bool units) {
  const auto& P = pal();
  if (slice < 0 || slice >= static_cast<int>(e.m.slices.size())) return 0;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float v = e.m.slices[static_cast<size_t>(slice)].pitch;
  const bool zero = std::fabs(v) < 0.005f;
  const float fs = theme::kFontLabel;
  std::string s = pitchText(v);
  auto widthOf = [&](const std::string& x) { return std::ceil(a.fontMono->CalcTextSizeA(fs, 1e9f, 0, x.c_str()).x) + 8; };
  if (units && widthOf(s + " st") <= maxW) s += " st";
  const float w = widthOf(s);
  if (w > maxW) return 0;
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemActivated()) {
    e.pitchDrag = slice;
    e.pitchFrom = v;
    e.pitchAcc = 0;
    ++e.gesture;
  }
  if (ac && e.pitchDrag == slice && io.MouseDelta.y != 0) {
    float nv;
    if (io.KeyShift) {  // fine: 0.05 semitone per pixel
      nv = v - io.MouseDelta.y * 0.05f;
      e.pitchFrom = nv;
      e.pitchAcc = 0;
    } else {  // a semitone per 8 px
      e.pitchAcc -= io.MouseDelta.y;
      nv = e.pitchFrom + std::round(e.pitchAcc / 8.0f);
    }
    slicePitchUi(a, t, e, slice, nv, gestureKey(0x5E18500000000000ull, e, slice));
  }
  if (ImGui::IsItemDeactivated()) e.pitchDrag = -1;
  if (hv && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !zero) slicePitchUi(a, t, e, slice, 0);
  wheelPitch(a, t, e, slice);
  if (hv && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    e.selSlice = slice;
    openRegionMenu(e, slice, -1);
  }
  if (hv || ac) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  if (hv && !ac) {
    char b[240];
    std::snprintf(b, sizeof b, "%s %s: %s %s\n%s", i18n::tr("Pitch of region"), regionName(slice).c_str(), pitchText(v).c_str(), i18n::tr("semitones"),
                  i18n::tr("Drag up or down: semitones (Shift: finer). Wheel: one semitone. Double-click: back to 0."));
    ui::Tooltip(b);
  }
  // paint: quiet at 0, lit when the slice is pitched
  const ImVec2 q1(p0.x + w, p0.y + h);
  const ImU32 face = zero ? theme::withAlpha(P.well, hv || ac ? 0.95f : 0.75f) : theme::withAlpha(accent(), hv || ac ? 1.0f : 0.88f);
  dl->AddRectFilled(p0, q1, face, theme::kRadiusSm);
  if (hv || ac) dl->AddRect(p0, q1, theme::withAlpha(P.paper, 0.8f), theme::kRadiusSm, 0, 1.0f);
  const ImU32 ink = zero ? P.textDim : theme::contrastText(accent());
  textC(dl, a.fontMono, fs, ImVec2((p0.x + q1.x) * 0.5f, (p0.y + q1.y) * 0.5f), ink, s.c_str());
  return w;
}

void stepBeatsUi(App& a, const PluginTarget& t, Ed& e, int step, double beats, uint64_t mergeKey) {
  Model nm = e.m;
  if (!setStepBeats(nm, step, beats)) return;
  commit(a, t, e, nm, i18n::N_("Step length"), mergeKey);
}

void cutDragBegin(Ed& e, int cut) {
  e.cutBase = e.m;
  e.cutDrag = cut;
  ++e.gesture;
}

void cutDragTo(App& a, const PluginTarget& t, Ed& e, int cut, double frame) {
  const Model& base = e.cutBase;
  const int n = static_cast<int>(base.slices.size());
  if (e.cutDrag != cut || cut <= 0 || cut >= n || cut >= static_cast<int>(e.m.slices.size())) return;
  SamplePtr smp = sampleOf(e);
  auto f = static_cast<int64_t>(std::llround(std::clamp(frame, 0.0, static_cast<double>(std::max<int64_t>(1, base.srcFrames)))));
  if (!ImGui::GetIO().KeyAlt) f = snapFrame(base, smp.get(), f, base.chop.snap);
  Model nm = base;
  if (!resizeCut(nm, cut, f)) nm = base;
  if (nm.slices[static_cast<size_t>(cut)].start == e.m.slices[static_cast<size_t>(cut)].start && nm.pattern == e.m.pattern) return;
  commit(a, t, e, nm, i18n::N_("Resize a slice"), gestureKey(0x5E185C0000000000ull, e, cut));
}

void startHereUi(App& a, const PluginTarget& t, Ed& e, int step, bool keepRest) {
  Model nm = e.m;
  if (!startSequenceAt(nm, step, keepRest)) return;
  commit(a, t, e, nm, keepRest ? i18n::N_("Start the sequence here") : i18n::N_("Start here, leave out the steps before"));
  e.selStep = 0;
}

void regionMenuItems(App& a, const PluginTarget& t, Ed& e, int region) {
  const int n = static_cast<int>(e.m.slices.size());
  if (region < 0 || region >= n) return;
  const std::string L = regionName(region);
  char b[96];
  std::snprintf(b, sizeof b, "%s %s", i18n::tr("Split"), L.c_str());
  if (ImGui::BeginMenu(b)) {
    for (int parts : kSplitParts) {
      Model probe = e.m;
      const bool ok = splitRegion(probe, region, parts);
      char it[64];
      std::snprintf(it, sizeof it, "1/%d   %s %d %s", parts, i18n::tr("into"), parts, i18n::tr("equal parts"));
      if (ImGui::MenuItem(it, nullptr, false, ok)) splitRegionUi(a, t, e, region, parts);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        std::string tip = L + " " + i18n::tr("keeps the first part; the next parts become") + " ";
        for (int p = 1; p < parts; ++p) tip += regionName(region + p) + (p + 1 < parts ? " " : "");
        tip += std::string(".\n") + i18n::tr("Later letters move on.");
        if (!ok) tip += std::string("\n") + i18n::tr("Too short for that many parts.");
        ui::Tooltip(tip.c_str());
      }
    }
    ImGui::EndMenu();
  }
  if (n > 1) std::snprintf(b, sizeof b, "%s %s   (%s %s)", i18n::tr("Delete"), L.c_str(), i18n::tr("joins"), regionName(region == 0 ? 1 : region - 1).c_str());
  else std::snprintf(b, sizeof b, "%s %s", i18n::tr("Delete"), L.c_str());
  if (ImGui::MenuItem(b, "Del", false, n > 1)) deleteRegionUi(a, t, e, region);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ui::Tooltip(n > 1 ? i18n::tr("The region joins its neighbour (the first one joins the next). The chopped region stays the same.")
                      : i18n::tr("The last region cannot be deleted."));
}

void openRegionMenu(Ed& e, int region, int step) {
  e.menuRegion = region;
  e.menuStep = step;
  e.menuWant = true;
}

void drawRegionMenu(App& a, const PluginTarget& t, Ed& e) {
  e.menuPts.clear();
  if (e.menuWant) {
    ImGui::OpenPopup("##sregion");
    e.menuWant = false;
  }
  if (!ImGui::BeginPopup("##sregion")) return;
  auto note = [&](const char* tag) {  // (trace: where the item is, for scripted runs)
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    e.menuPts.emplace_back(tag, ImVec2((r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f));
  };
  const int n = static_cast<int>(e.m.slices.size());
  const int ns = static_cast<int>(e.m.pattern.steps.size());
  const int region = e.menuRegion;
  if (region < 0 || region >= n) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  {
    char b[96];
    const double secs = static_cast<double>(e.m.slices[static_cast<size_t>(region)].end - e.m.slices[static_cast<size_t>(region)].start) /
                        std::max(1.0, e.m.srcRate);
    if (e.menuStep >= 0 && e.menuStep < ns)
      std::snprintf(b, sizeof b, "%s %d  \xC2\xB7  %s %s  \xC2\xB7  %.3f s", i18n::tr("Step"), e.menuStep + 1, i18n::tr("region"), regionName(region).c_str(), secs);
    else std::snprintf(b, sizeof b, "%s %s  \xC2\xB7  %.3f s", i18n::tr("Region"), regionName(region).c_str(), secs);
    ImGui::TextDisabled("%s", b);
  }
  ImGui::Separator();
  regionMenuItems(a, t, e, region);
  // J-185: the pitch: from a block of the sequence, that step's own pitch (the region's other steps
  // keep theirs) and the region's (every step of it); from a pad or the waveform, the region's
  ImGui::Separator();
  {
    const int nsP = static_cast<int>(e.m.pattern.steps.size());
    const bool fromStep = e.menuStep >= 0 && e.menuStep < nsP;
    const int stepI = e.menuStep;
    auto pitchMenu = [&](const char* title, float pv, const std::function<void(float)>& set, const char* tip) {
      char b[96];
      std::snprintf(b, sizeof b, "%s   %s %s", i18n::tr(title), pitchText(pv).c_str(), i18n::tr("st"));
      if (ImGui::BeginMenu(b)) {
        struct Item {
          const char* label;
          float delta;
        };
        const Item items[4] = {{i18n::N_("Up a semitone"), 1}, {i18n::N_("Down a semitone"), -1}, {i18n::N_("Up an octave"), 12}, {i18n::N_("Down an octave"), -12}};
        for (const auto& it : items) {
          const float nv = std::clamp(pv + it.delta, -kSlicePitchRange, kSlicePitchRange);
          if (ImGui::MenuItem(i18n::tr(it.label), nullptr, false, nv != pv)) set(nv);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Reset pitch"), nullptr, false, std::fabs(pv) >= 0.005f)) set(0);
        ImGui::EndMenu();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !ImGui::IsPopupOpen(b)) ui::Tooltip(i18n::tr(tip));
    };
    if (fromStep) {
      pitchMenu(i18n::N_("Step pitch"), e.m.pattern.steps[static_cast<size_t>(stepI)].pitch,
                [&](float v) { stepPitchUi(a, t, e, stepI, v); },
                i18n::N_("This step only: the region's other steps in the sequence keep their pitch. Also: the wheel over the block."));
      note("pitch");
      pitchMenu(i18n::N_("Region pitch"), e.m.slices[static_cast<size_t>(region)].pitch,
                [&](float v) { slicePitchUi(a, t, e, region, v); },
                i18n::N_("Every step of this region, its pad and its key. Also: drag the pitch readout on its pad up or down."));
      note("regionpitch");
    } else {
      pitchMenu(i18n::N_("Pitch"), e.m.slices[static_cast<size_t>(region)].pitch, [&](float v) { slicePitchUi(a, t, e, region, v); },
                i18n::N_("Also: drag the pitch readout on a pad or a region up or down, or turn the wheel over it."));
      note("pitch");
    }
  }
  // J-185: the sequence starts here (from this step; from a pad or the waveform: the region's first step)
  ImGui::Separator();
  {
    const int ns0 = static_cast<int>(e.m.pattern.steps.size());
    const int from = e.menuStep >= 0 && e.menuStep < ns0 ? e.menuStep : firstStepOf(e.m, region);
    const bool can = from > 0;
    if (ImGui::MenuItem(i18n::tr("Start the sequence here"), nullptr, false, can)) startHereUi(a, t, e, from, true);
    note("start");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ui::Tooltip(from < 0   ? i18n::tr("This region is not in the sequence.")
                  : from == 0 ? i18n::tr("The sequence already starts here.")
                              : i18n::tr("The sequence plays from here on; the steps before move to the end, so the loop keeps its length."));
    if (ImGui::MenuItem(i18n::tr("Start here, leave out the steps before"), nullptr, false, can)) startHereUi(a, t, e, from, false);
    note("startcut");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ui::Tooltip(i18n::tr("The sequence plays from here to its end; the steps before are taken out (Undo or Reset order brings them back)."));
  }
  const int st = e.menuStep;
  if (st >= 0 && st < ns) {
    ImGui::Separator();
    Model nm = e.m;
    Step& s = nm.pattern.steps[static_cast<size_t>(st)];
    if (ImGui::MenuItem(i18n::tr("Play backwards"), nullptr, s.reverse)) {
      s.reverse = !s.reverse;
      markEdited(nm);
      commit(a, t, e, nm, i18n::N_("Reverse a step"));
    }
    if (ImGui::MenuItem(i18n::tr("Duplicate step")) && ns < kMaxSteps) {
      nm.pattern.steps.insert(nm.pattern.steps.begin() + st + 1, s);
      markEdited(nm);
      commit(a, t, e, nm, i18n::N_("Duplicate a step"));
      e.selStep = st + 1;
    }
    note("dup");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ui::Tooltip(i18n::tr("A copy of this step right after it, with its length and pitch. Each copy can then have its own length."));
    // J-214: the step's own length in the sequence (its right edge in the lane)
    const bool own = st < static_cast<int>(e.m.pattern.steps.size()) && e.m.pattern.steps[static_cast<size_t>(st)].beats > 0;
    if (ImGui::MenuItem(i18n::tr("Reset length"), nullptr, false, own)) stepBeatsUi(a, t, e, st, 0);
    note("resetlen");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ui::Tooltip(own ? i18n::tr("The step lasts its default length in the sequence again.")
                  : i18n::tr("Drag the block's right edge to give this step its own length in the sequence."));
    if (ImGui::MenuItem(i18n::tr("Remove from the sequence"), nullptr, false, ns > 1)) {
      nm.pattern.steps.erase(nm.pattern.steps.begin() + st);
      markEdited(nm);
      commit(a, t, e, nm, i18n::N_("Remove a step"));
      e.selStep = -1;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ui::Tooltip(i18n::tr("Only this step leaves the sequence; the region stays (use Delete to join regions)."));
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Reset to A B C D order"), nullptr, false, !isNaturalOrder(e.m))) {
    Model nm = e.m;
    nm.preset.clear();
    naturalPattern(nm);
    commit(a, t, e, nm, i18n::N_("Reset the order"));
    e.selStep = -1;
  }
  ImGui::EndPopup();
}

// ------------------------------------------------------------------ the lane
void drawSequence(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Model& m = e.m;
  const int ns = static_cast<int>(m.pattern.steps.size());
  const int nSl = static_cast<int>(m.slices.size());
  const int bpb = std::max(1, m.grid.beatsPerBar);
  // J-214: one cycle (the rest up to the bar line included) and where the steps end. While a length
  // drag shortens a step the lane keeps its scale, so the edge stays under the pointer.
  const double cycle = std::max(1e-9, cycleBeats(m));
  const double stepsEnd = m.pattern.stepsBeats();
  const double beats = e.lenDrag >= 0 ? std::max(cycle, e.lenShown) : cycle;
  const bool natural = isNaturalOrder(m);
  char cap[160];
  {
    const double bars = cycle / bpb;
    char bb[24];
    std::snprintf(bb, sizeof bb, std::fabs(bars - std::round(bars)) < 1e-6 ? "%.0f" : "%.2f", bars);
    std::snprintf(cap, sizeof cap, "%s  \xC2\xB7  %d %s  \xC2\xB7  %s %s", natural ? i18n::tr("IN ORDER") : i18n::tr("YOUR ORDER"), ns,
                  ns == 1 ? i18n::tr("STEP") : i18n::tr("STEPS"), bb, std::fabs(bars - 1) < 1e-9 ? i18n::tr("BAR") : i18n::tr("BARS"));
    // J-185: where the sequence starts when that is not the first region
    const int sr = sequenceStartRegion(m);
    if (sr > 0) {
      const std::string c = cap;
      std::snprintf(cap, sizeof cap, "%s  \xC2\xB7  %s %s", c.c_str(), i18n::tr("STARTS AT"), regionName(sr).c_str());
    }
  }
  // J-152: the lane sits right under the waveform and shares its inner width (in order, A plays
  // under A). A 24 px strip between the two carries the title, the caption, the hint and Reset.
  const float stripH = 24;
  {
    const float cy = std::round(p0.y + stripH * 0.5f);
    dl->AddRectFilled(ImVec2(p0.x + 8, cy - 5), ImVec2(p0.x + 10, cy + 4), accent(), 1);
    ImFont* f = a.fontMonoBold ? a.fontMonoBold : a.fontMono;
    const char* title = i18n::tr("SEQUENCE");
    dl->AddText(f, 10.5f, ImVec2(p0.x + 16, cy - 7), P.text, title);
    const float tw = f->CalcTextSizeA(10.5f, 1e9f, 0, title).x;
    dl->AddText(a.fontMono, 10.5f, ImVec2(std::round(p0.x + 16 + tw + 12), cy - 7), P.textFaint, cap);
    const float btnW = 116, btnH = 20;
    const float bx = p1.x - btnW;
    if (button("##sseqreset", ImVec2(bx, cy - btnH * 0.5f), ImVec2(btnW, btnH), i18n::N_("Reset order"), false,
               natural ? i18n::N_("The sequence already plays every region in order")
                       : i18n::N_("Play every region once, in order (A B C D ...), each as long as its region"),
               static_cast<int>(ui::Icon::Loop)) &&
        !natural) {
      Model nm = m;
      nm.preset.clear();
      naturalPattern(nm);
      commit(a, t, e, nm, i18n::N_("Reset the order"));
      e.selStep = -1;
    }
    textR(dl, a.fontRegular, theme::kFontLabel, ImVec2(bx - 12, cy - 7), P.textFaint,
          i18n::tr("Drag a block to move it, its right edge to set its length. Right-click: more"));
  }
  // the well
  const float lx0 = p0.x, lx1 = p1.x, ly0 = p0.y + stripH, rulerH = 13, ly1 = p1.y - rulerH;
  const ImVec2 w0(lx0, ly0), w1(lx1, p1.y);
  pui::displayFrame(dl, w0, w1);
  e.seq0 = ImVec2(lx0, ly0), e.seq1 = ImVec2(lx1, ly1);
  e.seqCentres.clear();
  if (ns == 0) return;
  const float bx0 = lx0 + 8, bx1 = lx1 - 8, by0 = ly0 + 5, by1 = ly1 - 1;
  const float bw = bx1 - bx0;
  auto X = [&](double beat) { return bx0 + static_cast<float>(beat / beats) * bw; };
  // block edges (beats)
  std::vector<float> edge(static_cast<size_t>(ns) + 1);
  {
    double u = 0;
    for (int i = 0; i < ns; ++i) {
      edge[static_cast<size_t>(i)] = X(u);
      u += stepBeats(m.pattern, i);
    }
    edge[static_cast<size_t>(ns)] = X(stepsEnd);
  }
  // ruler: bars and beats under the blocks
  {
    const float ry = ly1 + 1;
    const float beatPx = bw / static_cast<float>(beats);
    for (int b = 0; b <= static_cast<int>(std::floor(beats + 1e-9)); ++b) {
      const bool bar = b % bpb == 0;
      if (!bar && beatPx < 6) continue;
      const float x = std::round(X(b)) + 0.5f;
      dl->AddLine(ImVec2(x, ry), ImVec2(x, ry + (bar ? 5.0f : 3.0f)), theme::withAlpha(P.textDim, bar ? 0.7f : 0.35f), 1);
      if (bar && b < beats - 1e-9 && bpb * beatPx >= 18) {
        char nb[16];
        std::snprintf(nb, sizeof nb, "%d", b / bpb + 1);
        dl->AddText(a.fontMono, theme::kFontLabel - 1, ImVec2(x + 3, ry + 1), P.textFaint, nb);
      }
    }
  }
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mp = io.MousePos;
  bool laneHovered = ImGui::IsMouseHoveringRect(w0, w1, true) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  // blocks
  for (int i = 0; i < ns; ++i) {
    const Step& s = m.pattern.steps[static_cast<size_t>(i)];
    const float x0 = edge[static_cast<size_t>(i)], x1 = edge[static_cast<size_t>(i) + 1];
    const ImVec2 q0(std::round(x0) + 1, by0), q1(std::max(std::round(x0) + 3, std::round(x1) - 1), by1);
    const int k = stepSlice(m.pattern, s, nSl);
    // (J-214: in the rest after the last step no block lights)
    const bool inRest = lv.patternPos >= 0 && lv.patternPos * cycle > stepsEnd + 1e-6;
    const bool playing = i == lv.step && lv.patternRunning && !inRest, sel = i == e.selStep, dragged = i == e.seqDrag;
    ImGui::PushID(0x5E0000 + i);
    ImGui::SetCursorScreenPos(q0);
    ImGui::SetNextItemAllowOverlap();  // J-185: the edges between blocks resize the regions (in order)
    ImGui::InvisibleButton("##blk", ImVec2(q1.x - q0.x, q1.y - q0.y));
    const bool hv = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
      e.seqPress = i;
      e.selStep = i;
      if (k >= 0) {
        e.selSlice = k;
        auditionStart(a, t, e, k);
      }
    }
    if (ImGui::IsItemActive() && e.seqPress == i && e.seqDrag < 0 && ImGui::IsMouseDragging(0, 5.0f) && ns > 1) {
      e.seqDrag = i;
      auditionStop(a, t, e);
    }
    if (ImGui::IsItemDeactivated()) auditionStop(a, t, e);
    // The wheel over a block steps that step's own pitch (Shift: 0.1), not its region's: the region's
    // other steps keep theirs. The wheel elsewhere scrolls as before.
    if (k >= 0 && e.seqDrag < 0) wheelStepPitch(a, t, e, i);
    if (hv && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && k >= 0) {
      e.selStep = i;
      e.selSlice = k;
      openRegionMenu(e, k, i);
    }
    if (hv && !ImGui::IsItemActive() && e.seqDrag < 0) {
      char b[320];
      const std::string pt = k >= 0 ? pitchText(m.slices[static_cast<size_t>(k)].pitch + s.pitch) : std::string("0");
      std::snprintf(b, sizeof b, "%s %d: %s %s  \xC2\xB7  %.4g %s%s  \xC2\xB7  %s %s %s\n%s", i18n::tr("Step"), i + 1, i18n::tr("region"),
                    k >= 0 ? regionName(k).c_str() : "-", stepBeats(m.pattern, i), i18n::tr("beats"),
                    s.beats > 0 ? (std::string(" (") + i18n::tr("its own length") + ")").c_str() : "", i18n::tr("pitch"), pt.c_str(), i18n::tr("st"),
                    i18n::tr("Click to hear it. Drag to move it. Wheel: this step's pitch (Shift: finer). Right-click: start here, pitch, split or delete the region."));
      ui::Tooltip(b);
    }
    if (hv && e.seqDrag < 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopID();
    // paint
    // J-152: a quiet tinted face with the hue along its foot; only the playing step lights up fully
    if (s.src == kRest || k < 0) {
      dl->AddRect(q0, q1, theme::withAlpha(P.line, 1.0f), theme::kRadius);
    } else {
      const ImU32 face = playing ? regionColor(k) : regionFace(k, hv ? 0.42f : sel ? 0.36f : 0.28f);
      skin::raisedFace(dl, q0, q1, dragged ? theme::withAlpha(face, 0.3f) : face, theme::kRadius);
      if (!playing && !dragged)
        dl->AddRectFilled(ImVec2(q0.x + 1, q1.y - 3), ImVec2(q1.x - 1, q1.y - 1), regionColor(k, 0.95f), 1.0f, ImDrawFlags_RoundCornersBottom);
      if (s.reverse)
        dl->AddRectFilledMultiColor(q0, q1, theme::withAlpha(P.ink, 0.0f), theme::withAlpha(P.ink, 0.28f), theme::withAlpha(P.ink, 0.28f),
                                    theme::withAlpha(P.ink, 0.0f));
      const ImU32 ink = theme::withAlpha(playing ? theme::contrastText(regionColor(k)) : P.text, dragged ? 0.4f : 1.0f);
      const float cw = q1.x - q0.x;
      const std::string L = regionName(k);
      if (cw >= 11) {
        const float fs = cw >= 26 ? 15.0f : 12.0f;
        textC(dl, a.fontBold, fs, ImVec2((q0.x + q1.x) * 0.5f, q0.y + (q1.y - q0.y) * (cw >= 26 ? 0.42f : 0.5f)), ink, L.c_str());
      }
      if (cw >= 30) {
        std::string marks;
        if (s.reverse) marks += "R ";
        // J-208: the pitch the block plays at (its region's pitch and the step's own)
        const float bp = m.slices[static_cast<size_t>(k)].pitch + s.pitch;
        if (std::fabs(bp) >= 0.005f) marks += pitchText(bp) + " ";
        if (s.stutter > 1) marks += "x" + std::to_string(s.stutter) + " ";
        if (!marks.empty()) {
          marks.pop_back();
          textC(dl, a.fontMono, theme::kFontLabel - 1, ImVec2((q0.x + q1.x) * 0.5f, q1.y - 8), theme::withAlpha(ink, 0.85f), marks.c_str());
        }
      }
    }
    // J-214: a step with its own length carries a small corner mark
    if (s.beats > 0 && q1.x - q0.x >= 12 && !dragged)
      dl->AddTriangleFilled(ImVec2(q1.x - 7, q0.y + 1), ImVec2(q1.x - 1, q0.y + 1), ImVec2(q1.x - 1, q0.y + 7), theme::withAlpha(accent(), 0.95f));
    if (playing && !dragged) dl->AddRect(ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), P.paper, theme::kRadius + 1, 0, 2.0f);
    else if (sel && !dragged) dl->AddRect(ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), accent(), theme::kRadius + 1, 0, 2.0f);
    e.seqCentres.push_back(ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f));
  }
  // J-214: the rest up to the bar line (once a step has its own length): the loop always restarts
  // on a bar line; lengthen a step or add one to fill it
  {
    const float r0 = std::round(edge[static_cast<size_t>(ns)]) + 1, r1 = std::round(X(cycle)) - 1;
    if (stepsEnd < cycle - 1e-6 && r1 - r0 >= 3) {
      const ImVec2 q0(r0, by0), q1(r1, by1);
      dl->PushClipRect(q0, q1, true);
      for (float hx = q0.x - (q1.y - q0.y); hx < q1.x; hx += 7)
        dl->AddLine(ImVec2(hx, q1.y), ImVec2(hx + (q1.y - q0.y), q0.y), theme::withAlpha(P.textDim, 0.16f), 1);
      dl->PopClipRect();
      dl->AddRect(q0, q1, theme::withAlpha(P.line, 1.0f), theme::kRadius);
      const char* rl = i18n::tr("REST");
      if (a.fontMono->CalcTextSizeA(theme::kFontLabel - 1, 1e9f, 0, rl).x + 8 <= q1.x - q0.x)
        textC(dl, a.fontMono, theme::kFontLabel - 1, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), P.textFaint, rl);
      if (e.lenDrag < 0 && e.seqDrag < 0 && ImGui::IsMouseHoveringRect(q0, q1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        char b[240];
        std::snprintf(b, sizeof b, "%s: %.4g %s\n%s", i18n::tr("Rest to the bar line"), cycle - stepsEnd, i18n::tr("beats"),
                      i18n::tr("The sequence always lasts whole bars. Lengthen a step or add one to fill the rest."));
        ui::Tooltip(b);
      }
    }
  }
  // J-214: the right edge of a block is that step's length in the sequence; its region stays (the cuts
  // are resized in the waveform). Drag: snaps to the chop's grid (a sixteenth at most; Shift: a
  // quarter of that; Alt: free). Double-click: its default length again. One undo step per drag.
  e.laneLenPts.clear();
  if (e.seqDrag < 0) {
    const double cell = chopCellBeats(m.chop, bpb);
    const double grid = m.chop.mode == ChopGrid && cell > 0 && cell <= 0.25 + 1e-9 ? cell : 0.25;
    for (int i = 0; i < ns; ++i) {
      const float x = std::round(edge[static_cast<size_t>(i) + 1]);
      const float wl = edge[static_cast<size_t>(i) + 1] - edge[static_cast<size_t>(i)];
      const float hw = std::min(4.0f, wl * 0.2f);
      if (hw < 2 && e.lenDrag != i) continue;
      ImGui::PushID(0x5E2140 + i);
      ImGui::SetCursorScreenPos(ImVec2(x - std::max(2.0f, hw), by0));
      ImGui::InvisibleButton("##slen", ImVec2(2 * std::max(2.0f, hw), by1 - by0));
      const bool h = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
      if (ImGui::IsItemActivated()) {
        e.lenDrag = i;
        e.lenFrom = stepBeats(m.pattern, i);
        e.lenStart = stepStart(m.pattern, i, 0);
        e.lenShown = beats;
        e.lenPx = bw / static_cast<float>(beats);
        e.lenX0 = mp.x;
        e.selStep = i;
        ++e.gesture;
      }
      if (ac && e.lenDrag == i && ImGui::IsMouseDragging(0, 1)) {
        const double end = e.lenStart + e.lenFrom + static_cast<double>(mp.x - e.lenX0) / std::max(1e-3f, e.lenPx);
        double len = end - e.lenStart;
        if (!io.KeyAlt) {
          const double g = io.KeyShift ? grid * 0.25 : grid;
          len = std::max(g, std::round(end / g) * g - e.lenStart);
        }
        stepBeatsUi(a, t, e, i, std::clamp(len, kMinStepBeats, kMaxStepBeats), gestureKey(0x5E214B0000000000ull, e, i));
      }
      if (ImGui::IsItemDeactivated()) e.lenDrag = -1;
      if (h && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) stepBeatsUi(a, t, e, i, 0);
      if (h && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const int k = stepSlice(m.pattern, m.pattern.steps[static_cast<size_t>(i)], nSl);
        if (k >= 0) {
          e.selStep = i;
          e.selSlice = k;
          openRegionMenu(e, k, i);
        }
      }
      if (h || ac) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        dl->AddLine(ImVec2(x, by0 - 2), ImVec2(x, by1 + 2), P.paper, 2.0f);
        dl->AddTriangleFilled(ImVec2(x - 4, by0 - 3), ImVec2(x + 4, by0 - 3), ImVec2(x, by0 + 2), P.paper);
        const bool own = m.pattern.steps[static_cast<size_t>(i)].beats > 0;
        char b[320];
        if (ac)
          std::snprintf(b, sizeof b, "%s %d: %.4g %s", i18n::tr("Step"), i + 1, stepBeats(m.pattern, i), i18n::tr("beats"));
        else
          std::snprintf(b, sizeof b, "%s %d: %.4g %s (%s)\n%s", i18n::tr("Step"), i + 1, stepBeats(m.pattern, i), i18n::tr("beats"),
                        own ? i18n::tr("its own length") : i18n::tr("default length"),
                        i18n::tr("Drag: this step's length in the sequence; its region stays (Shift: finer, Alt: no snap). Double-click: default length."));
        ui::Tooltip(b);
      }
      e.laneLenPts.push_back(ImVec2(x, (by0 + by1) * 0.5f));
      ImGui::PopID();
    }
  }
  // drag to a new place: the caret at the nearest edge, the block under the pointer
  if (e.seqDrag >= 0 && e.seqDrag < ns) {
    int best = 0;
    float bd = 1e9f;
    for (int j = 0; j <= ns; ++j)
      if (std::fabs(edge[static_cast<size_t>(j)] - mp.x) < bd) bd = std::fabs(edge[static_cast<size_t>(j)] - mp.x), best = j;
    e.seqDrop = best;
    const bool noop = best == e.seqDrag || best == e.seqDrag + 1;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    {  // the block, lifted under the pointer
      const Step& s = m.pattern.steps[static_cast<size_t>(e.seqDrag)];
      const int k = stepSlice(m.pattern, s, nSl);
      const float gw = std::clamp(edge[static_cast<size_t>(e.seqDrag) + 1] - edge[static_cast<size_t>(e.seqDrag)], 22.0f, 80.0f);
      const ImVec2 g0(std::round(mp.x - gw * 0.5f), std::round(by0 - 14)), g1(std::round(mp.x + gw * 0.5f), std::round(by0 + (by1 - by0) * 0.5f - 6));
      dl->AddRectFilled(ImVec2(g0.x + 2, g0.y + 4), ImVec2(g1.x + 2, g1.y + 5), IM_COL32(0, 0, 0, 80), theme::kRadius + 1);
      skin::raisedFace(dl, g0, g1, regionColor(k, 0.9f), theme::kRadius);
      dl->AddRect(g0, g1, P.paper, theme::kRadius, 0, 1.5f);
      textC(dl, a.fontBold, 14, ImVec2((g0.x + g1.x) * 0.5f, (g0.y + g1.y) * 0.5f), theme::contrastText(regionColor(k)), regionName(k).c_str());
    }
    if (!noop) {  // where it lands (drawn over the lifted block)
      const float cx = std::round(std::clamp(edge[static_cast<size_t>(best)], bx0 + 1, bx1 - 1));
      dl->AddLine(ImVec2(cx, by0 - 3), ImVec2(cx, by1 + 3), accent(), 2.5f);
      dl->AddTriangleFilled(ImVec2(cx - 5, by1 + 5), ImVec2(cx + 5, by1 + 5), ImVec2(cx, by1 - 1), accent());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) e.seqDrag = -1, e.seqDrop = -1;
    if (!io.MouseDown[0] && e.seqDrag >= 0) {
      Model nm = m;
      const int from = e.seqDrag;
      if (moveStep(nm, from, best)) {
        commit(a, t, e, nm, i18n::N_("Move a step"));
        e.selStep = best > from ? best - 1 : best;
      }
      e.seqDrag = -1, e.seqDrop = -1;
    }
  } else if (!io.MouseDown[0]) {
    e.seqDrag = -1, e.seqPress = -1;
  }
  // playhead
  if (lv.patternPos >= 0 && lv.patternRunning) {
    const float xx = X(lv.patternPos * cycle);
    dl->AddLine(ImVec2(xx, by0 - 2), ImVec2(xx, by1 + 2), P.playhead, 1.5f);
  }
  // keys: Alt+Left / Alt+Right move the selected step, Delete deletes the region under the pointer (or the selected one)
  if (!io.WantTextInput && e.seqDrag < 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    const int s = e.selStep;
    if (s >= 0 && s < ns && io.KeyAlt) {
      int to = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && s > 0) to = s - 1;
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && s + 1 < ns) to = s + 2;
      Model nm = m;
      if (to >= 0 && moveStep(nm, s, to)) {
        commit(a, t, e, nm, i18n::N_("Move a step"));
        e.selStep = to > s ? to - 1 : to;
      }
    }
    if (laneHovered && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
      int target = -1;
      for (int i = 0; i < ns; ++i)
        if (mp.x >= edge[static_cast<size_t>(i)] && mp.x < edge[static_cast<size_t>(i) + 1]) target = i;
      if (target < 0) target = e.selStep;
      if (target >= 0 && target < ns) {
        const int k = stepSlice(m.pattern, m.pattern.steps[static_cast<size_t>(target)], nSl);
        if (k >= 0) deleteRegionUi(a, t, e, k);
      }
    }
  }
}

}  // namespace gl::packs::seltmann::sui
