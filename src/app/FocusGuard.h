// Gridline - 0.7.8: drawing that nobody sees leaves the interface's focus alone.
//
// Some passes draw menus or panels only to read them: the macOS menu bar's walk of every menu, the
// command palette's, a menu item chosen from the system bar. That code is the menus' own, so it may
// ask for the keyboard (a plugin picker's search field calls SetKeyboardFocusHere when it appears)
// or take the active item. In a pass nobody sees, either one took the active item from what the
// pointer held: a note or a clip being dragged stopped after its first step (every edit changed the
// menus, so the walk ran again), with the button still down.
//
//   { focusguard::Keep keep;  ...draw the unseen pass...  }   // restores on scope exit
//
// What it keeps: the active item and everything ImGui keeps with it, the navigation (focused window,
// item, layer, scope) and any navigation or focus request (a tabbing or SetKeyboardFocusHere request
// made inside is dropped, one made before is kept), and whether text input is wanted.
#pragma once
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"

namespace gl::focusguard {

class Keep {
 public:
  Keep() {
    ImGuiContext* c = ImGui::GetCurrentContext();
    if (!c) return;
    ctx_ = c;
    const ImGuiContext& g = *c;
    // active item
    activeId_ = g.ActiveId;
    activeIdIsAlive_ = g.ActiveIdIsAlive;
    activeIdTimer_ = g.ActiveIdTimer;
    justActivated_ = g.ActiveIdIsJustActivated;
    wasSelected_ = g.ActiveIdWasSelected;
    wasSoleSelected_ = g.ActiveIdWasSoleSelected;
    allowOverlap_ = g.ActiveIdAllowOverlap;
    noClearOnFocusLoss_ = g.ActiveIdNoClearOnFocusLoss;
    pressedBefore_ = g.ActiveIdHasBeenPressedBefore;
    editedBefore_ = g.ActiveIdHasBeenEditedBefore;
    editedThisFrame_ = g.ActiveIdHasBeenEditedThisFrame;
    fromShortcut_ = g.ActiveIdFromShortcut;
    mouseButton_ = g.ActiveIdMouseButton;
    disabledId_ = g.ActiveIdDisabledId;
    clickOffset_ = g.ActiveIdClickOffset;
    source_ = g.ActiveIdSource;
    activeWindow_ = g.ActiveIdWindow;
    valueOnActivation_ = g.ActiveIdValueOnActivation;
    usingNavDirMask_ = g.ActiveIdUsingNavDirMask;
    usingAllKeys_ = g.ActiveIdUsingAllKeyboardKeys;
    // navigation and its requests
    navId_ = g.NavId;
    navWindow_ = g.NavWindow;
    navFocusScopeId_ = g.NavFocusScopeId;
    navLayer_ = g.NavLayer;
    navIdItemFlags_ = g.NavIdItemFlags;
    navActivateId_ = g.NavActivateId;
    navActivateDownId_ = g.NavActivateDownId;
    navActivatePressedId_ = g.NavActivatePressedId;
    navActivateFlags_ = g.NavActivateFlags;
    navFocusRoute_ = g.NavFocusRoute;
    navNextActivateId_ = g.NavNextActivateId;
    navNextActivateFlags_ = g.NavNextActivateFlags;
    navCursorVisible_ = g.NavCursorVisible;
    navHighlightUnderNav_ = g.NavHighlightItemUnderNav;
    navAnyRequest_ = g.NavAnyRequest;
    navInitRequest_ = g.NavInitRequest;
    navInitRequestFromMove_ = g.NavInitRequestFromMove;
    navInitResult_ = g.NavInitResult;
    navMoveSubmitted_ = g.NavMoveSubmitted;
    navMoveScoringItems_ = g.NavMoveScoringItems;
    navMoveForward_ = g.NavMoveForwardToNextFrame;
    navMoveFlags_ = g.NavMoveFlags;
    navMoveScrollFlags_ = g.NavMoveScrollFlags;
    navMoveKeyMods_ = g.NavMoveKeyMods;
    navMoveDir_ = g.NavMoveDir;
    navMoveDirForDebug_ = g.NavMoveDirForDebug;
    navMoveClipDir_ = g.NavMoveClipDir;
    navScoringRect_ = g.NavScoringRect;
    navScoringNoClipRect_ = g.NavScoringNoClipRect;
    navTabbingDir_ = g.NavTabbingDir;
    navTabbingCounter_ = g.NavTabbingCounter;
    navMoveResultLocal_ = g.NavMoveResultLocal;
    navMoveResultLocalVisible_ = g.NavMoveResultLocalVisible;
    navMoveResultOther_ = g.NavMoveResultOther;
    navTabbingResultFirst_ = g.NavTabbingResultFirst;
    wantTextInputNextFrame_ = g.WantTextInputNextFrame;
  }
  Keep(const Keep&) = delete;
  Keep& operator=(const Keep&) = delete;
  ~Keep() {
    if (!ctx_ || ImGui::GetCurrentContext() != ctx_) return;
    ImGuiContext& g = *ctx_;
    // How often the pass changed anything (tests and the drag log read it).
    if (g.ActiveId != activeId_ || g.NavWindow != navWindow_ || g.NavId != navId_ || g.NavMoveSubmitted != navMoveSubmitted_ ||
        g.NavActivateId != navActivateId_ || g.NavNextActivateId != navNextActivateId_)
      ++kept();
    g.ActiveId = activeId_;
    g.ActiveIdIsAlive = activeIdIsAlive_;
    g.ActiveIdTimer = activeIdTimer_;
    g.ActiveIdIsJustActivated = justActivated_;
    g.ActiveIdWasSelected = wasSelected_;
    g.ActiveIdWasSoleSelected = wasSoleSelected_;
    g.ActiveIdAllowOverlap = allowOverlap_;
    g.ActiveIdNoClearOnFocusLoss = noClearOnFocusLoss_;
    g.ActiveIdHasBeenPressedBefore = pressedBefore_;
    g.ActiveIdHasBeenEditedBefore = editedBefore_;
    g.ActiveIdHasBeenEditedThisFrame = editedThisFrame_;
    g.ActiveIdFromShortcut = fromShortcut_;
    g.ActiveIdMouseButton = mouseButton_;
    g.ActiveIdDisabledId = disabledId_;
    g.ActiveIdClickOffset = clickOffset_;
    g.ActiveIdSource = source_;
    g.ActiveIdWindow = activeWindow_;
    g.ActiveIdValueOnActivation = valueOnActivation_;
    g.ActiveIdUsingNavDirMask = usingNavDirMask_;
    g.ActiveIdUsingAllKeyboardKeys = usingAllKeys_;
    g.NavId = navId_;
    g.NavWindow = navWindow_;
    g.NavFocusScopeId = navFocusScopeId_;
    g.NavLayer = navLayer_;
    g.NavIdItemFlags = navIdItemFlags_;
    g.NavActivateId = navActivateId_;
    g.NavActivateDownId = navActivateDownId_;
    g.NavActivatePressedId = navActivatePressedId_;
    g.NavActivateFlags = navActivateFlags_;
    g.NavFocusRoute = navFocusRoute_;
    g.NavNextActivateId = navNextActivateId_;
    g.NavNextActivateFlags = navNextActivateFlags_;
    g.NavCursorVisible = navCursorVisible_;
    g.NavHighlightItemUnderNav = navHighlightUnderNav_;
    g.NavAnyRequest = navAnyRequest_;
    g.NavInitRequest = navInitRequest_;
    g.NavInitRequestFromMove = navInitRequestFromMove_;
    g.NavInitResult = navInitResult_;
    g.NavMoveSubmitted = navMoveSubmitted_;
    g.NavMoveScoringItems = navMoveScoringItems_;
    g.NavMoveForwardToNextFrame = navMoveForward_;
    g.NavMoveFlags = navMoveFlags_;
    g.NavMoveScrollFlags = navMoveScrollFlags_;
    g.NavMoveKeyMods = navMoveKeyMods_;
    g.NavMoveDir = navMoveDir_;
    g.NavMoveDirForDebug = navMoveDirForDebug_;
    g.NavMoveClipDir = navMoveClipDir_;
    g.NavScoringRect = navScoringRect_;
    g.NavScoringNoClipRect = navScoringNoClipRect_;
    g.NavTabbingDir = navTabbingDir_;
    g.NavTabbingCounter = navTabbingCounter_;
    g.NavMoveResultLocal = navMoveResultLocal_;
    g.NavMoveResultLocalVisible = navMoveResultLocalVisible_;
    g.NavMoveResultOther = navMoveResultOther_;
    g.NavTabbingResultFirst = navTabbingResultFirst_;
    g.WantTextInputNextFrame = wantTextInputNextFrame_;
  }
  // Passes that changed the focus or the active item (and were put back), since the start.
  static int& kept() {
    static int n = 0;
    return n;
  }

