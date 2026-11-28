// K-86 part 2 (0.7.9): Collaboration chat panel, "Send to chat" prompt, and "View" + element flash.
#include "CollabChatUi.h"

#include "App.h"
#include "CollabUi.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "collab/Session.h"
#include "core/I18n.h"
#include "core/Meter.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/SemanticAnchor.h"
#include "core/Song.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>

namespace gl::collabchat {

namespace {

using i18n::tr;
using theme::pal;

struct NoteFlash {
  ObjectId patId = kNoId;
  ObjectId chId = kNoId;
  Tick tick0 = 0;
  Tick tick1 = 0;
  int pitchLo = 0;
  int pitchHi = 127;
  double until = 0;
  double duration = 1.8;
  uint32_t colorRgb = 0;
};

struct State {
  char inputBuf[2048] = {};
  bool focusInputNext = false;
  size_t lastMsgCount = 0;
  bool scrollToBottom = false;

  // Compact "Send to chat" comment prompt state.
  bool promptPendingOpen = false;
  bool promptOpen = false;
  bool promptFocusInput = false;
  ChatAttachment promptAtt;
  char promptComment[1024] = {};

  // Active Piano Roll note highlight from clicking View on a Notes attachment.
  NoteFlash noteFlash;

  // Dev/test telemetry for automated UI tests.
  std::string lastViewFlashKey;
  bool lastViewOk = false;
  int lastViewCount = 0;
  bool chatWindowDrawn = false;
  std::string chatDockRegion;
};

State& S() {
  static State s;
  return s;
}

std::string formatTimeShort(uint64_t atMs) {
  if (atMs == 0) return "";
  const std::time_t t = static_cast<std::time_t>(atMs / 1000);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

ImU32 badgeColorFor(ChatRefKind k) {
  const auto& P = pal();
  switch (k) {
    case ChatRefKind::Clip: return P.accent;
    case ChatRefKind::Pattern: return P.hot;
    case ChatRefKind::Channel: return P.led;
    case ChatRefKind::Plugin: return theme::mix(P.accent, P.hot, 0.45f);
    case ChatRefKind::Param: return P.hot;
    case ChatRefKind::MixerTrack: return P.accent;
    case ChatRefKind::Notes: return P.led;
    case ChatRefKind::Marker: return P.hot;
    default: return P.textDim;
  }
}

ImU32 peerDisplayColor(const App& a, const ChatMessage& m) {
  uint32_t rgb = m.color;
  if (rgb == 0) rgb = collabui::memberColorRgb(a, m.site);
  if (rgb == 0) return pal().accent;
  return theme::rgb(rgb);
}

// Draws a compact rounded attachment card used in both the Chat feed and the Send-to-chat prompt.
// Returns true if the "View" button was clicked (only drawn when `showViewButton` is true).
bool drawAttachmentCard(App& a, const ChatAttachment& att, float width, bool showViewButton, const char* idSuffix) {
  const auto& P = pal();
  const ChatRefResolution res = resolveChatAttachment(a.doc.song(), att);
  const char* badgeText = tr(chatRefKindBadge(att.kind));
  const ImU32 badgeCol = res.exists ? badgeColorFor(att.kind) : P.textFaint;

  const float padX = 8.0f;
  const float padY = 6.0f;
  const float accentBarW = 3.0f;
  const float topRowH = 20.0f;
  const float titleH = theme::kFontBody + 2.0f;
  const std::string subText = res.exists ? res.detail : std::string(tr("Removed from project"));
  const bool hasSub = !subText.empty();
  const float subH = hasSub ? (theme::kFontCaption + 2.0f) : 0.0f;
  const float cardH = padY + topRowH + 3.0f + titleH + (hasSub ? (2.0f + subH) : 0.0f) + padY;

  ImGui::PushID(idSuffix);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + width, p0.y + cardH);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Card background and subtle border strictly from Theme palette roles.
  const ImU32 bgCol = P.dark ? P.surface800 : P.paper;
  const ImU32 borderCol = res.exists ? (P.dark ? P.line : P.lineStrong) : theme::withAlpha(P.line, 0.55f);
  dl->AddRectFilled(p0, p1, bgCol, theme::kRadius);
  dl->AddRect(p0, p1, borderCol, theme::kRadius, 0, 1.0f);
  dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + accentBarW, p1.y), badgeCol, theme::kRadius, ImDrawFlags_RoundCornersLeft);

  const float contentX = p0.x + accentBarW + padX;
  const float contentRight = p1.x - padX;
  const float topY = p0.y + padY;

