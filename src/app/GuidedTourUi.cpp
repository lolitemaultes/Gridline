// Gridline - K-105: Interactive non-blocking Guided Tour inside a real project.
#include "GuidedTourUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutPreset.h"
#include "../core/SetupTour.h"
#include "App.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "SetupWizardUi.h"
#include "SmoothScrollUi.h"
#include "Theme.h"
#include "TrackerView.h"
#include "Widgets.h"

namespace gl::guidedtour {

namespace {

using i18n::tr;
using setuptour::CardPlacement;
using setuptour::CardSide;
using setuptour::Rect;
using setuptour::TourStep;
using setuptour::TourTarget;

struct TourState {
  bool running = false;
  bool askEmpty = false;  // Help > Guided tour... on an empty song: offer demo vs current project
  int step = 0;
  int appliedStep = -1;
  layout::Preset appliedLayout = layout::Preset::Count;
  bool hasSmooth = false;
  Rect smoothSpot{};
  Rect lastTarget{};
  Rect lastCard{};
};

TourState& st() {
  static TourState s;
  return s;
}

ImGuiWindow* findLiveWindow(const char* stableName) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g) return nullptr;
  for (ImGuiWindow* w : g->Windows) {
    if (!w || !(w->Active || w->WasActive)) continue;
    if (w->Size.x < 16.0f || w->Size.y < 16.0f) continue;
    if (std::strcmp(w->Name, stableName) == 0 || std::strcmp(ui::stableWindowName(w->Name), stableName) == 0)
      return w;
  }
  return nullptr;
}

bool rectFromWindow(const char* stableName, Rect& out) {
  ImGuiWindow* w = findLiveWindow(stableName);
  if (!w) return false;
  out = {w->Pos.x, w->Pos.y, w->Size.x, w->Size.y};
  return out.w > 16.0f && out.h > 16.0f;
}

bool rectFromRegion(const char* regionId, Rect& out) {
  const ImGuiID nodeId = layoutui::regionNode(regionId);
  if (!nodeId) return false;
  ImGuiDockNode* node = ImGui::DockBuilderGetNode(nodeId);
  if (!node || node->Size.x < 16.0f || node->Size.y < 16.0f) return false;
  out = {node->Pos.x, node->Pos.y, node->Size.x, node->Size.y};
  return true;
}

// Ensures the window/region for the current step is visible and focused in the active layout.
void revealTarget(App& a, TourTarget target, layout::Preset preset) {
  if (preset == layout::Preset::Tracker) {
    switch (target) {
      case TourTarget::TrackerHeader:
        trackerview::setExtended(a, false);
        break;
      case TourTarget::PatternEditor:
        trackerview::showPage(a, "Pattern editor");
        break;
      case TourTarget::Browser:
        a.showBrowser = true;
        a.focusWindow = "Browser";
        break;
      case TourTarget::PianoRoll:
        trackerview::showPage(a, "Piano roll");
        break;
      case TourTarget::Playlist:
        trackerview::showPage(a, "Playlist");
        break;
      case TourTarget::Mixer:
        trackerview::showPage(a, "Mixer");
        break;
      default: break;
    }
    return;
  }

  if (preset == layout::Preset::Live) {
    switch (target) {
      case TourTarget::Browser:
        a.showBrowser = true;
        layoutui::setRegionVisible(a, "live.browser", true);
        break;
      case TourTarget::Playlist:
        a.showPlaylist = true;
        layoutui::showPlaylist(a);
        break;
      case TourTarget::PianoRoll:
        a.showPiano = true;
        layoutui::setRegionVisible(a, "live.clip", true);
        a.focusWindow = "Piano roll";
        break;
      case TourTarget::DeviceChain:
        layoutui::setRegionVisible(a, "live.device", true);
        a.focusWindow = "Device chain";
        break;
      case TourTarget::Mixer:
        a.showMixer = true;
        layoutui::setRegionVisible(a, "live.mixer", true);
        a.focusWindow = "Mixer";
        break;
      default: break;
    }
    return;
  }

  if (preset == layout::Preset::Logic) {
    switch (target) {
      case TourTarget::Inspector:
        layoutui::setRegionVisible(a, "logic.inspector", true);
        a.focusWindow = "Inspector";
        break;
      case TourTarget::Browser:
        a.showBrowser = true;
        layoutui::setRegionVisible(a, "logic.library", true);
        a.focusWindow = "Browser";
        break;
      case TourTarget::Playlist:
        a.showPlaylist = true;
        a.focusWindow = "Playlist";
        break;
      case TourTarget::PianoRoll:
        a.showPiano = true;
        layoutui::showPaneWindow(a, "Piano roll");
        a.focusWindow = "Piano roll";
        break;
      case TourTarget::Mixer:
        a.showMixer = true;
        layoutui::showPaneWindow(a, "Mixer");
        a.focusWindow = "Mixer";
        break;
      default: break;
    }
    return;
  }

  // Studio layout
  switch (target) {
    case TourTarget::Browser:
      a.showBrowser = true;
      a.focusWindow = "Browser";
      break;
    case TourTarget::ChannelRack:
      a.showRack = true;
      a.focusWindow = "Channel rack";
      break;
    case TourTarget::PianoRoll:
      a.showPiano = true;
      a.focusWindow = "Piano roll";
      break;
    case TourTarget::Playlist:
      a.showPlaylist = true;
      a.focusWindow = "Playlist";
      break;
    case TourTarget::Mixer:
      a.showMixer = true;
      a.focusWindow = "Mixer";
      break;
    default: break;
  }
}