 private:
  ImGuiContext* ctx_ = nullptr;
  ImGuiID activeId_ = 0, activeIdIsAlive_ = 0, disabledId_ = 0;
  float activeIdTimer_ = 0;
  bool justActivated_ = false, wasSelected_ = false, wasSoleSelected_ = false, allowOverlap_ = false, noClearOnFocusLoss_ = false,
       pressedBefore_ = false, editedBefore_ = false, editedThisFrame_ = false, fromShortcut_ = false, usingAllKeys_ = false;
  ImS8 mouseButton_ = -1;
  ImVec2 clickOffset_;
  ImGuiInputSource source_ = ImGuiInputSource_None;
  ImGuiWindow* activeWindow_ = nullptr;
  ImGuiDataTypeStorage valueOnActivation_{};
  ImU32 usingNavDirMask_ = 0;
  ImGuiID navId_ = 0, navFocusScopeId_ = 0, navActivateId_ = 0, navActivateDownId_ = 0, navActivatePressedId_ = 0, navNextActivateId_ = 0;
  ImGuiWindow* navWindow_ = nullptr;
  ImGuiNavLayer navLayer_ = ImGuiNavLayer_Main;
  ImGuiItemFlags navIdItemFlags_ = 0;
  ImGuiActivateFlags navActivateFlags_ = 0, navNextActivateFlags_ = 0;
  ImVector<ImGuiFocusScopeData> navFocusRoute_;
  bool navCursorVisible_ = false, navHighlightUnderNav_ = false, navAnyRequest_ = false, navInitRequest_ = false, navInitRequestFromMove_ = false,
       navMoveSubmitted_ = false, navMoveScoringItems_ = false, navMoveForward_ = false;
  ImGuiNavItemData navInitResult_, navMoveResultLocal_, navMoveResultLocalVisible_, navMoveResultOther_, navTabbingResultFirst_;
  ImGuiNavMoveFlags navMoveFlags_ = 0;
  ImGuiScrollFlags navMoveScrollFlags_ = 0;
  ImGuiKeyChord navMoveKeyMods_ = 0;
  ImGuiDir navMoveDir_ = ImGuiDir_None, navMoveDirForDebug_ = ImGuiDir_None, navMoveClipDir_ = ImGuiDir_None;
  ImRect navScoringRect_, navScoringNoClipRect_;
  int navTabbingDir_ = 0, navTabbingCounter_ = 0, wantTextInputNextFrame_ = -1;
};

}  // namespace gl::focusguard