  // Kind pill badge on the left of the top row.
  const ImVec2 badgeSz = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, badgeText);
  const float pillW = std::min(contentRight - contentX - (showViewButton ? 58.0f : 0.0f), badgeSz.x + 12.0f);
  const ImVec2 b0(contentX, topY + 1.0f);
  const ImVec2 b1(contentX + std::max(28.0f, pillW), topY + topRowH - 1.0f);
  dl->AddRectFilled(b0, b1, theme::withAlpha(badgeCol, P.dark ? 0.22f : 0.16f), theme::kRadiusSm);
  dl->AddRect(b0, b1, theme::withAlpha(badgeCol, P.dark ? 0.45f : 0.55f), theme::kRadiusSm, 0, 1.0f);
  const ImU32 badgeInk = res.exists ? theme::mix(P.text, badgeCol, P.dark ? 0.35f : 0.25f) : P.textFaint;
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption,
                      ImVec2(b0.x + 6.0f, theme::textTop(a.fontRegular, theme::kFontCaption, b0.y, b1.y - b0.y)),
                      b1.x - b0.x - 10.0f, badgeInk, badgeText);

  // "View" button right-aligned on the top row.
  bool viewClicked = false;
  if (showViewButton) {
    const char* viewLbl = tr("View");
    const ImVec2 vSz = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, viewLbl);
    const float btnW = std::max(44.0f, vSz.x + 16.0f);
    const float btnH = topRowH;
    const ImVec2 v0(std::max(b1.x + 6.0f, contentRight - btnW), topY);
    const ImVec2 v1(v0.x + btnW, v0.y + btnH);
    ImGui::SetCursorScreenPos(v0);
    if (ImGui::InvisibleButton("##viewbtn", ImVec2(btnW, btnH))) viewClicked = true;
    const bool hov = ImGui::IsItemHovered();
    const bool act = ImGui::IsItemActive();
    const ImU32 btnBg = !res.exists ? P.surface700
                        : act       ? theme::mix(P.accent, P.surface900, 0.2f)
                        : hov       ? P.accent
                                    : theme::withAlpha(P.accent, P.dark ? 0.24f : 0.16f);
    const ImU32 btnBorder = res.exists ? theme::withAlpha(P.accent, hov ? 0.95f : 0.55f) : P.line;
    const ImU32 btnInk = !res.exists ? P.textFaint : (hov || act ? theme::contrastText(P.accent) : P.text);
    dl->AddRectFilled(v0, v1, btnBg, theme::kRadiusSm);
    dl->AddRect(v0, v1, btnBorder, theme::kRadiusSm, 0, 1.0f);
    dl->AddText(a.fontRegular, theme::kFontCaption,
                ImVec2(std::round(v0.x + (btnW - vSz.x) * 0.5f), theme::textTop(a.fontRegular, theme::kFontCaption, v0.y, btnH)),
                btnInk, viewLbl);
    if (hov) {
      ui::Tooltip(res.exists ? tr("Jump to this item and highlight it") : tr("This item is no longer in the project."));
    }
  }

  // Target live name.
  const float titleY = topY + topRowH + 3.0f;
  const float availTextW = std::max(20.0f, contentRight - contentX);
  const ImU32 titleInk = res.exists ? P.text : P.textDim;
  theme::textEllipsis(dl, a.fontRegular, theme::kFontBody, ImVec2(contentX, titleY), availTextW, titleInk, res.title.c_str());

  // Optional detail / removed status line.
  if (hasSub) {
    const float subY = titleY + titleH + 2.0f;
    const ImU32 subInk = res.exists ? P.textDim : P.textFaint;
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(contentX, subY), availTextW, subInk, subText.c_str());
  }

  // Reserve exact layout space so ImGui advances cleanly without overlap.
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(ImVec2(width, cardH));
  if (ImGui::IsItemHovered() && !viewClicked) {
    std::string tip = res.title;
    if (!subText.empty()) tip += "\n" + subText;
    ui::Tooltip(tip.c_str());
  }
  ImGui::PopID();
  return viewClicked;
}

void drawSendPrompt(App& a) {
  State& st = S();
  collab::Session* sess = collabui::session(a);
  if (!sess) {
    st.promptPendingOpen = false;
    st.promptOpen = false;
    return;
  }

  const char* popupId = "Send to chat###send_to_chat_prompt";
  if (st.promptPendingOpen) {
    ImGui::OpenPopup(popupId);
    st.promptPendingOpen = false;
    st.promptOpen = true;
    st.promptFocusInput = true;
  }

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  if (vp) {
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.42f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  }
  ImGui::SetNextWindowSize(ImVec2(348.0f, 0.0f), ImGuiCond_Always);

  bool keepOpen = true;
  if (ImGui::BeginPopupModal(popupId, &keepOpen,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
    const auto& P = pal();
    const float innerW = std::max(260.0f, ImGui::GetContentRegionAvail().x);

    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Share this item with collaborators:"));
    ImGui::Dummy(ImVec2(0, theme::kS1));
    drawAttachmentCard(a, st.promptAtt, innerW, false, "prompt_preview");
    ImGui::Dummy(ImVec2(0, theme::kS2));

    ImGui::SetNextItemWidth(innerW);
    if (st.promptFocusInput) {
      ImGui::SetKeyboardFocusHere();
      st.promptFocusInput = false;
    }
    const bool enterSend = ImGui::InputTextWithHint("##send_comment", tr("Add an optional comment..."), st.promptComment,
                                                    sizeof st.promptComment, ImGuiInputTextFlags_EnterReturnsTrue);
    ui::FocusRing();
    ImGui::Dummy(ImVec2(0, theme::kS2));

    const float cancelW = ui::TextButtonWidth(tr("Cancel"));
    const float sendW = ui::TextButtonWidth(tr("Send"));
    const float rowW = cancelW + theme::kS2 + sendW;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, innerW - rowW));
    const bool cancelPressed = ui::TextButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SameLine(0, theme::kS2);
    const bool sendPressed = ui::PrimaryButton("Send") || enterSend;

    if (sendPressed) {
      std::string err;
      if (sess->sendChatMessage(st.promptComment, st.promptAtt, &err)) {
        st.promptComment[0] = 0;
        st.promptOpen = false;
        st.scrollToBottom = true;
        a.showChat = true;
        a.focusWindow = "Chat";
        ImGui::CloseCurrentPopup();
      } else if (!err.empty()) {
        a.notify(err, true);
      }
    } else if (cancelPressed || !keepOpen) {
      st.promptComment[0] = 0;
      st.promptOpen = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  } else {
    st.promptOpen = false;
  }
}