Rect resolveTargetRect(TourTarget target, layout::Preset preset, const Rect& vp) {
  Rect r{};
  switch (target) {
    case TourTarget::Transport:
      if (rectFromWindow("##topbar", r)) return r;
      return {vp.x, vp.y, vp.w, theme::kTopBarH};
    case TourTarget::Browser:
      if (rectFromWindow("Browser", r)) return r;
      if (preset == layout::Preset::Logic && rectFromRegion("logic.library", r)) return r;
      if (preset == layout::Preset::Live && rectFromRegion("live.browser", r)) return r;
      return {vp.x + 8.0f, vp.y + theme::kTopBarH + 8.0f, std::min(260.0f, vp.w * 0.22f), vp.h - theme::kTopBarH - 42.0f};
    case TourTarget::ChannelRack:
      if (rectFromWindow("Channel rack", r)) return r;
      return {vp.x + vp.w * 0.24f, vp.y + vp.h * 0.24f, vp.w * 0.48f, vp.h * 0.34f};
    case TourTarget::PianoRoll:
      if (rectFromWindow("Piano roll", r)) return r;
      if (preset == layout::Preset::Live && rectFromRegion("live.clip", r)) return r;
      if (preset == layout::Preset::Logic && rectFromRegion("logic.lower", r)) return r;
      if (preset == layout::Preset::Tracker && rectFromRegion("tracker.pages", r)) return r;
      return {vp.x + vp.w * 0.22f, vp.y + theme::kTopBarH + 8.0f, vp.w * 0.54f, vp.h * 0.6f};
    case TourTarget::Playlist:
      if (preset == layout::Preset::Live && rectFromWindow("Live pads", r)) return r;
      if (rectFromWindow("Playlist", r)) return r;
      if (preset == layout::Preset::Live && rectFromRegion("live.main", r)) return r;
      if (preset == layout::Preset::Logic && rectFromRegion("logic.tracks", r)) return r;
      if (preset == layout::Preset::Tracker && rectFromRegion("tracker.pages", r)) return r;
      return {vp.x + vp.w * 0.22f, vp.y + theme::kTopBarH + 8.0f, vp.w * 0.54f, vp.h * 0.72f};
    case TourTarget::Mixer:
      if (rectFromWindow("Mixer", r)) return r;
      if (preset == layout::Preset::Live && rectFromRegion("live.mixer", r)) return r;
      if (preset == layout::Preset::Logic && rectFromRegion("logic.lower", r)) return r;
      if (preset == layout::Preset::Tracker && rectFromRegion("tracker.pages", r)) return r;
      return {vp.x + vp.w * 0.75f, vp.y + theme::kTopBarH + 8.0f, vp.w * 0.24f, vp.h - theme::kTopBarH - 42.0f};
    case TourTarget::DeviceChain:
      if (rectFromWindow("Device chain", r)) return r;
      if (rectFromRegion("live.device", r)) return r;
      return {vp.x + 12.0f, vp.y + vp.h - 240.0f, vp.w - 24.0f, 200.0f};
    case TourTarget::Inspector:
      if (rectFromWindow("Inspector", r)) return r;
      if (rectFromRegion("logic.inspector", r)) return r;
      return {vp.x + 8.0f, vp.y + theme::kTopBarH + 8.0f, 230.0f, vp.h - theme::kTopBarH - 42.0f};
    case TourTarget::TrackerHeader:
      if (rectFromRegion("tracker.header", r)) return r;
      return {vp.x + 4.0f, vp.y + 30.0f, vp.w - 8.0f, std::min(250.0f, vp.h * 0.30f)};
    case TourTarget::PatternEditor:
      if (rectFromWindow("Pattern editor", r)) return r;
      if (rectFromRegion("tracker.pages", r)) return r;
      return {vp.x + 4.0f, vp.y + vp.h * 0.34f, vp.w - 8.0f, vp.h * 0.60f};
  }
  return {vp.x + 24.0f, vp.y + 60.0f, 320.0f, 220.0f};
}

void drawSpotlightScrim(ImDrawList* dl, const Rect& vp, const Rect& spot, double now) {
  const auto& P = theme::pal();
  const ImU32 scrim = theme::withAlpha(P.ink, P.dark ? 0.56f : 0.32f);
  const float vx0 = vp.x, vy0 = vp.y, vx1 = vp.x + vp.w, vy1 = vp.y + vp.h;
  const float sx0 = spot.x, sy0 = spot.y, sx1 = spot.x + spot.w, sy1 = spot.y + spot.h;

  // Four non-overlapping scrim strips leave the spotlight cutout completely clear and interactive.
  if (sy0 > vy0) dl->AddRectFilled(ImVec2(vx0, vy0), ImVec2(vx1, sy0), scrim);
  if (sy1 < vy1) dl->AddRectFilled(ImVec2(vx0, sy1), ImVec2(vx1, vy1), scrim);
  if (sx0 > vx0) dl->AddRectFilled(ImVec2(vx0, sy0), ImVec2(sx0, sy1), scrim);
  if (sx1 < vx1) dl->AddRectFilled(ImVec2(sx1, sy0), ImVec2(vx1, sy1), scrim);

  // Crisp glowing accent cutout ring around the highlighted panel.
  const float pulse = scrollui::animations() ? (0.5f + 0.5f * std::sin(static_cast<float>(now) * 3.2f)) : 0.6f;
  const float rad = theme::kRadiusLg + 2.0f;
  dl->AddRect(ImVec2(sx0 - 3.0f, sy0 - 3.0f), ImVec2(sx1 + 3.0f, sy1 + 3.0f),
              theme::withAlpha(P.accent, 0.22f + 0.14f * pulse), rad + 3.0f, 0, 5.0f);
  dl->AddRect(ImVec2(sx0, sy0), ImVec2(sx1, sy1), P.accent, rad, 0, 2.5f);
}