void drawChatWindow(App& a) {
  State& st = S();
  st.chatWindowDrawn = false;
  st.chatDockRegion.clear();

  collab::Session* sess = collabui::session(a);
  if (!sess) {
    a.showChat = false;
    return;
  }
  if (!a.showChat) return;

  const auto& P = pal();
  const size_t unread = sess->unreadChat();
  std::string title = ui::windowTitle("Chat");
  if (unread > 0) {
    const size_t hash = title.find("###");
    const std::string badge = " (" + std::to_string(unread) + ")";
    if (hash != std::string::npos) title.insert(hash, badge);
    else title += badge;
  }

  ImGui::SetNextWindowSize(ImVec2(280.0f, 520.0f), ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS2, theme::kS2));
  const bool vis = ImGui::Begin(title.c_str(), &a.showChat,
                                layoutui::dockFlags(title.c_str(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
  ImGui::PopStyleVar();

  if (vis) {
    st.chatWindowDrawn = true;
    if (ImGuiWindow* w = ImGui::GetCurrentWindow()) {
      if (w->DockNode && w->DockNode->HostWindow) st.chatDockRegion = ui::stableWindowName(w->DockNode->HostWindow->Name);
      else st.chatDockRegion = w->DockIsActive ? "docked" : "floating";
    }
    // User is actively viewing the Chat tab: clear unread count.
    if (unread > 0) sess->markChatRead();

    const float availW = std::max(140.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- Compact Header Strip ----
    {
      const ImVec2 hp0 = ImGui::GetCursorScreenPos();
      const float headH = 24.0f;
      int online = 1;  // local user
      for (const auto& [site, pv] : sess->peers()) {
        (void)site;
        if (pv.connected) ++online;
      }
      const ImU32 dotCol = online > 1 ? P.led : P.accent;
      dl->AddCircleFilled(ImVec2(hp0.x + 5.0f, hp0.y + headH * 0.5f), 3.5f, dotCol, 12);

      char onlineBuf[48];
      std::snprintf(onlineBuf, sizeof onlineBuf, tr("%d online"), online);
      const ImVec2 onSz = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, onlineBuf);
      const float onX = hp0.x + availW - onSz.x;
      const std::string hdr = tr("Project chat");
      theme::textEllipsis(dl, a.fontRegular, theme::kFontBody,
                          ImVec2(hp0.x + 15.0f, theme::textTop(a.fontRegular, theme::kFontBody, hp0.y, headH)),
                          std::max(30.0f, onX - (hp0.x + 22.0f)), P.text, hdr.c_str());
      dl->AddText(a.fontRegular, theme::kFontCaption,
                  ImVec2(onX, theme::textTop(a.fontRegular, theme::kFontCaption, hp0.y, headH)), P.textDim, onlineBuf);
      dl->AddLine(ImVec2(hp0.x, hp0.y + headH), ImVec2(hp0.x + availW, hp0.y + headH), P.line, 1.0f);
      ImGui::Dummy(ImVec2(availW, headH + theme::kS1));
    }

    // ---- Scrollable Message Feed ----
    const float frameH = ImGui::GetFrameHeight();
    const float composerH = frameH + theme::kS2 + 6.0f;
    const float feedH = std::max(80.0f, ImGui::GetContentRegionAvail().y - composerH);
    const std::vector<ChatMessage>& msgs = sess->chatMessages();
    if (msgs.size() != st.lastMsgCount) {
      if (msgs.size() > st.lastMsgCount) st.scrollToBottom = true;
      st.lastMsgCount = msgs.size();
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
    if (ImGui::BeginChild("##chat_feed", ImVec2(availW, feedH), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
      const float innerW = std::max(120.0f, ImGui::GetContentRegionAvail().x - 4.0f);
      if (msgs.empty()) {
        ImGui::Dummy(ImVec2(0, theme::kS4));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + innerW);
        ImGui::TextColored(theme::vec(P.textDim), "%s", tr("No messages yet."));
        ImGui::Dummy(ImVec2(0, theme::kS1));
        ImGui::TextColored(theme::vec(P.textFaint), "%s",
                           tr("Right-click any clip, pattern, channel, mixer track, plugin, knob, notes, or marker and choose Send to chat."));
        ImGui::PopTextWrapPos();
      } else {
        uint64_t prevSite = 0;
        uint64_t prevAtMs = 0;
        uint64_t prevShownMs = 0;
        for (size_t i = 0; i < msgs.size(); ++i) {
          const ChatMessage& m = msgs[i];
          const uint64_t shownMs = m.atMs > 0 ? std::max(prevShownMs, m.atMs) : 0;
          if (shownMs > 0) prevShownMs = shownMs;
          const bool grouped = (i > 0 && m.site == prevSite && shownMs >= prevAtMs && (shownMs - prevAtMs) < 120000ull);
          prevSite = m.site;
          prevAtMs = shownMs;

          ImGui::PushID(static_cast<int>(i));
          if (i > 0) ImGui::Dummy(ImVec2(0, grouped ? 4.0f : 10.0f));

          if (!grouped) {
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            ImDrawList* fdl = ImGui::GetWindowDrawList();
            const float rowH = theme::kFontBody + 2.0f;
            const ImU32 pCol = peerDisplayColor(a, m);
            fdl->AddCircleFilled(ImVec2(rp.x + 4.5f, rp.y + rowH * 0.5f), 3.5f, pCol, 12);

            const std::string tsStr = formatTimeShort(shownMs);
            const ImVec2 tsSz = tsStr.empty() ? ImVec2(0, 0) : a.fontMono->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, tsStr.c_str());
            const float tsX = rp.x + innerW - tsSz.x;
            const std::string authorLabel =
                (m.author.empty() ? std::string(tr("Collaborator")) : m.author) +
                (m.site == sess->site() ? (std::string(" (") + tr("you") + ")") : std::string());
            const ImU32 nameInk = theme::mix(P.text, pCol, P.dark ? 0.30f : 0.25f);
            theme::textEllipsis(fdl, a.fontRegular, theme::kFontBody,
                                ImVec2(rp.x + 13.0f, theme::textTop(a.fontRegular, theme::kFontBody, rp.y, rowH)),
                                std::max(30.0f, tsX - (rp.x + 19.0f)), nameInk, authorLabel.c_str());
            if (!tsStr.empty()) {
              fdl->AddText(a.fontMono, theme::kFontCaption,
                           ImVec2(tsX, theme::textTop(a.fontMono, theme::kFontCaption, rp.y, rowH)),
                           P.dark ? P.textFaint : P.textDim, tsStr.c_str());
            }
            ImGui::Dummy(ImVec2(innerW, rowH));
          }

          if (!m.text.empty()) {
            ImGui::Indent(13.0f);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(40.0f, innerW - 15.0f));
            ImGui::TextColored(theme::vec(P.text), "%s", m.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Unindent(13.0f);
          }

          if (m.hasAttachment()) {
            ImGui::Dummy(ImVec2(0, 3.0f));
            ImGui::Indent(13.0f);
            const float cardW = std::max(80.0f, innerW - 15.0f);
            if (drawAttachmentCard(a, m.att, cardW, true, "msg_att")) {
              viewAttachment(a, m.att);
            }
            ImGui::Unindent(13.0f);
          }
          ImGui::PopID();
        }
      }
      if (st.scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        st.scrollToBottom = false;
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- Bottom Composer Bar ----
    {
      const ImVec2 cp = ImGui::GetCursorScreenPos();
      dl->AddLine(ImVec2(cp.x, cp.y + 2.0f), ImVec2(cp.x + availW, cp.y + 2.0f), P.line, 1.0f);
      ImGui::Dummy(ImVec2(0, 6.0f));

      const char* sendLbl = tr("Send");
      const float sendW = std::max(44.0f, a.fontRegular->CalcTextSizeA(theme::kFontBody, 1e9f, 0, sendLbl).x + 16.0f);
      const float inW = std::max(60.0f, availW - sendW - theme::kS1);
      ImGui::SetNextItemWidth(inW);
      if (st.focusInputNext) {
        ImGui::SetKeyboardFocusHere();
        st.focusInputNext = false;
      }
      const bool enterPressed = ImGui::InputTextWithHint("##chat_input", tr("Message chat..."), st.inputBuf,
                                                         sizeof st.inputBuf, ImGuiInputTextFlags_EnterReturnsTrue);
      ui::FocusRing();
      ImGui::SameLine(0, theme::kS1);
      const bool hasText = !cleanChatText(st.inputBuf).empty();
      const bool clickSend = ui::PrimaryButton("Send", sendW, nullptr, hasText);

      if (hasText && (enterPressed || clickSend)) {
        std::string err;
        if (sess->sendChatMessage(st.inputBuf, {}, &err)) {
          st.inputBuf[0] = 0;
          st.scrollToBottom = true;
          if (enterPressed) st.focusInputNext = true;
        } else if (!err.empty()) {
          a.notify(err, true);
        }
      }
    }
  }
  ImGui::End();
}

}  // namespace

std::string paramFlashKey(const ParamId& pid) {
  return "param/#" + std::to_string(pid.owner) + "/" + std::to_string(pid.index);
}
std::string slotFlashKey(ObjectId mixerId, int slot) {
  return "slot/#" + std::to_string(mixerId) + "/" + std::to_string(slot);
}
std::string pluginFlashKey(ObjectId ownerId, int slot) {
  return "plugin/#" + std::to_string(ownerId) + "/" + std::to_string(slot);
}
std::string markerFlashKey(Tick pos) { return "marker/#" + std::to_string(pos); }
std::string patternFlashKey(ObjectId patId) { return "patterns/#" + std::to_string(patId); }

void frame(App& a, double now) {
  if (!collabui::active(a)) {
    a.showChat = false;
    State& st = S();
    st.promptPendingOpen = false;
    st.promptOpen = false;
    st.noteFlash = {};
    st.lastMsgCount = 0;
    return;
  }
  State& st = S();
  if (st.noteFlash.until > 0 && now >= st.noteFlash.until) st.noteFlash = {};
}

void draw(App& a) {
  if (a.embedded) return;
  drawChatWindow(a);
  drawSendPrompt(a);
}

bool sendToChatMenuItem(App& a, const ChatAttachment& att, bool separatorBefore) {
  if (!collabui::active(a) || !att.valid()) return false;
  if (separatorBefore) ImGui::Separator();
  if (ImGui::MenuItem(tr("Send to chat..."))) {
    promptSendToChat(a, att);
    return true;
  }
  return false;
}

void promptSendToChat(App& a, const ChatAttachment& att) {
  if (!collabui::active(a) || !att.valid()) return;
  State& st = S();
  st.promptAtt = att;
  st.promptComment[0] = 0;
  st.promptPendingOpen = true;
}

bool viewAttachment(App& a, const ChatAttachment& att) {
  State& st = S();
  st.lastViewOk = false;
  st.lastViewFlashKey.clear();
  ++st.lastViewCount;

  const Song& s = a.doc.song();
  const ChatRefResolution res = resolveChatAttachment(s, att);
  if (!att.valid() || !res.exists) {
    a.notify(tr("This item is no longer in the project."), false);
    return false;
  }

  constexpr double kViewFlashSec = 2.4;
  // 0 = theme accent/hot highlight colour for unmistakable visual clarity.
  constexpr uint32_t kFlashRgb = 0;

  switch (att.kind) {
    case ChatRefKind::Clip: {
      const Clip* c = s.clip(att.id);
      if (!c) {
        a.notify(tr("This item is no longer in the project."), false);
        return false;
      }
      a.showPlaylist = true;
      a.focusWindow = "Playlist";
      a.playlist.selected = {c->id};
      a.playlist.scrollX = std::max(0.0f, static_cast<float>(c->start) * a.playlist.pxPerTick - 120.0f);
      a.playlist.scrollY = std::max(0.0f, static_cast<float>(std::max(0, c->track)) * a.playlist.trackHeight - 80.0f);
      const std::string key = "playlist/#" + std::to_string(c->id);
      collabui::triggerFlash(a, key, kFlashRgb, kViewFlashSec);
      st.lastViewFlashKey = key;
      st.lastViewOk = true;
      return true;
    }
    case ChatRefKind::Pattern: {
      const Pattern* p = s.pattern(att.id);
      if (!p) {
        a.notify(tr("This item is no longer in the project."), false);
        return false;
      }
      a.selectPattern(p->id);
      a.playlist.pickerTab = 0;
      a.playlist.pickerSel = {p->id};
      a.showPiano = true;
      a.focusWindow = "Piano roll";
      const std::string key = patternFlashKey(p->id);
      collabui::triggerFlash(a, key, kFlashRgb, kViewFlashSec);
      st.lastViewFlashKey = key;
      st.lastViewOk = true;
      return true;
    }
    case ChatRefKind::Channel: {
      const Channel* ch = s.channel(att.id);
      if (!ch) {
        a.notify(tr("This item is no longer in the project."), false);
        return false;
      }
      a.selectChannel(ch->id);
      if (layoutui::active() == layout::Preset::Studio) {
        a.showRack = true;
        a.focusWindow = "Channel rack";
      } else {
        anchor::Target t;
        t.kind = anchor::Kind::Channel;
        t.id = ch->id;
        layoutui::reveal(a, t);
      }
      const std::string key = "channels/#" + std::to_string(ch->id);
      collabui::triggerFlash(a, key, kFlashRgb, kViewFlashSec);
      st.lastViewFlashKey = key;
      st.lastViewOk = true;
      return true;
    }
    case ChatRefKind::Plugin: {
      if (att.sub < 0) {
        const Channel* ch = s.channel(att.id);
        if (!ch) {
          a.notify(tr("This item is no longer in the project."), false);
          return false;
        }
        a.selectChannel(ch->id);
        a.settingsChannel = ch->id;
        a.channelTab = 0;
        a.focusWindow = "###channelsettings";
        const std::string key = pluginFlashKey(ch->id, -1);
        collabui::triggerFlash(a, key, kFlashRgb, kViewFlashSec);
        collabui::triggerFlash(a, "channels/#" + std::to_string(ch->id), kFlashRgb, kViewFlashSec);
        st.lastViewFlashKey = key;
        st.lastViewOk = true;
        return true;
      }
      if (att.sub >= 0 && att.sub < kEffectSlots) {
        const MixerTrack* mt = s.mixerTrack(att.id);
        if (!mt || mt->slots[static_cast<size_t>(att.sub)].plugin.type.empty()) {
          a.notify(tr("This item is no longer in the project."), false);
          return false;
        }
        a.selMixer = mt->id;
        a.showMixer = true;
        a.fxEditor = {mt->id, att.sub};
        a.focusWindow = "###fxeditor";
        const std::string key = slotFlashKey(mt->id, att.sub);
        collabui::triggerFlash(a, key, kFlashRgb, kViewFlashSec);
        collabui::triggerFlash(a, pluginFlashKey(mt->id, att.sub), kFlashRgb, kViewFlashSec);
        collabui::triggerFlash(a, "mixer/#" + std::to_string(mt->id), kFlashRgb, kViewFlashSec);
        st.lastViewFlashKey = key;
        st.lastViewOk = true;
        return true;
      }
      break;
    }
    case ChatRefKind::Param: {
      if (att.sub < 0) {
        a.notify(tr("This item is no longer in the project."), false);
        return false;
      }
      const ParamId pid{att.id, static_cast<uint32_t>(att.sub)};
      if (!describeParam(s, pid)) {
        a.notify(tr("This item is no longer in the project."), false);
        return false;
      }
      if (pid.owner == kSongId) {
        // Song parameters live in the Top Bar (always visible).
      } else if (s.channel(pid.owner)) {
        a.selectChannel(pid.owner);
        if (pid.index >= kChannelPluginBase) {
          a.settingsChannel = pid.owner;
          a.channelTab = 0;
          a.focusWindow = "###channelsettings";
          collabui::triggerFlash(a, pluginFlashKey(pid.owner, -1), kFlashRgb, kViewFlashSec);
        } else {
          a.showRack = true;
          a.focusWindow = "Channel rack";
          collabui::triggerFlash(a, "channels/#" + std::to_string(pid.owner), kFlashRgb, kViewFlashSec);
        }
      } else if (s.mixerTrack(pid.owner)) {
        a.selMixer = pid.owner;
        a.showMixer = true;
        if (pid.index >= kSlotBase) {
          const int slot = static_cast<int>((pid.index - kSlotBase) / kSlotStride);
          const uint32_t subIdx = (pid.index - kSlotBase) % kSlotStride;
          if (slot >= 0 && slot < kEffectSlots) {
            if (subIdx == kSlotMix || subIdx == kSlotEnable) {
              a.focusWindow = "Mixer";
              collabui::triggerFlash(a, slotFlashKey(pid.owner, slot), kFlashRgb, kViewFlashSec);
            } else {
              a.fxEditor = {pid.owner, slot};
              a.focusWindow = "###fxeditor";
              collabui::triggerFlash(a, pluginFlashKey(pid.owner, slot), kFlashRgb, kViewFlashSec);
            }
          }
        } else {
          a.focusWindow = "Mixer";
          collabui::triggerFlash(a, "mixer/#" + std::to_string(pid.owner), kFlashRgb, kViewFlashSec);
        }
      }
      const std::string key = paramFlashKey(pid);
      collabui::triggerFlash(a, key, kFlashRgb, kViewFlashSec);
      st.lastViewFlashKey = key;
      st.lastViewOk = true;
      return true;
    }
    case ChatRefKind::MixerTrack: {
      if (s.mixerIndex(att.id) < 0) {
        a.notify(tr("This item is no longer in the project."), false);
        return false;
      }
      a.selMixer = att.id;
      a.showMixer = true;
      a.focusWindow = "Mixer";
      const std::string key = "mixer/#" + std::to_string(att.id);
      collabui::triggerFlash(a, key, kFlashRgb, kViewFlashSec);
      st.lastViewFlashKey = key;
      st.lastViewOk = true;
      return true;
    }
    case ChatRefKind::Notes: {
      const Pattern* p = s.pattern(att.id);
      const Channel* ch = s.channel(att.extra);
      if (!p || !ch) {
        a.notify(tr("This item is no longer in the project."), false);
        return false;
      }
      a.selectPattern(p->id);
      a.selectChannel(ch->id);
      a.showPiano = true;
      a.focusWindow = "Piano roll";
      a.piano.selected.clear();
      a.piano.selPattern = p->id;
      a.piano.selChannel = ch->id;
      if (auto it = p->notes.find(ch->id); it != p->notes.end()) {
        for (size_t i = 0; i < it->second.size(); ++i) {
          const Note& n = it->second[i];
          const Tick nEnd = n.start + std::max<Tick>(1, n.length);
          const bool timeHit = att.tick1 > att.tick0 ? (n.start < att.tick1 && nEnd > att.tick0) : (n.start == att.tick0);
          const bool pitchHit = att.pitchHi >= att.pitchLo ? (n.pitch >= att.pitchLo && n.pitch <= att.pitchHi) : true;
          if (timeHit && pitchHit) a.piano.selected.insert(i);
        }
      }
      a.piano.scrollX = std::max(0.0f, static_cast<float>(att.tick0) * a.piano.pxPerTick - 96.0f);
      const float midPitch = 0.5f * static_cast<float>(att.pitchLo + att.pitchHi);
      a.piano.scrollY = std::max(0.0f, (127.0f - midPitch) * a.piano.rowHeight - 160.0f);
      a.piano.centred = true;
      st.noteFlash = NoteFlash{p->id, ch->id, att.tick0, att.tick1, att.pitchLo, att.pitchHi, a.now + kViewFlashSec, kViewFlashSec, kFlashRgb};
      const std::string key = "notes/#" + std::to_string(p->id) + "/" + std::to_string(ch->id);
      collabui::triggerFlash(a, key, kFlashRgb, kViewFlashSec);
      collabui::triggerFlash(a, patternFlashKey(p->id), kFlashRgb, kViewFlashSec);
      st.lastViewFlashKey = key;
      st.lastViewOk = true;
      return true;
    }
    case ChatRefKind::Marker: {
      const Marker* found = nullptr;
      for (const Marker& mk : s.markers) {
        if (mk.pos == att.tick0) {
          found = &mk;
          break;
        }
      }
      if (!found && !att.label.empty()) {
        for (const Marker& mk : s.markers) {
          if (mk.name == att.label) {
            found = &mk;
            break;
          }
        }
      }
      if (!found) {
        a.notify(tr("This item is no longer in the project."), false);
        return false;
      }
      a.showPlaylist = true;
      a.focusWindow = "Playlist";
      a.playlist.scrollX = std::max(0.0f, static_cast<float>(found->pos) * a.playlist.pxPerTick - 120.0f);
      const std::string key = markerFlashKey(found->pos);
      collabui::triggerFlash(a, key, kFlashRgb, kViewFlashSec);
      st.lastViewFlashKey = key;
      st.lastViewOk = true;
      return true;
    }
    default: break;
  }
  return false;
}

float noteFlash(const App& a, ObjectId patId, ObjectId chId, const Note& n, ImU32* color) {
  if (!collabui::active(a)) return 0.0f;
  const State& st = S();
  const NoteFlash& nf = st.noteFlash;
  if (nf.until <= a.now || nf.patId != patId || nf.chId != chId) return 0.0f;
  const Tick nEnd = n.start + std::max<Tick>(1, n.length);
  const bool timeHit = nf.tick1 > nf.tick0 ? (n.start < nf.tick1 && nEnd > nf.tick0) : (n.start == nf.tick0);
  const bool pitchHit = nf.pitchHi >= nf.pitchLo ? (n.pitch >= nf.pitchLo && n.pitch <= nf.pitchHi) : true;
  if (!timeHit || !pitchHit) return 0.0f;
  if (color) *color = nf.colorRgb ? theme::rgb(nf.colorRgb) : pal().hot;
  return static_cast<float>(std::clamp((nf.until - a.now) / std::max(0.1, nf.duration), 0.0, 1.0));
}

bool devCommand(App& a, const std::vector<std::string>& args, int frame) {
  if (args.empty()) return false;
  const std::string& cmd = args[0];
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
  auto joinFrom = [&](size_t i) {
    std::string out;
    for (size_t k = i; k < args.size(); ++k) {
      if (k > i) out += ' ';
      out += args[k];
    }
    return out;
  };
  auto buildAtt = [&](const std::string& kindStr, int idx, int sub) -> ChatAttachment {
    const Song& s = a.doc.song();
    if (kindStr == "clip" && idx >= 0 && static_cast<size_t>(idx) < s.playlist.size())
      return makeClipAttachment(s, s.playlist[static_cast<size_t>(idx)].id);
    if (kindStr == "pattern" && idx >= 0 && static_cast<size_t>(idx) < s.patterns.size())
      return makePatternAttachment(s, s.patterns[static_cast<size_t>(idx)].id);
    if (kindStr == "channel" && idx >= 0 && static_cast<size_t>(idx) < s.channels.size())
      return makeChannelAttachment(s, s.channels[static_cast<size_t>(idx)].id);
    if (kindStr == "mixer" && idx >= 0 && static_cast<size_t>(idx) < s.mixer.size())
      return makeMixerTrackAttachment(s, s.mixer[static_cast<size_t>(idx)].id);
    if (kindStr == "plugin") {
      if (sub < 0 && idx >= 0 && static_cast<size_t>(idx) < s.channels.size())
        return makePluginAttachment(s, s.channels[static_cast<size_t>(idx)].id, -1);
      if (sub >= 0 && idx >= 0 && static_cast<size_t>(idx) < s.mixer.size())
        return makePluginAttachment(s, s.mixer[static_cast<size_t>(idx)].id, sub);
    }
    if (kindStr == "param") {
      if (idx < 0) return makeParamAttachment(s, songParam(SongParam::Tempo));
      if (static_cast<size_t>(idx) < s.channels.size())
        return makeParamAttachment(s, channelParam(s.channels[static_cast<size_t>(idx)].id, ChannelParam::Volume));
    }
    if (kindStr == "notes" && !s.patterns.empty() && !s.channels.empty()) {
      const size_t pi = static_cast<size_t>(std::clamp(idx, 0, static_cast<int>(s.patterns.size()) - 1));
      const size_t ci = static_cast<size_t>(std::clamp(sub, 0, static_cast<int>(s.channels.size()) - 1));
      return makeNotesAttachment(s, s.patterns[pi].id, s.channels[ci].id, {});
    }
    if (kindStr == "marker" && idx >= 0 && static_cast<size_t>(idx) < s.markers.size())
      return makeMarkerAttachment(s, static_cast<size_t>(idx));
    return {};
  };

  if (cmd == "chat-open") {
    a.showChat = true;
    a.focusWindow = "Chat";
    return true;
  }
  if (cmd == "chat-close") {
    a.showChat = false;
    return true;
  }
  if (cmd == "chat-send") {
    if (collab::Session* sess = collabui::session(a)) {
      sess->sendChatMessage(joinFrom(1));
      S().scrollToBottom = true;
    }
    return true;
  }
  if (cmd == "chat-send-ref") {
    if (collab::Session* sess = collabui::session(a)) {
      const std::string kind = arg(1);
      const int idx = std::atoi(arg(2).c_str());
      const int sub = args.size() > 3 ? std::atoi(arg(3).c_str()) : -1;
      const std::string comment = args.size() > 4 ? joinFrom(4) : std::string();
      const ChatAttachment att = buildAtt(kind, idx, sub);
      if (att.valid()) {
        sess->sendChatMessage(comment, att);
        S().scrollToBottom = true;
      }
    }
    return true;
  }
  if (cmd == "chat-prompt") {
    const std::string kind = arg(1);
    const int idx = std::atoi(arg(2).c_str());
    const int sub = args.size() > 3 ? std::atoi(arg(3).c_str()) : -1;
    const ChatAttachment att = buildAtt(kind, idx, sub);
    if (att.valid()) {
      promptSendToChat(a, att);
      if (args.size() > 4) std::snprintf(S().promptComment, sizeof S().promptComment, "%s", joinFrom(4).c_str());
    }
    return true;
  }
  if (cmd == "chat-view") {
    if (collab::Session* sess = collabui::session(a)) {
      const int idx = std::atoi(arg(1).c_str());
      const auto& msgs = sess->chatMessages();
      if (idx >= 0 && static_cast<size_t>(idx) < msgs.size() && msgs[static_cast<size_t>(idx)].hasAttachment()) {
        viewAttachment(a, msgs[static_cast<size_t>(idx)].att);
      }
    }
    return true;
  }
  if (cmd == "chat-report") {
    const std::string outPath = arg(1);
    const collab::Session* sess = collabui::session(a);
    const State& st = S();
    std::string text;
    text += "frame=" + std::to_string(frame) + "\n";
    text += "active=" + std::string(collabui::active(a) ? "1" : "0") + "\n";
    text += "showChat=" + std::string(a.showChat ? "1" : "0") + "\n";
    text += "chatDrawn=" + std::string(st.chatWindowDrawn ? "1" : "0") + "\n";
    text += "chatDock=" + (st.chatDockRegion.empty() ? std::string("none") : st.chatDockRegion) + "\n";
    text += "messages=" + std::to_string(sess ? sess->chatMessages().size() : 0) + "\n";
    text += "unread=" + std::to_string(sess ? sess->unreadChat() : 0) + "\n";
    text += "undoSize=" + std::to_string(a.doc.history().size()) + "\n";
    text += "lastViewOk=" + std::string(st.lastViewOk ? "1" : "0") + "\n";
    text += "lastViewFlashKey=" + st.lastViewFlashKey + "\n";
    text += "flashActive=" + std::string(collabui::hasFlash(a, st.lastViewFlashKey) ? "1" : "0") + "\n";
    text += "showPlaylist=" + std::string(a.showPlaylist ? "1" : "0") + "\n";
    text += "showPiano=" + std::string(a.showPiano ? "1" : "0") + "\n";
    text += "showRack=" + std::string(a.showRack ? "1" : "0") + "\n";
    text += "showMixer=" + std::string(a.showMixer ? "1" : "0") + "\n";
    text += "fxEditorTrack=" + std::to_string(a.fxEditor.first) + "\n";
    text += "fxEditorSlot=" + std::to_string(a.fxEditor.second) + "\n";
    text += "settingsChannel=" + std::to_string(a.settingsChannel) + "\n";
    if (!outPath.empty()) {
      std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
      f << text;
    } else {
      std::printf("%s", text.c_str());
      std::fflush(stdout);
    }
    return true;
  }
  return false;
}

}  // namespace gl::collabchat