void drawConnectorPointer(ImDrawList* dl, const CardPlacement& pl, ImU32 fillCol, ImU32 borderCol) {
  if (pl.side == CardSide::Inset) return;
  const float cx0 = pl.card.x, cy0 = pl.card.y, cx1 = pl.card.x + pl.card.w, cy1 = pl.card.y + pl.card.h;
  const float sz = 8.0f;
  ImVec2 a, b, tip;
  if (pl.side == CardSide::Right) {
    const float y = std::clamp(pl.anchorY, cy0 + 20.0f, cy1 - 20.0f);
    a = ImVec2(cx0, y - sz);
    b = ImVec2(cx0, y + sz);
    tip = ImVec2(cx0 - sz, y);
  } else if (pl.side == CardSide::Left) {
    const float y = std::clamp(pl.anchorY, cy0 + 20.0f, cy1 - 20.0f);
    a = ImVec2(cx1, y - sz);
    b = ImVec2(cx1, y + sz);
    tip = ImVec2(cx1 + sz, y);
  } else if (pl.side == CardSide::Bottom) {
    const float x = std::clamp(pl.anchorX, cx0 + 24.0f, cx1 - 24.0f);
    a = ImVec2(x - sz, cy0);
    b = ImVec2(x + sz, cy0);
    tip = ImVec2(x, cy0 - sz);
  } else {
    const float x = std::clamp(pl.anchorX, cx0 + 24.0f, cx1 - 24.0f);
    a = ImVec2(x - sz, cy1);
    b = ImVec2(x + sz, cy1);
    tip = ImVec2(x, cy1 + sz);
  }
  dl->AddTriangleFilled(a, b, tip, fillCol);
  dl->AddLine(a, tip, borderCol, 1.5f);
  dl->AddLine(b, tip, borderCol, 1.5f);
}

void drawEmptySongChoice(App& a) {
  const auto& P = theme::pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float w = std::min(430.0f, vp->WorkSize.x - 40.0f);
  const float h = 186.0f;
  const ImVec2 pos(std::round(vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f),
                   std::round(vp->WorkPos.y + (vp->WorkSize.y - h) * 0.5f));

  // Subtle backdrop window above workspace and below choice card.
  ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(theme::withAlpha(P.ink, P.dark ? 0.45f : 0.25f)));
  if (ImGui::Begin("##guided_tour_empty_bg", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);

  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
  ImGui::SetNextWindowViewport(vp->ID);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::kRadiusLg + 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface900));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(P.accent));

  if (ImGui::Begin("##guided_tour_empty_ask", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::PushFont(a.fontBold, theme::kFontTitle);
    ImGui::TextColored(theme::vec(P.text), "%s", tr("Start the guided tour"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 40.0f);
    ImGui::TextColored(theme::vec(P.textDim), "%s",
                       tr("Your project is currently empty. Load the built-in demo song so you can hear and edit real tracks during the tour, or tour your current workspace."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, theme::kS4));

    if (ui::PrimaryButton("Load demo & start tour", 0.0f)) {
      st().askEmpty = false;
      a.guardUnsaved([&a]() { start(a, true); });
    }
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Tour current project", false, 0.0f)) {
      st().askEmpty = false;
      start(a, false);
    }
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Cancel", false, 0.0f)) {
      st().askEmpty = false;
    }
    if (keys::escapePressed()) st().askEmpty = false;
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
}

}  // namespace

void start(App& a, bool loadDemoFirst) {
  if (setupwizard::visible()) setupwizard::close(a, false);
  if (loadDemoFirst) {
    a.home.projectOpened(a.now);
    a.newFromTemplate("demo");
  } else if (a.home.visible()) {
    a.home.projectOpened(a.now);
  }
  TourState& s = st();
  s.askEmpty = false;
  s.running = true;
  s.step = 0;
  s.appliedStep = -1;
  s.appliedLayout = layout::Preset::Count;
  s.hasSmooth = false;
}

void openFromMenu(App& a) {
  if (setupwizard::visible()) setupwizard::close(a, false);
  if (a.home.visible()) a.home.projectOpened(a.now);
  if (setuptour::isSongEmpty(a.doc.song())) {
    st().askEmpty = true;
    st().running = false;
  } else {
    start(a, false);
  }
}

void stop() {
  TourState& s = st();
  s.running = false;
  s.askEmpty = false;
  s.appliedStep = -1;
  s.hasSmooth = false;
}

void next(App& a) {
  TourState& s = st();
  if (!s.running) return;
  const auto& steps = setuptour::tourStepsFor(layoutui::active());
  if (s.step + 1 < static_cast<int>(steps.size())) {
    ++s.step;
  } else {
    stop();
  }
  (void)a;
}

void back(App& a) {
  TourState& s = st();
  if (!s.running) return;
  s.step = std::max(0, s.step - 1);
  (void)a;
}

void setStep(App& a, int stepIndex) {
  if (!st().running) start(a, false);
  const auto& steps = setuptour::tourStepsFor(layoutui::active());
  if (steps.empty()) return;
  st().step = std::clamp(stepIndex, 0, static_cast<int>(steps.size()) - 1);
}

bool active() { return st().running || st().askEmpty; }

int currentStep() { return st().step; }

void draw(App& a) {
  TourState& s = st();
  if (a.home.visible()) return;
  if (s.askEmpty) {
    drawEmptySongChoice(a);
    return;
  }
  if (!s.running) return;

  const layout::Preset preset = layoutui::active();
  const auto& steps = setuptour::tourStepsFor(preset);
  if (steps.empty()) {
    stop();
    return;
  }
  const int count = static_cast<int>(steps.size());
  s.step = std::clamp(s.step, 0, count - 1);
  const TourStep& cur = steps[static_cast<size_t>(s.step)];

  // Ensure the target panel is visible whenever the step or layout changes.
  if (s.appliedStep != s.step || s.appliedLayout != preset) {
    revealTarget(a, cur.target, preset);
    s.appliedStep = s.step;
    s.appliedLayout = preset;
  }

  ImGuiViewport* vp = ImGui::GetMainViewport();
  const Rect vpRect{vp->WorkPos.x, vp->WorkPos.y, vp->WorkSize.x, vp->WorkSize.y};
  const Rect rawTarget = resolveTargetRect(cur.target, preset, vpRect);
  const Rect clampedTarget = setuptour::clampSpotlightRect(vpRect, rawTarget, 4.0f, 3.0f);

  // Smooth spotlight transition between steps while tracking live window drags tightly.
  if (!s.hasSmooth || !scrollui::animations()) {
    s.smoothSpot = clampedTarget;
    s.hasSmooth = true;
  } else {
    const float k = std::min(1.0f, ImGui::GetIO().DeltaTime * 24.0f);
    auto lerp = [k](float a0, float b0) {
      const float v = a0 + (b0 - a0) * k;
      return std::fabs(b0 - v) < 0.75f ? b0 : v;
    };
    s.smoothSpot.x = lerp(s.smoothSpot.x, clampedTarget.x);
    s.smoothSpot.y = lerp(s.smoothSpot.y, clampedTarget.y);
    s.smoothSpot.w = lerp(s.smoothSpot.w, clampedTarget.w);
    s.smoothSpot.h = lerp(s.smoothSpot.h, clampedTarget.h);
  }
  s.lastTarget = s.smoothSpot;

  const auto& P = theme::pal();

  // Compute card metrics & non-overlapping placement inside the viewport.
  const float cardW = std::min(360.0f, std::max(270.0f, vpRect.w - 32.0f));
  const float cardPadX = 18.0f;
  const float textW = cardW - cardPadX * 2.0f;
  const char* bodyTxt = tr(cur.body);
  const char* tipTxt = tr(cur.tip);
  const float bodyH = a.fontRegular->CalcTextSizeA(theme::kFontBody, FLT_MAX, textW, bodyTxt).y;
  const float tipH = a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, textW - 24.0f, tipTxt).y;
  const float cardH = std::round(18.0f + 22.0f + 26.0f + bodyH + 12.0f + (tipH + 22.0f) + 18.0f + 16.0f + 38.0f + 16.0f);

  const CardPlacement pl = setuptour::placeCalloutCard(vpRect, s.smoothSpot, cardW, cardH, 16.0f, 16.0f);
  s.lastCard = pl.card;

  // Non-blocking scrim overlay window (NoInputs lets clicks pass straight through to the spotlighted panel!).
  ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  if (ImGui::Begin("##guided_tour_scrim", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    drawSpotlightScrim(sdl, vpRect, s.smoothSpot, a.now);
    drawConnectorPointer(sdl, pl, P.surface900, P.accent);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);

  ImGui::SetNextWindowPos(ImVec2(pl.card.x, pl.card.y), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(pl.card.w, pl.card.h), ImGuiCond_Always);
  ImGui::SetNextWindowViewport(vp->ID);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPadX, 16.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::kRadiusLg + 5.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface900));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(P.accent));

  const ImGuiWindowFlags cardFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##guided_tour_card", nullptr, cardFlags)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float wAvail = ImGui::GetContentRegionAvail().x;

    // Top row: "Step X of N · Layout" badge + close X button
    char badge[64];
    std::snprintf(badge, sizeof badge, tr("Step %d of %d"), s.step + 1, count);
    const ImVec2 topCur = ImGui::GetCursorScreenPos();
    const ImVec2 bSz = a.fontMono->CalcTextSizeA(theme::kFontLabel, FLT_MAX, 0.0f, badge);
    const float pillW = bSz.x + 14.0f;
    const float pillH = 20.0f;
    dl->AddRectFilled(topCur, ImVec2(topCur.x + pillW, topCur.y + pillH), theme::withAlpha(P.accent, 0.18f), pillH * 0.5f);
    dl->AddRect(topCur, ImVec2(topCur.x + pillW, topCur.y + pillH), theme::withAlpha(P.accent, 0.55f), pillH * 0.5f);
    dl->AddText(a.fontMono, theme::kFontLabel,
                ImVec2(std::round(topCur.x + 7.0f), theme::textTop(a.fontMono, theme::kFontLabel, topCur.y, pillH)),
                P.accent, badge);

    // Layout tag right next to step pill
    const char* layoutName = tr(layout::displayName(preset));
    dl->AddText(a.fontMono, theme::kFontLabel,
                ImVec2(std::round(topCur.x + pillW + 8.0f), theme::textTop(a.fontMono, theme::kFontLabel, topCur.y, pillH)),
                P.textFaint, layoutName);

    // Close button on top-right
    ImGui::SetCursorScreenPos(ImVec2(topCur.x + wAvail - 20.0f, topCur.y));
    if (ui::IconButton("##tour_close", ui::Icon::Close, false, 20.0f, i18n::N_("Skip tour"))) {
      stop();
      ImGui::End();
      ImGui::PopStyleColor(2);
      ImGui::PopStyleVar(3);
      return;
    }

    // Step title
    ImGui::SetCursorScreenPos(ImVec2(topCur.x, topCur.y + 26.0f));
    ImGui::PushFont(a.fontBold, theme::kFontTitle);
    ImGui::TextColored(theme::vec(P.text), "%s", tr(cur.title));
    ImGui::PopFont();

    // Description
    ImGui::Dummy(ImVec2(0, 2.0f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wAvail);
    ImGui::TextColored(theme::vec(P.textDim), "%s", bodyTxt);
    ImGui::PopTextWrapPos();

    // "Try it" actionable tip pill box
    ImGui::Dummy(ImVec2(0, theme::kS2));
    {
      const ImVec2 t0 = ImGui::GetCursorScreenPos();
      const float boxH = std::round(tipH + 18.0f);
      const ImVec2 t1(t0.x + wAvail, t0.y + boxH);
      dl->AddRectFilled(t0, t1, P.surface800, theme::kRadiusLg);
      dl->AddRectFilled(t0, ImVec2(t0.x + 3.0f, t1.y), P.hot, theme::kRadiusLg, ImDrawFlags_RoundCornersLeft);
      dl->AddRect(t0, t1, P.line, theme::kRadiusLg);
      dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round(t0.x + 12.0f), std::round(t0.y + 9.0f)),
                  P.text, tipTxt, nullptr, wAvail - 20.0f);
      ImGui::Dummy(ImVec2(wAvail, boxH));
    }

    // Progress dots (clickable)
    ImGui::Dummy(ImVec2(0, theme::kS2));
    {
      const ImVec2 dPos = ImGui::GetCursorScreenPos();
      float dx = dPos.x;
      const float dy = std::round(dPos.y + 5.0f);
      for (int i = 0; i < count; ++i) {
        const bool isCur = (i == s.step);
        const bool isPast = (i < s.step);
        const float dw = isCur ? 18.0f : 7.0f;
        const float dh = 7.0f;
        ImGui::SetCursorScreenPos(ImVec2(dx, dy - 4.0f));
        ImGui::PushID(500 + i);
        if (ImGui::InvisibleButton("##tdot", ImVec2(dw + 2.0f, 14.0f))) s.step = i;
        ImGui::PopID();
        const ImU32 col = isCur ? P.accent : (isPast ? theme::withAlpha(P.accent, 0.55f) : P.lineStrong);
        dl->AddRectFilled(ImVec2(dx, dy), ImVec2(dx + dw, dy + dh), col, dh * 0.5f);
        dx += dw + 6.0f;
      }
      ImGui::SetCursorScreenPos(ImVec2(dPos.x, dPos.y + 16.0f));
    }

    // Footer buttons: Skip tour (left) | Back + Next/Done (right)
    const float footY = winPos.y + pl.card.h - 16.0f - theme::kControlH;
    ImGui::SetCursorScreenPos(ImVec2(topCur.x, footY));
    if (ui::TextButton("Skip tour", false, 0.0f, "Exit the guided tour (Esc)")) {
      stop();
      ImGui::End();
      ImGui::PopStyleColor(2);
      ImGui::PopStyleVar(3);
      return;
    }

    const bool isLast = (s.step + 1 >= count);
    const char* nextLbl = isLast ? i18n::N_("Done") : i18n::N_("Next");
    const float nextW = std::max(72.0f, ui::TextButtonWidth(nextLbl));
    const float backW = s.step > 0 ? std::max(64.0f, ui::TextButtonWidth("Back")) : 0.0f;
    float rx = topCur.x + wAvail - nextW;
    if (s.step > 0) {
      ImGui::SetCursorScreenPos(ImVec2(rx - theme::kS2 - backW, footY));
      if (ui::TextButton("Back", false, backW)) back(a);
    }
    ImGui::SetCursorScreenPos(ImVec2(rx, footY));
    if (ui::PrimaryButton(nextLbl, nextW)) next(a);
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);

  // Esc exits the tour when no popup or text input owns it.
  if (s.running && !ImGui::GetIO().WantTextInput &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && keys::escapePressed()) {
    stop();
  }
}

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  const std::string sub = args.empty() ? std::string("state") : args[0];
  if (sub == "start") {
    const bool demo = args.size() >= 2 && args[1] == "demo";
    start(a, demo);
  } else if (sub == "menu") {
    openFromMenu(a);
  } else if (sub == "next") {
    next(a);
  } else if (sub == "back") {
    back(a);
  } else if (sub == "step" && args.size() >= 2) {
    const int n = std::atoi(args[1].c_str());
    setStep(a, n <= 0 ? 0 : n - 1);
  } else if (sub == "stop") {
    stop();
  }
  const TourState& s = st();
  const auto& steps = setuptour::tourStepsFor(layoutui::active());
  const int idx = std::clamp(s.step, 0, std::max(0, static_cast<int>(steps.size()) - 1));
  const char* tid = steps.empty() ? "-" : steps[static_cast<size_t>(idx)].id;
  std::printf("tour frame %d: active %d askEmpty %d step %d/%zu target %s layout %s spot %.0f,%.0f,%.0f,%.0f card %.0f,%.0f,%.0f,%.0f\n",
              frame, s.running ? 1 : 0, s.askEmpty ? 1 : 0, s.step + 1, steps.size(), tid,
              layout::id(layoutui::active()), static_cast<double>(s.lastTarget.x), static_cast<double>(s.lastTarget.y),
              static_cast<double>(s.lastTarget.w), static_cast<double>(s.lastTarget.h),
              static_cast<double>(s.lastCard.x), static_cast<double>(s.lastCard.y), static_cast<double>(s.lastCard.w),
              static_cast<double>(s.lastCard.h));
  std::fflush(stdout);
}

}  // namespace gl::guidedtour
