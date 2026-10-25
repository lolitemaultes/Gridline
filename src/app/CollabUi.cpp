// Gridline - J-111: collaboration in the app. See CollabUi.h.
#include "CollabUi.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "../collab/Controller.h"
#include "../collab/Lifecycle.h"
#include "../collab/LocalState.h"
#include "../collab/NetTest.h"
#include "../collab/VersionGate.h"
#include "../core/I18n.h"
#include "../core/Platform.h"
#include "../core/Png.h"
#include "../core/Project.h"
#include "../core/ProjectFiles.h"
#include "../core/ProjectInterop.h"
#include "../core/Secrets.h"
#include "../core/Settings.h"
#include "../host/ui/NativeView.h"
#include "App.h"
#include "CursorAnchor.h"
#include "DevSignal.h"
#include "DialogLayout.h"
#include "GridlineVersion.h"
#include "HostedUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "UpdateUi.h"
#include "Widgets.h"

namespace fs = std::filesystem;

namespace gl::collabui {

using i18n::tr;
using theme::pal;
using namespace gl::collab;

namespace {

constexpr float kPanelW = 520.0f;     // Collaboration / Join windows: default width
constexpr float kPanelMinW = 440.0f;  // ... and the narrowest they get (Sync review too)
constexpr double kFlashSec = 1.2;

struct NetTestJob {
  std::mutex mu;
  bool done = false;
  NetReport report;
};

struct Ui {
  // invite
  int inviteMinutes = 60;
  std::string inviteId;
  bool scrollToInvite = false;  // "Invite again": bring the invite section into view
  char replyBuf[8192] = {};
  // reconnect
  SiteId reconnectPeer = 0;
  char reconnectBuf[8192] = {};
  // join
  char codeBuf[8192] = {};
  std::string lastParsed;
  InviteInfo parsed;
  bool parsedOk = false;
  std::string parseErr;
  std::string joinFolder;
  std::string joinErr;
  bool joining = false;
  // make collaborative (J-111h: "Share this project", inline in the panel: the name and the folder)
  std::string makeErr;
  char shareName[512] = {};
  std::string shareParent;   // where the project's folder is made
  std::string shareFor;      // the project file the fields were filled in for
  bool shareInit = false;
  bool shareFocus = false;
  std::string shareChecked;  // the folder last looked at, and what was there
  double shareCheckedAt = -1;
  bool shareTaken = false, shareExists = false;
  // renaming the project (J-111f; J-111h: inline): 0 not, 1 in the panel's header, 2 the Shared name row
  // (Project info, Project settings)
  int renameAt = 0;
  char projNameBuf[512] = {};
  bool nameFocus = false;
  int renameFrame = 0;  // the frame it opened on (J-111i: the header's field gives up if it never got focus)
  bool alsoFolder = false;
  std::string nameErr;
  std::vector<std::pair<std::string, std::string>> foreign;
  // review
  int reviewTab = 0;
  std::string previewObject;
  SiteId previewSite = 0;
  // net test
  std::shared_ptr<NetTestJob> netTest;
  NetReport lastReport;
  bool haveReport = false;
  // flashes of remote edits
  struct FlashMark {
    double until = 0;
    uint32_t color = 0;
  };
  std::map<std::string, FlashMark> flashes;
  // avatars
  struct Tex {
    GLuint id = 0;
  };
  std::map<std::string, Tex> avatars;
  // presence
  Presence presence;
  struct Shown {  // a collaborator's cursor as drawn: eased towards the reported spot (J-111d)
    std::string window;      // window it was last shown in
    Presence last;           // the spot last reported (idle: unchanged)
    ImVec2 pos{0, 0};        // drawn hotspot
    ImVec2 target{0, 0};     // where the anchor is here
    bool placed = false;
    float vis = 0;           // 0..1: fades out where the spot is not shown here, back in where it is
    ImVec2 ghostPos{0, 0};   // jumped to another window: fades out where it was
    float ghostVis = 0;
    float idle = 1;          // dims after a few seconds without moving
    double movedAt = -1;
    bool want = false;       // the spot is shown here
    std::string why;         // dev reports: shown / window / off-view / not-drawn / window-hidden / outside / offline /
                             // other-plugin / plugin-window (J-111g)
    double hiddenSince = -1; // J-111g: since when the spot is not shown here (-1: it is); a moment: no fade
    bool holding = false;    // J-111g: hidden for a moment only, still drawn where it was
    bool snapping = false;   // J-111g: moving to another window: quick, no fade
    bool crossing = false;   // J-111g: a far jump: fading in quickly at the new spot
    bool occluded = false;   // J-111g: its hotspot is under a plugin editor open here (clipped there)
    float minVis = 1;        // J-111g: the lowest visibility since the last trace reset (dev)
    ImGuiWindow* root = nullptr;  // window it is drawn over
    ImGuiWindow* ghostRoot = nullptr;  // J-111h: the window the fading copy (crossfade) is over
    float covered = 0;       // J-111h: how much of the arrow's box windows in front of it cover here (0..1, dev)
    size_t probe = 0;        // its probe this frame (CursorAnchor: what it is over here)
    ImVec2 fillProbe{0, 0};  // a point of the coloured fill (dev pixel check)
    int drawnFrame = -1;
  };
  std::map<SiteId, Shown> shownCursors;
  double cursorsAt = -1;
  float pointerH = 0;          // J-111g: the system pointer arrow's height, logical units (setPointerSize)
  std::string pointerSource;
  // J-111g: avatars drawn as pictures this frame (dev reports): hash -> centre and radius of the largest
  struct DrawnAvatar {
    ImVec2 c{0, 0};
    float r = 0;
    int frame = -1;
  };
  std::map<std::string, DrawnAvatar> drawnAvatars;
  // follow a collaborator's playhead
  SiteId follow = 0;
  // version refusal waiting to be shown
  bool refusalPending = false;
  Hub::Refusal refusal;
  // profile editing
  char nameBuf[128] = {};
  char emailBuf[256] = {};
  bool profileInit = false;
  std::string avatarErr;
  bool confirmReset = false;
  // dev demo: a simulated collaborator
  struct Demo {
    std::shared_ptr<MemFabric::Shared> net;
    Document doc;
    std::unique_ptr<ProfileStore> profiles;
    std::unique_ptr<Session> session;
    std::unique_ptr<Hub> hub;
    std::string folder;
    std::string invite;
    int step = 0;
    double at = 0;
    bool quietReview = false;  // screenshots of the editors: the review does not pop up
    std::string removeReason;  // J-111e: "collab demo-remove-me"
    bool cursorSet = false;    // "collab demo-cursor": Sam's pointer held on this spot
    Presence cursor;
  };
  std::unique_ptr<Demo> demo;
  // a newer release appeared while the session runs
  std::string bannerVersion;
  bool bannerClosed = false;
  double now = 0;
  // scripted runs (--dev-input "collab ..."): actions waiting for a file or the network
  struct Dev {
    std::string inviteFile, inviteId;
    std::string joinCodeFile, joinFolder;
    std::string reconnectOut, reconnectIn;
    std::string pixelFile;  // "collab pixel": the colour under the first shown cursor, after drawing
    std::string picturesFile;  // J-111g "collab pictures": members' pictures, after drawing
    bool sectionProbe = false;  // J-111h "collab section-probe": a window that sizes itself to a full-width section
  } dev;
  // J-111e: removing someone, leaving, stopping, the files at the end
  SiteId removeTarget = 0;      // the Remove confirmation is open for them
  char removeReason[301] = {};
  std::string removeErr;
  bool showGone = false;        // People: removed members and people who left
  std::string reinviteFor;      // the invite on screen re-admits them
  int leaveDialog = 0;          // 1 Leave, 2 Stop sharing, 3 Make it a normal project (abandoned)
  int leaveMode = 0;            // 0 keep the folder as a normal project, 1 move it to the trash
  bool leaveWait = false;       // "Wait for sync": leaves once a collaborator has our changes
  std::string leaveErr, leaveName;
  bool cleanupOpen = false;     // Clean up unused audio
  std::vector<std::pair<std::string, uint64_t>> unused;
  uint64_t unusedBytes = 0;
  struct Sizes {
    std::string folder;
    uint64_t total = 0, audio = 0, history = 0;
    double at = -100;
  } sizes;
  bool exitChoosing = false;    // the folder chooser of "Keep a private copy" is up
  std::string exitErr;
  std::string dupFolder, dupOf;  // open refused: a copy of a project that is also elsewhere
};

Ui& U() {
  static Ui u;
  return u;
}

std::string fmt1(const char* f, const std::string& a) {
  char buf[512];
  std::snprintf(buf, sizeof buf, f, a.c_str());
  return buf;
}

std::string fmt2(const char* f, const std::string& a, const std::string& b) {
  char buf[512];
  std::snprintf(buf, sizeof buf, f, a.c_str(), b.c_str());
  return buf;
}

std::string ago(uint64_t ms, uint64_t nowMs) {
  if (!ms) return tr("never");
  const int64_t s = static_cast<int64_t>(nowMs > ms ? (nowMs - ms) / 1000 : 0);
  char b[64];
  if (s < 10) return tr("just now");
  if (s < 60) std::snprintf(b, sizeof b, tr("%d s ago"), static_cast<int>(s));
  else if (s < 3600) std::snprintf(b, sizeof b, tr("%d min ago"), static_cast<int>(s / 60));
  else if (s < 86400) std::snprintf(b, sizeof b, tr("%d h ago"), static_cast<int>(s / 3600));
  else std::snprintf(b, sizeof b, tr("%d days ago"), static_cast<int>(s / 86400));
  return b;
}

std::string clockTime(uint64_t ms, bool approx) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[64];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
  return approx ? std::string(tr("about")) + " " + b : std::string(b);
}

uint64_t wallMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

ImU32 colorOf(uint32_t rgb) { return theme::rgb(rgb ? rgb : 0x8FA0B5); }

void label(const char* text, float x = 168.0f) { ui::FormLabelFit(tr(text), x, pal().textDim); }

void dim(const std::string& t, ImU32 col = 0) {
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(col ? col : pal().textDim), "%s", t.c_str());
  ImGui::PopTextWrapPos();
}

// ---- Settings-style building blocks (the Settings window's language: recessed
// sections with mono captions, a dim label column, short captions, lit primary
// buttons, danger buttons for what cannot be undone).

// Short secondary text under a row (the Settings caption: faint, wrapped).
void note(const std::string& t, ImU32 col = 0) { dim(t, col ? col : pal().textFaint); }

// Label column of a window's form rows: the widest label plus a gap, from the
// body's left edge (sections indent kS2).
float formX(std::initializer_list<const char*> labels, float minX = 96.0f, float maxX = 168.0f) {
  float w = 0;
  for (const char* l : labels) w = std::max(w, ImGui::CalcTextSize(tr(l)).x);
  return std::clamp(theme::kS2 + w + theme::kS3, minX, maxX);
}

// Start of a window's scrolling body (Settings' "##page"): everything under
// the header scrolls, `footer` px stay below it for the footer buttons.
void beginBody(const char* id, float footer = 0) {
  ImGui::BeginChild(id, ImVec2(0, footer > 0 ? -footer : 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
}

void projectTitle(App& a, const std::string& title, float maxW);
bool renamingTitle();
void nameCaption(const char* typed, const std::string& clean, const char* ok);

// Window header: title (Inter SemiBold 16) and one caption line under it;
// `right` is drawn right-aligned on the title line (a small status).
// `renamable`: the title is the collaborative project's name, renamed in place (projectTitle).
void header(App& a, const std::string& title, const std::string& sub, const std::string& right = std::string(), ImU32 rightCol = 0,
            bool renamable = false) {
  const auto& P = pal();
  const float avail = ImGui::GetContentRegionAvail().x;
  float rw = 0;
  if (!right.empty()) rw = ImGui::CalcTextSize(right.c_str()).x + theme::kS3;
  const float x0 = ImGui::GetCursorPosX();
  const float tw = std::max(40.0f, avail - rw);
  if (renamable) {
    projectTitle(a, title, tw);
  } else {
    ImGui::PushFont(a.fontBold, theme::kFontTitle);
    ui::TextFit(title.c_str(), tw, P.text);
    ImGui::PopFont();
  }
  if (!right.empty()) {
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 2 * theme::kS3, x0 + avail - rw + theme::kS3));  // right-aligned
    ImGui::TextColored(theme::vec(rightCol ? rightCol : P.textDim), "%s", right.c_str());
  }
  if (renamable && renamingTitle())
    nameCaption(U().projNameBuf, cleanProjectName(U().projNameBuf), tr("Enter or clicking away renames it for everyone; Esc keeps the old name."));
  else if (!sub.empty())
    dim(sub);
  ImGui::Dummy(ImVec2(0, theme::kS1));
}

// A tinted strip across the width: a status lamp, one line of text and an
// optional button on the right. Returns true when the button is clicked.
bool noticeBar(const char* id, const std::string& text, ImU32 tone, const char* button = nullptr, bool primary = true) {
  const auto& P = pal();
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight() + 2 * theme::kS1;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::mix(P.surface800, tone, P.dark ? 0.14f : 0.10f), theme::kRadiusLg);
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), theme::withAlpha(tone, 0.45f), theme::kRadiusLg);
  const float bw = button ? ui::TextButtonWidth(button) : 0.0f;
  ImGui::SetCursorScreenPos(ImVec2(p.x + theme::kS3, p.y + theme::kS1));
  ImGui::AlignTextToFramePadding();
  ui::StatusDot(tone);
  ui::TextFit(text.c_str(), std::max(40.0f, w - 2 * theme::kS3 - 14 - (button ? bw + theme::kS2 : 0.0f)), P.text);
  bool clicked = false;
  if (button) {
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - theme::kS1 - bw, p.y + theme::kS1));
    clicked = ui::TextButton(button, primary, bw);
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  ImGui::Dummy(ImVec2(w, 0));
  ImGui::PopID();
  return clicked;
}

// A status line: lamp + text (fitted, full text on hover).
void statusLine(const std::string& text, ImU32 dot, ImU32 col = 0, bool pulse = false) {
  ImGui::AlignTextToFramePadding();
  ui::StatusDot(dot, pulse);
  ui::TextFit(text.c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x), col ? col : pal().text);
}

// Places the cursor so `width` px of buttons end at the right edge of the line.
void alignRight(float width) {
  const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
}

float buttonsWidth(std::initializer_list<const char*> labels) {
  float w = 0;
  for (const char* l : labels) w += ui::TextButtonWidth(l);
  return w + theme::kS2 * static_cast<float>(labels.size() > 0 ? labels.size() - 1 : 0);
}

// A hairline across the section (between list rows, before a section's actions).
void hairline(float gapAbove = theme::kS1, float gapBelow = theme::kS1) {
  ImGui::Dummy(ImVec2(0, std::max(0.0f, gapAbove - ImGui::GetStyle().ItemSpacing.y)));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), pal().line);
  ImGui::Dummy(ImVec2(w, std::max(1.0f, gapBelow - ImGui::GetStyle().ItemSpacing.y)));
}

// Small pill after a name ("you", "admin").
float badge(App& a, ImDrawList* dl, ImVec2 pos, const char* text, ImU32 fg, ImU32 bg) {
  const float fs = theme::kFontLabel, padX = 5.0f, h = 16.0f;
  const ImVec2 ts = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, text);
  dl->AddRectFilled(pos, ImVec2(pos.x + ts.x + 2 * padX, pos.y + h), bg, theme::kRadius);
  dl->AddText(a.fontRegular, fs, ImVec2(pos.x + padX, pos.y + (h - ts.y) * 0.5f), fg, text);
  return ts.x + 2 * padX;
}

// Read-only code box (invite / reply / reconnect codes): mono on the field
// colour, wrapped, `lines` lines tall.
void copyField(App& a, const char* id, const std::string& text, int lines) {
  const auto& P = pal();
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(P.field));
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  std::string buf = text;
  buf.resize(text.size() + 1);
  const float h = ImGui::GetTextLineHeight() * static_cast<float>(lines) + ImGui::GetStyle().FramePadding.y * 2 + 2;
  ImGui::InputTextMultiline(id, &buf[0], buf.size(), ImVec2(-1, h), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
  ImGui::PopFont();
  ImGui::PopStyleColor();
}

// The safety code (four groups of four digits) large, each group in its own
// field box, so both people can read it out and compare.
void safetyDigits(App& a, const std::string& code) {
  const auto& P = pal();
  std::vector<std::string> groups;
  size_t i = 0;
  while (i < code.size()) {
    const size_t j = code.find(' ', i);
    if (j != i) groups.push_back(code.substr(i, j == std::string::npos ? std::string::npos : j - i));
    if (j == std::string::npos) break;
    i = j + 1;
  }
  if (groups.empty()) return;
  ImGui::PushFont(a.fontMonoBold ? a.fontMonoBold : a.fontMono, theme::kFontLcd);
  const ImVec2 gs = ImGui::CalcTextSize("0000");
  ImGui::PopFont();
  const float bw = gs.x + 2 * theme::kS3, bh = gs.y + 2 * theme::kS2, avail = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float x0 = p.x;
  float bottom = p.y;
  for (const std::string& g : groups) {
    if (p.x > x0 && p.x + bw > x0 + avail) p = ImVec2(x0, p.y + bh + theme::kS2);
    dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh), P.field, theme::kRadius);
    dl->AddRect(p, ImVec2(p.x + bw, p.y + bh), P.line, theme::kRadius);
    const ImVec2 ts = (a.fontMonoBold ? a.fontMonoBold : a.fontMono)->CalcTextSizeA(theme::kFontLcd, 1e9f, 0, g.c_str());
    dl->AddText(a.fontMonoBold ? a.fontMonoBold : a.fontMono, theme::kFontLcd, ImVec2(p.x + (bw - ts.x) * 0.5f, p.y + (bh - ts.y) * 0.5f), P.text, g.c_str());
    bottom = p.y + bh;
    p.x += bw + theme::kS2;
  }
  ImGui::Dummy(ImVec2(std::min(avail, (bw + theme::kS2) * static_cast<float>(groups.size())), bottom - ImGui::GetCursorScreenPos().y));
}

// "59:12" / "1:02:03" (time left of an invite).
std::string countdown(int64_t seconds) {
  seconds = std::max<int64_t>(0, seconds);
  char b[32];
  if (seconds >= 3600)
    std::snprintf(b, sizeof b, "%d:%02d:%02d", static_cast<int>(seconds / 3600), static_cast<int>(seconds / 60 % 60), static_cast<int>(seconds % 60));
  else
    std::snprintf(b, sizeof b, "%d:%02d", static_cast<int>(seconds / 60), static_cast<int>(seconds % 60));
  return b;
}

// Animated "..." for work in progress.
std::string dots() { return std::string(static_cast<size_t>(static_cast<int>(ImGui::GetTime() * 3.0) % 4), '.'); }

// ---- avatars: the picture in a circle, or initials on the collaborator's colour
ImTextureID avatarTexture(const std::string& jpeg, std::string* hashOut = nullptr) {
  if (jpeg.empty()) return ImTextureID_Invalid;
  Ui& u = U();
  Profile tmp;
  tmp.avatar = jpeg;
  const std::string h = tmp.avatarHash();
  if (hashOut) *hashOut = h;
  auto it = u.avatars.find(h);
  if (it != u.avatars.end()) return it->second.id ? static_cast<ImTextureID>(it->second.id) : ImTextureID_Invalid;
  Ui::Tex t;
  int w = 0, hh = 0;
  std::vector<uint8_t> rgba;
  if (decodeAvatar(jpeg, w, hh, rgba)) {
    GLint last = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, hh, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
  }
  u.avatars[h] = t;
  return t.id ? static_cast<ImTextureID>(t.id) : ImTextureID_Invalid;
}

void drawAvatar(App& a, ImDrawList* dl, ImVec2 c, float r, const std::string& name, uint32_t color, const std::string& jpeg, int online) {
  const auto& P = pal();
  const ImU32 col = colorOf(color);
  std::string hash;
  const ImTextureID tex = avatarTexture(jpeg, &hash);
  if (tex != ImTextureID_Invalid) {
    dl->AddImageRounded(tex, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
    Ui::DrawnAvatar& d = U().drawnAvatars[hash];  // (dev reports: which pictures are on screen, where)
    if (d.frame != ImGui::GetFrameCount() || r > d.r) d = Ui::DrawnAvatar{c, r, ImGui::GetFrameCount()};
  } else {
    dl->AddCircleFilled(c, r, col, 24);
    const std::string ini = initials(name);
    const float fs = std::max(8.0f, r * 0.95f);
    const ImVec2 ts = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, ini.c_str());
    dl->AddText(a.fontBold, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), theme::contrastText(col), ini.c_str());
  }
  dl->AddCircle(c, r + 1.0f, col, 24, 1.5f);
  if (online >= 0) {  // presence dot: green online, grey away
    const ImVec2 d(c.x + r * 0.72f, c.y + r * 0.72f);
    dl->AddCircleFilled(d, std::max(2.5f, r * 0.30f), P.chrome, 12);
    dl->AddCircleFilled(d, std::max(1.8f, r * 0.22f), online > 0 ? P.led : P.textFaint, 12);
  }
}

// ---- readable change descriptions ("Volume 78% -> 55%", "deleted clip 'Vox 2'")
std::string objectName(const Song& s, const std::string& object) {
  const Path p = splitPath(object);
  if (!p.empty() && p[0] == "meta") return tr("Project name");  // J-111f: meta/project/name
  if (p.size() < 3) return p.size() > 1 ? p[1] : object;
  const std::string& kind = p[1];
  const ObjectId id = p[2].size() > 1 && p[2][0] == '#' ? static_cast<ObjectId>(std::strtoul(p[2].c_str() + 1, nullptr, 10)) : kNoId;
  if (kind == "channels") {
    const Channel* c = s.channel(id);
    return fmt1(tr("Channel '%s'"), c ? c->name : std::string(tr("(deleted)")));
  }
  if (kind == "patterns") {
    const Pattern* x = s.pattern(id);
    return fmt1(tr("Pattern '%s'"), x ? x->name : std::string(tr("(deleted)")));
  }
  if (kind == "mixer") {
    const int i = s.mixerIndex(id);
    return fmt1(tr("Mixer track '%s'"), i >= 0 ? mixerTrackName(s, static_cast<size_t>(i)) : std::string(tr("(deleted)")));
  }
  if (kind == "playlist") {
    const Clip* c = s.clip(id);
    std::string n = tr("clip");
    if (c && c->kind == ClipKind::Pattern)
      if (const Pattern* x = s.pattern(c->pattern)) n = x->name;
    if (c && c->kind == ClipKind::Audio)
      if (const Channel* ch = s.channel(c->source)) n = ch->name;
    return fmt1(tr("Clip '%s'"), n);
  }
  if (kind == "automations") {
    const AutomationClip* x = s.automation(id);
    return fmt1(tr("Automation '%s'"), x ? x->name : std::string(tr("(deleted)")));
  }
  return kind;
}

std::string valueText(const Json& v) {
  if (v.is(Json::Type::Number)) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v.number);
    return b;
  }
  if (v.is(Json::Type::String)) return "'" + v.str.substr(0, 40) + "'";
  if (v.is(Json::Type::Bool)) return v.boolean ? tr("on") : tr("off");
  return tr("(none)");
}

std::string describe(const Song& s, const SideOp& op) {
  const Path& p = op.path;
  if (!p.empty() && p[0] == "meta") {  // J-111f: the project's name
    if (op.prev.is(Json::Type::String) && !op.prev.str.empty())
      return fmt2(tr("renamed the project from %s to %s"), valueText(op.prev), valueText(op.value));
    return fmt1(tr("named the project %s"), valueText(op.value));
  }
  const std::string last = p.empty() ? std::string() : p.back();
  // well-known properties with their units
  auto pct = [](const Json& v) {
    char b[24];
    std::snprintf(b, sizeof b, "%.0f%%", v.is(Json::Type::Number) ? v.number * 100.0 : 0.0);
    return std::string(b);
  };
  if (op.kind == OpKind::Del) return p.size() <= 3 ? fmt1(tr("deleted %s"), objectName(s, joinPath(Path(p.begin(), p.begin() + std::min<size_t>(p.size(), 3))))) : fmt1(tr("removed %s"), last);
  if (op.kind == OpKind::Ins) return p.size() <= 3 ? tr("added it") : fmt1(tr("added %s"), last.find('.') != std::string::npos || last[0] == '#' ? std::string(tr("an item")) : last);
  if (op.kind == OpKind::Undel) return tr("restored it");
  if (op.kind == OpKind::Mov) return tr("moved it");
  static const std::map<std::string, const char*> names = {
      {"name", i18n::N_("Name")},   {"gain", i18n::N_("Volume")},   {"volume", i18n::N_("Volume")}, {"pan", i18n::N_("Pan")},
      {"muted", i18n::N_("Mute")},  {"pitch", i18n::N_("Pitch")},   {"start", i18n::N_("Position")}, {"track", i18n::N_("Track")},
      {"length", i18n::N_("Length")}, {"color", i18n::N_("Colour")}, {"tempo", i18n::N_("Tempo")},  {"velocity", i18n::N_("Velocity")},
      {"mix", i18n::N_("Mix")},     {"enabled", i18n::N_("On")},     {"level", i18n::N_("Send level")}};
  std::string field = last;
  if (auto it = names.find(last); it != names.end()) field = tr(it->second);
  // plugin parameters: name and value from the plugin's descriptors
  if (p.size() >= 2 && p[p.size() - 2] == "params") {
    for (size_t i = 0; i + 1 < p.size(); ++i)
      if (p[i] == "plugin") field = tr("Plugin parameter");
  }
  const bool percent = last == "gain" || last == "volume" || last == "mix" || last == "level";
  const std::string before = percent ? pct(op.prev) : valueText(op.prev), after = percent ? pct(op.value) : valueText(op.value);
  return field + "  " + before + " \xE2\x86\x92 " + after;
}

// ---- controller lifetime
Controller& ctlOf(App& a) {
  if (!a.collab) {
    a.collab = std::make_unique<Controller>((fs::path(a.configDir()) / "collab").string());
    a.collab->net = netSettingsFrom(a.settings);
    a.collab->onNotice = [&a](const std::string& t, bool e) { a.notify(t, e); };
    a.collab->onRemoteChange = [&a](const std::vector<Flash>& fl) {
      Ui& u = U();
      Session* s = a.collab ? a.collab->session() : nullptr;
      for (const auto& f : fl) {
        uint32_t c = 0;
        if (s)
          if (PeerView* pv = s->peer(f.site)) c = pv->profile.color;
        u.flashes[f.object] = Ui::FlashMark{u.now + kFlashSec, c};
      }
    };
    a.collab->onRenamed = [&a](const std::string&, const std::string& after, SiteId by) {  // J-111f
      Session* s = a.collab ? a.collab->session() : nullptr;
      if (!s) return;
      if (by == s->site()) {
        a.notify(fmt1(tr("The project is now called %s for everyone."), after));
        return;
      }
      std::string who;
      if (const PeerView* pv = s->peer(by); pv && !pv->profile.name.empty()) who = pv->profile.name;
      if (who.empty())
        if (const Member* m = s->member(by)) who = m->name;
      a.notify(fmt2(tr("%s renamed the project to %s"), who.empty() ? std::string(tr("A collaborator")) : who, after));
    };
    a.collab->onVersionRefusal = [](const Hub::Refusal& r) {
      Ui& u = U();
      u.refusal = r;
      u.refusalPending = true;
    };
  }
  return *a.collab;
}

// J-111g: the picture to show for a member: this user's own; a collaborator's as
// their profile names it in this session (the bytes received, or kept on this
// computer from before); before they said anything, the one kept from before.
std::string pictureOf(App& a, Session& s, const Member& m) {
  if (m.site == s.site()) return s.config().profile.avatar;
  const PeerView* pv = s.peer(m.site);
  const Profile* known = ctlOf(a).profiles().known(m.fingerprint());
  if (pv && !pv->profile.name.empty()) {
    if (!pv->profile.avatar.empty()) return pv->profile.avatar;
    if (pv->profile.avatarRef.empty()) return {};  // they have no picture (or removed it)
    if (std::string b = s.avatarFor(pv->profile.avatarRef); !b.empty()) return b;
  }
  return known ? known->avatar : std::string();  // (a changed picture: the old one until the new one is here)
}

void afterProjectReplaced(App& a) {
  const Song& s = a.doc.song();
  a.curPattern = s.patterns.empty() ? kNoId : s.patterns[0].id;
  a.selChannel = s.channels.empty() ? kNoId : s.channels[0].id;
  a.selChannels = {a.selChannel};
  a.selMixer = s.masterId();
  a.playlist.selected.clear();
  a.engine.setMode(s.playlist.empty() ? PlayMode::Pattern : PlayMode::Song);
  a.syncEngine();
}

// Folder chooser (the in-app file dialog in folder mode).
void chooseFolder(App& a, const char* title, const std::string& start, std::function<void(const std::string&)> done) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = title;
  a.fileDialog.dir = start.empty() ? (platform::homeEnv() ? platform::homeEnv() : ".") : start;
  a.fileDialog.exts = {"/"};
  a.fileDialog.onOk = std::move(done);
}

std::string safeFolderName(std::string n) {
  for (char& c : n)
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '-';
  n = cleanName(n);
  return n.empty() ? std::string("Collaborative project") : n;
}

// The version gate as a notice above the start / join buttons. Returns true when collaboration is allowed.
bool gateLine(App& a) {
  const GateState& g = updui::gate(a);
  const auto& P = pal();
  if (g.kind == GateState::Kind::UpdateRequired) {
    if (noticeBar("##gate", fmt1(tr("Update to Gridline %s to use collaborative features."), g.latest), P.hot, "Update"))
      updui::showRequired(a, fmt1(tr("Update to Gridline %s to collaborate"), g.latest), g.latest);
    ImGui::Dummy(ImVec2(0, theme::kS1));
    return false;
  }
  if (updui::gateChecking() && g.latest.empty()) {
    statusLine(tr("Checking for updates..."), P.textFaint, P.textDim, true);
  } else if (g.couldNotCheck) {
    statusLine(tr("Couldn't check for updates"), P.textFaint, P.textDim);
    ui::Tooltip("Collaborators on other versions are refused when you connect.");
  }
  return true;
}

void saveCodeFile(App& a, const std::string& code, const char* name) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.save = true;
  a.fileDialog.title = "Save the code as a file";
  a.fileDialog.dir = platform::homeEnv() ? platform::homeEnv() : ".";
  a.fileDialog.name = name;
  a.fileDialog.exts = {".txt"};
  a.fileDialog.onOk = [&a, code](const std::string& p) {
    std::ofstream(p) << code << "\n";
    a.notify("Saved " + fs::path(p).filename().string());
  };
}

// ---- windows ------------------------------------------------------------------
// The open project becomes collaborative in `folder` (file <name>.gridline.json).
bool makeCollaborativeAt(App& a, const std::string& folder, const std::string& name) {
  Ui& uu = U();
  std::error_code ec;
  if (fs::exists(fs::path(folder) / ".gridline-collab", ec)) {
    uu.makeErr = tr("That folder already holds a collaborative project.");
    return false;
  }
  std::string err;
  if (!ctlOf(a).create(a.doc, folder, name, &err)) {
    uu.makeErr = err;
    return false;
  }
  uu.makeErr.clear();
  a.projectPath = ctlOf(a).session()->projectFile();
  a.projectFormat = SaveFormat::Gridline;
  a.openedFrom.clear();
  a.addRecent(a.projectPath);
  a.doc.markSaved();
  a.notify(fmt1(tr("%s is now a collaborative project in its own folder; your original project is unchanged. Invite someone from the Collaboration panel."), name));
  return true;
}

// J-111h: making a project collaborative happens in the panel ("Share this project": the name and
// the folder, filled in for the open project), not in a window of its own.
// The fields, filled in once per project: its name, and the folder its file is in.
void shareDefaults(App& a, bool force) {
  Ui& u = U();
  const std::string ref = a.projectFile();
  if (!force && u.shareInit && u.shareFor == ref) return;
  u.shareInit = true;
  u.shareFor = ref;
  const std::string title = a.doc.song().info.title;
  const std::string name = !ref.empty() ? projectStemOf(ref) : !title.empty() && title != "Untitled" ? title : std::string(tr("Collaborative project"));
  std::snprintf(u.shareName, sizeof u.shareName, "%s", cleanProjectName(name).c_str());
  u.shareParent = !ref.empty() ? fs::path(ref).parent_path().string() : std::string(platform::homeEnv() ? platform::homeEnv() : ".");
  u.shareChecked.clear();
  u.makeErr.clear();
  u.foreign.clear();
}

// File > New collaborative project... (and "collab name-window"): the panel, its name field focused.
void makeCollaborative(App& a) {
  a.showCollab = true;
  shareDefaults(a, true);
  U().shareFocus = true;
}

// The folder "Share this project" makes for `name`.
std::string shareTarget(const std::string& name) {
  return (fs::path(U().shareParent) / projectFileStem(name.empty() ? std::string("Project") : name)).string();
}

// "Make collaborative": with the name and the folder of the panel.
bool shareNow(App& a) {
  Ui& u = U();
  Controller& c = ctlOf(a);
  if (!c.profileReady()) {
    a.settingsTab = 11;
    a.showSettings = true;
    a.notify(tr("Choose a display name in Settings > Collaboration first."));
    return false;
  }
  u.foreign = foreignPlugins(songToJson(a.doc.song(), false));
  if (!u.foreign.empty()) return false;  // listed in the panel
  const std::string name = cleanProjectName(u.shareName);
  if (name.empty()) return false;
  if (!makeCollaborativeAt(a, shareTarget(name), name)) return false;
  u.shareInit = false;  // filled in again for the next project
  return true;
}

// The caption under a project name field: why the name can't be used as typed, else `ok`.
void nameCaption(const char* typed, const std::string& clean, const char* ok) {
  const auto& P = pal();
  size_t n = 0;  // characters as typed (UTF-8 aware)
  for (const char* q = typed; *q; ++q) n += (static_cast<unsigned char>(*q) & 0xC0) != 0x80;
  ImGui::AlignTextToFramePadding();
  if (clean.empty()) {
    ImGui::TextColored(theme::vec(theme::lit(P.danger)), "%s", tr("A project needs a name."));
  } else if (n > kMaxProjectNameChars) {
    char b[96];
    std::snprintf(b, sizeof b, tr("Names are at most %d characters: the rest is left out."), static_cast<int>(kMaxProjectNameChars));
    dim(b, P.hot);
  } else {
    dim(ok, P.textFaint);
  }
}

void openRename(App& a, int where) {
  Ui& u = U();
  Session* s = a.collab ? a.collab->session() : nullptr;
  if (!s || !s->ready() || s->stopped()) return;
  std::snprintf(u.projNameBuf, sizeof u.projNameBuf, "%s", s->projectName().c_str());
  u.renameAt = where;
  u.nameFocus = true;
  u.renameFrame = ImGui::GetFrameCount();
  u.alsoFolder = false;
  u.nameErr.clear();
}

// After the folder was renamed (the session reopened from it).
void afterFolderRenamed(App& a, const std::string& oldFile) {
  Session* s = a.collab ? a.collab->session() : nullptr;
  if (!s) return;
  a.recentFiles.erase(std::remove(a.recentFiles.begin(), a.recentFiles.end(), oldFile), a.recentFiles.end());
  a.projectPath = s->projectFile();
  a.addRecent(a.projectPath);
  afterProjectReplaced(a);
}

// Renames the project to `name` (and this computer's folder when asked); closes the rename when done.
void commitRename(App& a, const std::string& name) {
  Ui& u = U();
  Controller& c = ctlOf(a);
  std::string err;
  if (!c.rename(name, &err)) {
    u.nameErr = err;
    return;
  }
  if (u.alsoFolder && !c.folderForName().empty()) {
    const std::string oldFile = a.projectPath;
    a.engine.stopToStart();
    a.engine.panic(false);
    std::string folder;
    if (!c.renameFolder(a.doc, &folder, &err)) {
      u.nameErr = err;
      if (c.session()) afterFolderRenamed(a, oldFile);  // reopened where it was
      else a.newProject();
      return;
    }
    afterFolderRenamed(a, oldFile);
    a.notify(fmt1(tr("The project folder is now %s"), folder));
  }
  u.renameAt = 0;
}

// J-111h: renaming in place (the panel's header, the Shared name row): the name field with Rename and
// Cancel on its line, then what changes, the opt-in for this computer's folder and any error, each
// row starting at `x` (the row's label column; 0: the line's start).
void renameEditor(App& a, float x) {
  Ui& u = U();
  const auto& P = pal();
  Session* s = ctlOf(a).session();
  if (!s || !s->ready() || s->stopped()) {
    u.renameAt = 0;
    return;
  }
  const std::string name = cleanProjectName(u.projNameBuf);
  const std::string stem = projectFileStem(name.empty() ? std::string("Project") : name);
  const fs::path folder = fs::path(s->config().folder).lexically_normal();
  const fs::path target = folder.parent_path() / stem;
  const bool differs = target.filename() != folder.filename();
  const bool nothing = name == s->projectName() && !(u.alsoFolder && differs);  // the same name, the folder stays
  const float okW = ui::TextButtonWidth("Rename##inline"), cw = ui::TextButtonWidth("Cancel##rename");
  ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - okW - cw - 2 * theme::kS2));
  if (u.nameFocus) {
    ImGui::SetKeyboardFocusHere();
    u.nameFocus = false;
  }
  const bool enter = ImGui::InputTextWithHint("##projname", tr("Project name"), u.projNameBuf, sizeof u.projNameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
  const bool escape = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape);
  ui::FocusRing();
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(name.empty() || nothing);
  const bool go = ui::TextButton("Rename##inline", true, okW, "Renames the project for everyone") || (enter && !name.empty() && !nothing);
  ImGui::EndDisabled();
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Cancel##rename", false, cw) || escape) {
    u.renameAt = 0;
    return;
  }
  auto row = [&] {
    if (x > 0) ui::FormLabelFit("", x, P.textDim);
  };
  row();
  nameCaption(u.projNameBuf, name, tr("Everyone sees the new name: now if they are online, or the next time you are online together."));
  row();
  ImGui::AlignTextToFramePadding();
  dim(fmt1(tr("Each computer's project file becomes %s."), stem + kProjectFileExt), P.textFaint);
  row();
  ImGui::BeginDisabled(!differs);
  bool also = u.alsoFolder && differs;
  if (ImGui::Checkbox(tr("Also rename the project folder on this computer"), &also)) u.alsoFolder = also;
  ImGui::EndDisabled();
  row();
  if (!differs)
    dim(tr("The folder already has this name."), P.textFaint);
  else if (also)
    dim(fmt1(tr("The folder becomes \"%s\". Only your copy: collaborators' folders stay as they are. Gridline reopens the project and reconnects."),
             target.filename().string()),
        P.textFaint);
  else
    dim(fmt1(tr("Folders keep their names (yours is \"%s\")."), folder.filename().string()), P.textFaint);
  if (!u.nameErr.empty()) {
    row();
    statusLine(u.nameErr, P.danger, theme::lit(P.danger));
  }
  if (go && u.renameAt != 0) commitRename(a, name);
}

bool renamingTitle() { return U().renameAt == 1; }

// Ends renaming in the panel's header: `keep` renames the project to what the field holds (when it
// is a new, valid name), otherwise the old name stays.
void finishTitleRename(App& a, bool keep) {
  Ui& u = U();
  u.renameAt = 0;
  u.nameFocus = false;
  Session* s = a.collab ? a.collab->session() : nullptr;
  if (!keep || !s || !s->ready() || s->stopped()) return;
  const std::string name = cleanProjectName(u.projNameBuf);
  if (name.empty() || name == s->projectName()) return;
  std::string err;
  if (!ctlOf(a).rename(name, &err)) a.notify(err, true);
}

// J-111i: the Collaboration panel's title is the project's name, renamed in place. Hovering shows a
// faint box around it, a click turns it into a field (the same text in the same place, so nothing
// moves); Enter or clicking away renames the project for everyone, Escape keeps the old name. It
// takes the title's line and no more, at most `maxW` wide. (The project folder is renamed from the
// Shared name row in Project info, which has that option.)
void projectTitle(App& a, const std::string& title, float maxW) {
  Ui& u = U();
  const auto& P = pal();
  Session* s = a.collab ? a.collab->session() : nullptr;
  const bool can = s && s->ready() && !s->stopped();
  if (!can && u.renameAt == 1) u.renameAt = 0;
  const ImVec2 pad(theme::kS1 + 1, 2);  // the box around the text
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  if (u.renameAt != 1) {
    ui::TextFit(title.c_str(), maxW, P.text);
    ImGui::PopFont();
    if (!can) return;
    const ImVec2 t0 = ImGui::GetItemRectMin(), t1 = ImGui::GetItemRectMax();
    const ImRect bb(t0.x - pad.x, t0.y - pad.y, t1.x + pad.x, t1.y + pad.y);
    const ImGuiID id = win->GetID("##renametitle");
    if (!ImGui::ItemAdd(bb, id)) return;  // over the text, without taking room of its own
    bool hovered = false, held = false;
    if (ImGui::ButtonBehavior(bb, id, &hovered, &held)) openRename(a, 1);
    if (hovered || held) {
      dl->AddRect(bb.Min, bb.Max, P.lineStrong, theme::kRadius);
      ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
      ui::Tooltip("Click to rename the project for everyone");
    }
    return;
  }
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float lh = ImGui::GetTextLineHeight();
  ImGui::SetCursorScreenPos(ImVec2(p.x - pad.x, p.y - pad.y));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, pad);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kRadius);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, P.field);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, P.field);
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, P.field);
  ImGui::SetNextItemWidth(maxW + pad.x);
  if (u.nameFocus) ImGui::SetKeyboardFocusHere();
  ImGui::InputText("##projtitle", u.projNameBuf, sizeof u.projNameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
  const bool active = ImGui::IsItemActive(), gone = ImGui::IsItemDeactivated();
  const bool escape = gone && ImGui::IsKeyPressed(ImGuiKey_Escape);
  dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), active ? P.accent : P.lineStrong, theme::kRadius, 0, active ? 1.5f : 1.0f);
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(maxW, lh));  // the title's line, as when it is not being renamed
  if (active) u.nameFocus = false;
  else if (u.nameFocus) {
    if (ImGui::GetFrameCount() > u.renameFrame + 3) finishTitleRename(a, false);  // it never got focus
  } else if (gone) {
    finishTitleRename(a, !escape);  // Enter or clicked away: renamed; Escape: as it was
  } else {
    finishTitleRename(a, true);  // focus went elsewhere without a deactivation (another window took it)
  }
}

// A new invite (row action "Invite again"): unless one is already waiting.
void inviteAgain(Hub& hub) {
  Ui& u = U();
  const Hub::InviteView* iv = u.inviteId.empty() ? nullptr : hub.invite(u.inviteId);
  const bool waiting = iv && (iv->status == Hub::InviteStatus::Preparing || iv->status == Hub::InviteStatus::Ready ||
                              iv->status == Hub::InviteStatus::Connecting);
  if (!waiting) {
    u.inviteId = hub.createInvite(u.inviteMinutes);
    u.reinviteFor.clear();
  }
  u.scrollToInvite = true;
}

// A member's link in words, with its lamp colour.
struct MemberStatus {
  std::string text, more;  // `more`: extra detail for the tooltip
  ImU32 dot = 0;
  bool pulse = false, online = false, failed = false;
};

MemberStatus memberStatus(Session& s, const Member& m, const PeerView* pv, const Hub::LinkView& lv) {
  const auto& P = pal();
  MemberStatus st;
  const std::string dotSep = "  \xC2\xB7  ";
  if (m.site == s.site()) {
    st.text = std::string(tr("Online")) + dotSep + tr("this computer");
    st.dot = P.led;
    st.online = true;
    return st;
  }
  const std::string synced = std::string(tr("last synced")) + " " + ago(pv ? pv->lastSyncMs : 0, wallMs());
  switch (lv.state) {
    case Hub::LinkView::State::Direct: {
      st.text = std::string(tr("Online")) + dotSep + tr("direct");
      if (lv.rttMs >= 0) {
        char b[32];
        std::snprintf(b, sizeof b, "%.0f ms", lv.rttMs);
        st.text += dotSep + b;
      }
      st.more = fmt1(tr("Connection: %s"), lv.path.empty() ? std::string("ice") : lv.path);
      st.dot = P.led;
      st.online = true;
      break;
    }
    case Hub::LinkView::State::Relayed:
      st.text = std::string(tr("Online")) + dotSep + fmt1(tr("relayed through %s"), lv.via.empty() ? std::string(tr("a collaborator")) : lv.via);
      st.dot = theme::litRole(theme::role::kSlip);
      st.online = true;
      break;
    case Hub::LinkView::State::Connecting:
      st.text = tr("Connecting...");
      st.dot = P.hot;
      st.pulse = true;
      break;
    case Hub::LinkView::State::Waiting:
      st.text = std::string(tr("Offline")) + dotSep + synced;
      st.more = tr("Reconnects when they open the project.");
      st.dot = P.textFaint;
      break;
    case Hub::LinkView::State::Failed:
      st.text = std::string(tr("Couldn't connect directly")) + dotSep + synced;
      st.dot = P.danger;
      st.failed = true;
      break;
    default:
      st.text = std::string(tr("Offline")) + dotSep + synced;
      st.dot = P.textFaint;
      break;
  }
  if (pv && pv->syncTotal > 0) {
    char b[128];
    std::snprintf(b, sizeof b, tr("Syncing %d changes"), static_cast<int>(pv->syncTotal));
    st.text = b;
    st.dot = P.accent;
    st.pulse = true;
  } else if (pv && pv->unseen > 0) {
    char b[128];
    std::snprintf(b, sizeof b, tr("%d of your changes not seen yet"), static_cast<int>(pv->unseen));
    st.text += dotSep + b;
  }
  return st;
}

void memberMenu(Session& s, Hub* hub, const Member& m, bool me, bool inviteItem) {
  Ui& u = U();
  if (ImGui::MenuItem(tr("Copy fingerprint"))) ImGui::SetClipboardText(m.fingerprint().c_str());
  if (!me && s.isAdmin() && ImGui::MenuItem(tr("Make admin"))) s.transferAdmin(m.site);
  if (!me && ImGui::MenuItem(tr("Follow their playhead"), nullptr, u.follow == m.site)) u.follow = u.follow == m.site ? 0 : m.site;
  if (!me && ImGui::MenuItem(tr("Reconnect with a code..."))) u.reconnectPeer = m.site;
  if (inviteItem && hub && ImGui::MenuItem(tr("Invite again"))) inviteAgain(*hub);
  // J-111e: the admin removes someone (not themselves: they leave, after handing admin over)
  if (!me && s.isAdmin() && s.ready()) {
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Remove from project..."))) {
      u.removeTarget = m.site;
      u.removeReason[0] = 0;
      u.removeErr.clear();
    }
  }
}

// J-111e: someone who is no longer in the project (removed by an admin, or left):
// dimmed, who / when / why, and the way back (an admin's re-invite).
void drawGoneMember(App& a, Session& s, Hub* hub, const Member& m, float w) {
  Ui& u = U();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const MemberState st = s.memberState(m.site);
  const Removal* r = s.openRemoval(m.site);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float rowH = 44, fh = ImGui::GetFrameHeight(), avR = 15, textX = 2 * avR + theme::kS2 + theme::kS3;
  ImGui::PushID(static_cast<int>(m.site & 0x7fffffff));
  const bool removed = st == MemberState::Removed;
  const char* btn = removed ? "Re-invite..." : "Invite again";
  const bool canAct = hub && s.ready() && (removed ? s.isAdmin() : true);
  const float bw = canAct ? ui::TextButtonWidth(btn) : 0.0f;
  const float textW = std::max(40.0f, w - textX - (canAct ? bw + theme::kS2 : 0.0f) - theme::kS1);
  const std::string name = m.name.empty() ? std::string(tr("(no name)")) : m.name;
  drawAvatar(a, dl, ImVec2(p.x + theme::kS1 + avR, p.y + rowH * 0.5f), avR, name, defaultColor(m.signPublic), std::string(), -1);
  dl->AddCircleFilled(ImVec2(p.x + theme::kS1 + avR, p.y + rowH * 0.5f), avR + 1.5f, theme::withAlpha(P.surface800, 0.55f), 24);
  theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(p.x + textX, p.y + 5), textW, P.textDim, name.c_str());
  std::string line;
  if (removed && r) {
    line = fmt2(tr("Removed by %s, %s"), r->byName.empty() ? std::string(tr("the admin")) : r->byName, clockTime(r->at, false));
    if (!r->reason.empty()) line += "  \xC2\xB7  \xE2\x80\x9C" + r->reason + "\xE2\x80\x9D";
  } else {
    line = tr("Left the project");
  }
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(p.x + textX, p.y + 25), textW, P.textFaint, line.c_str());
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##gone", ImVec2(std::max(1.0f, w - (canAct ? bw + theme::kS2 : 0.0f)), rowH));
  if (ImGui::IsItemHovered()) {
    std::string tip = std::string(tr("Fingerprint")) + ": " + m.fingerprint() + "\n" + line;
    tip += "\n";
    tip += removed ? tr("Their changes from before stay, with their name in Sync review. Nothing they do now reaches the project.")
                   : tr("Their changes from before stay, with their name in Sync review.");
    ui::Tooltip(tip.c_str());
  }
  if (canAct) {
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - theme::kS1, p.y + (rowH - fh) * 0.5f));
    if (ui::TextButton(btn, false, bw, removed ? "A one-time invite that lets them back in (only an admin's invite does)" : "A new invite code for them")) {
      u.inviteId = hub->createInvite(u.inviteMinutes, removed ? m.site : 0);
      u.reinviteFor = name;
      u.scrollToInvite = true;
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
  ImGui::Dummy(ImVec2(w, 0));
  ImGui::PopID();
}

// One person: avatar, name with "you" / "admin" badges, a lamp with the link
// in words; on the right the actions that fit (offline: Reconnect, Invite
// again) and a menu with the rest.
void drawMember(App& a, Session& s, Hub* hub, const Member& m, float w) {
  Ui& u = U();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool me = m.site == s.site();
  const PeerView* pv = me ? nullptr : s.peer(m.site);
  Profile prof = me ? s.config().profile : (pv ? pv->profile : Profile());
  if (prof.name.empty()) prof.name = m.name;
  prof.avatar = pictureOf(a, s, m);  // J-111g
  const Hub::LinkView lv = hub && !me ? hub->link(m.site) : Hub::LinkView();
  const MemberStatus st = memberStatus(s, m, pv, lv);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float rowH = 44, fh = ImGui::GetFrameHeight(), avR = 15, textX = 2 * avR + theme::kS2 + theme::kS3;
  ImGui::PushID(static_cast<int>(m.site & 0x7fffffff));
  // actions: offline people get Reconnect and Invite again (when there is room), everyone the menu
  const bool offline = !me && !st.online;  // (also while a link keeps retrying)
  const bool canInvite = offline && lv.state != Hub::LinkView::State::Connecting && hub && s.ready();
  const float moreW = fh, rBtn = offline ? ui::TextButtonWidth("Reconnect...") : 0.0f, iBtn = canInvite ? ui::TextButtonWidth("Invite again") : 0.0f;
  float actionsW = moreW;
  if (offline) actionsW += rBtn + theme::kS1;
  bool inviteInRow = canInvite && w - (actionsW + iBtn + theme::kS1) - textX >= 150.0f;
  if (inviteInRow) actionsW += iBtn + theme::kS1;
  const float textW = std::max(40.0f, w - textX - actionsW - theme::kS3);
  // the row (hover, tooltip, right-click menu) under the actions
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##m", ImVec2(w, rowH));
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
  if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), theme::withAlpha(P.surface700, 0.6f), theme::kRadius);
  if (ImGui::IsItemHovered()) {
    std::string tip = std::string(tr("Fingerprint")) + ": " + m.fingerprint();
    if (pv && !pv->profile.email.empty()) tip += "\n" + pv->profile.email;
    tip += "\n" + st.text;
    if (!st.more.empty()) tip += "\n" + st.more;
    if (pv && pv->clockApprox) tip += std::string("\n") + tr("Their clock differs from this computer's: times are approximate.");
    ui::Tooltip(tip.c_str());
  }
  if (ImGui::BeginPopupContextItem("##mmenu")) {
    memberMenu(s, hub, m, me, canInvite);
    ImGui::EndPopup();
  }
  drawAvatar(a, dl, ImVec2(p.x + theme::kS1 + avR, p.y + rowH * 0.5f), avR, prof.name, prof.color ? prof.color : defaultColor(m.signPublic), prof.avatar, -1);
  // name, then the badges right after it
  const std::string name = prof.name.empty() ? std::string(tr("(no name)")) : prof.name;
  const char* youT = tr("you");
  const char* adminT = tr("admin");
  const bool isAdmin = m.site == s.admin();
  float badgesW = 0;
  if (me) badgesW += a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, youT).x + 10 + theme::kS1;
  if (isAdmin) badgesW += a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, adminT).x + 10 + theme::kS1;
  const float nameMax = std::max(30.0f, textW - badgesW);
  const float y1 = p.y + 5, x1 = p.x + textX;
  theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(x1, y1), nameMax, P.text, name.c_str());
  float bx = x1 + std::min(nameMax, a.fontBold->CalcTextSizeA(theme::kFontBody, 1e9f, 0, name.c_str()).x) + theme::kS2;
  const float by = y1 + 1;
  if (me) bx += badge(a, dl, ImVec2(bx, by), youT, P.textDim, P.surface700) + theme::kS1;
  if (isAdmin) badge(a, dl, ImVec2(bx, by), adminT, theme::lit(P.hot), theme::mix(P.surface800, P.hot, P.dark ? 0.22f : 0.16f));
  // the link: lamp and words
  const float y2 = p.y + 25, lh = theme::kFontCaption;
  ImU32 dot = st.dot;
  if (st.pulse) dot = theme::withAlpha(dot, 0.55f + 0.45f * static_cast<float>(0.5 + 0.5 * std::sin(ImGui::GetTime() * 4.0)));
  dl->AddCircleFilled(ImVec2(x1 + 3.5f, y2 + lh * 0.5f + 1), 3.5f, dot, 16);
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(x1 + 12, y2), textW - 12, st.failed ? theme::lit(P.danger) : P.textDim,
                      st.text.c_str());
  // actions, right-aligned and centred on the row
  float ax = p.x + w - actionsW - theme::kS1;
  const float ay = p.y + (rowH - fh) * 0.5f;
  if (offline) {
    ImGui::SetCursorScreenPos(ImVec2(ax, ay));
    if (ui::TextButton("Reconnect...", false, rBtn, "Exchange reconnect codes with them (chat, email)")) u.reconnectPeer = m.site;
    ax += rBtn + theme::kS1;
  }
  if (inviteInRow) {
    ImGui::SetCursorScreenPos(ImVec2(ax, ay));
    if (ui::TextButton("Invite again", false, iBtn, "A new invite code: they join again and stay the same member")) inviteAgain(*hub);
    ax += iBtn + theme::kS1;
  }
  ImGui::SetCursorScreenPos(ImVec2(ax, ay));
  if (ui::IconButton("##more", ui::Icon::More, ImGui::IsPopupOpen("##mmenu2"), fh, "More: fingerprint, admin, follow their playhead"))
    ImGui::OpenPopup("##mmenu2");
  if (ImGui::BeginPopup("##mmenu2")) {
    memberMenu(s, hub, m, me, canInvite && !inviteInRow);
    ImGui::EndPopup();
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
  ImGui::Dummy(ImVec2(w, 0));
  ImGui::PopID();
}

void drawInvite(App& a, Session& s, Hub& hub) {
  Ui& u = U();
  const auto& P = pal();
  ui::BeginSection(tr("INVITE SOMEONE"));
  if (u.scrollToInvite) {
    ImGui::SetScrollHereY(0.2f);
    u.scrollToInvite = false;
  }
  const float x = formX({"Valid for", "Reply code"});
  const Hub::InviteView* iv = u.inviteId.empty() ? nullptr : hub.invite(u.inviteId);
  if (!iv || iv->status == Hub::InviteStatus::Expired || iv->status == Hub::InviteStatus::Failed) {
    // no invite: choose how long the code works, then make it
    if (iv && iv->status == Hub::InviteStatus::Expired) statusLine(tr("The last invite expired. Make a new one."), P.textFaint, P.textDim);
    if (iv && iv->status == Hub::InviteStatus::Failed) statusLine(iv->error, P.danger, theme::lit(P.danger));
    static const int kMinutes[] = {15, 60, 24 * 60};
    static const char* kLabels[] = {i18n::N_("15 minutes"), i18n::N_("1 hour"), i18n::N_("24 hours")};
    int sel = 1;
    for (int i = 0; i < 3; ++i)
      if (u.inviteMinutes == kMinutes[i]) sel = i;
    label("Valid for", x);
    if (ui::BeginCombo("##invexp", tr(kLabels[sel]), 128)) {
      for (int i = 0; i < 3; ++i)
        if (ImGui::Selectable(tr(kLabels[i]), i == sel)) u.inviteMinutes = kMinutes[i];
      ImGui::EndCombo();
    }
    ImGui::SameLine(0, theme::kS2);
    alignRight(ui::TextButtonWidth("Create invite"));
    if (ui::TextButton("Create invite", true, 0, "Makes a one-time code for one person")) {
      u.inviteId = hub.createInvite(u.inviteMinutes);
      u.reinviteFor.clear();
      a.settings.set("collab.inviteMinutes", u.inviteMinutes);
    }
    note(tr("A code works once, for one person."));
  } else if (iv->status == Hub::InviteStatus::Preparing) {
    statusLine(std::string(tr("Preparing the invite")) + dots(), P.hot, P.text, true);
    note(tr("Finding this computer's network addresses."));
  } else if (iv->status == Hub::InviteStatus::Connected) {
    statusLine(fmt1(tr("%s joined"), iv->joinerName.empty() ? std::string(tr("Someone")) : iv->joinerName), P.led);
    if (!iv->safety.empty()) {
      ImGui::Dummy(ImVec2(0, theme::kS1));
      ui::Label("SAFETY CODE");
      safetyDigits(a, iv->safety);
      note(tr("Compare it with them by phone or chat. If it matches, nobody is in between."));
    }
    ImGui::Dummy(ImVec2(0, theme::kS1));
    if (ui::TextButton("Invite someone else")) {
      u.inviteId.clear();
      u.reinviteFor.clear();
    }
  } else {
    // a code is ready (or they are connecting with it)
    const bool connecting = iv->status == Hub::InviteStatus::Connecting;
    const int64_t left = iv->expires - static_cast<int64_t>(std::time(nullptr));
    const std::string exp = fmt1(tr("expires in %s"), countdown(left));
    ImGui::AlignTextToFramePadding();
    ui::StatusDot(connecting ? P.led : P.accent, true);
    const float ew = ImGui::CalcTextSize(exp.c_str()).x;
    ui::TextFit(connecting ? tr("Someone is connecting...") : tr("Waiting for them to join"), std::max(40.0f, ImGui::GetContentRegionAvail().x - ew - theme::kS3),
                P.text);
    ImGui::SameLine();
    alignRight(ew);
    ImGui::TextColored(theme::vec(left < 120 ? theme::lit(P.hot) : P.textFaint), "%s", exp.c_str());
    note(tr("Send this code to the person you are inviting. It works once."));
    if (!u.reinviteFor.empty()) note(fmt1(tr("For %s only: they come back as the same member."), u.reinviteFor), P.textDim);
    copyField(a, "##invcode", iv->code, 4);
    if (ui::TextButton("Copy", true, 0, "Copy the invite code")) ImGui::SetClipboardText(iv->code.c_str());
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Save as file...")) saveCodeFile(a, iv->code, "Gridline invite.txt");
    ImGui::SameLine(0, theme::kS2);
    alignRight(ui::TextButtonWidth("Cancel invite"));
    if (ui::TextButton("Cancel invite")) {
      hub.cancelInvite(u.inviteId);
      u.inviteId.clear();
      u.reinviteFor.clear();
    }
    hairline(theme::kS2, theme::kS2);
    label("Reply code", x);
    const float useW = ui::TextButtonWidth("Use reply");
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - useW - theme::kS2));
    ImGui::InputTextWithHint("##reply", "gridline-reply:...", u.replyBuf, sizeof u.replyBuf);
    ui::FocusRing();
    ImGui::SameLine(0, theme::kS2);
    ImGui::BeginDisabled(u.replyBuf[0] == 0);
    if (ui::TextButton("Use reply", true)) {
      std::string err;
      if (hub.acceptReply(u.replyBuf, &err)) u.replyBuf[0] = 0;
      else a.notify(err, true);
    }
    ImGui::EndDisabled();
    label("", x);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Only if they can't connect directly: they then get a reply code to send you."));
    ImGui::PopTextWrapPos();
  }
  (void)s;
  ui::EndSection();
}

void drawReconnect(App& a, Session& s, Hub& hub) {
  Ui& u = U();
  const auto& P = pal();
  if (!u.reconnectPeer) return;
  const Member* m = s.member(u.reconnectPeer);
  if (!m) {
    u.reconnectPeer = 0;
    return;
  }
  ui::BeginSection(tr("RECONNECT"));
  const float x = formX({"Your code", "Their code"});
  note(fmt1(tr("Send %s your code and paste theirs. Codes only carry network addresses; your keys check the connection."), m->name));
  const std::string code = hub.reconnectCode(u.reconnectPeer);
  label("Your code", x);
  if (code.empty()) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textFaint), "%s", (std::string(tr("Preparing")) + dots()).c_str());
  } else {
    ImGui::BeginGroup();
    copyField(a, "##rccode", code, 3);
    if (ui::TextButton("Copy##rc", true)) ImGui::SetClipboardText(code.c_str());
    ImGui::EndGroup();
  }
  label("Their code", x);
  const float cw = ui::TextButtonWidth("Connect");
  ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - cw - theme::kS2));
  ImGui::InputTextWithHint("##rcin", "gridline-reconnect:...", u.reconnectBuf, sizeof u.reconnectBuf);
  ui::FocusRing();
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(u.reconnectBuf[0] == 0);
  if (ui::TextButton("Connect", true)) {
    std::string err;
    if (hub.acceptReconnectCode(u.reconnectBuf, &err)) u.reconnectBuf[0] = 0;
    else a.notify(err, true);
  }
  ImGui::EndDisabled();
  alignRight(ui::TextButtonWidth("Close##rc"));
  if (ui::TextButton("Close##rc")) u.reconnectPeer = 0;
  ui::EndSection();
}

void startNetTest(App& a) {
  Ui& u = U();
  if (u.netTest) return;
  auto job = std::make_shared<NetTestJob>();
  u.netTest = job;
  NetSettings ns = netSettingsFrom(a.settings);
  std::thread([job, ns] {
    NetReport r = testConnection(ns);
    std::lock_guard<std::mutex> lk(job->mu);
    job->report = r;
    job->done = true;
  }).detach();
}

// "Test my connection" and, once it ran, its findings as form rows (labels end at `x`).
void netTestBlock(App& a, float x) {
  Ui& u = U();
  const auto& P = pal();
  if (u.netTest) {
    std::lock_guard<std::mutex> lk(u.netTest->mu);
    if (u.netTest->done) {
      u.lastReport = u.netTest->report;
      u.haveReport = true;
      u.netTest.reset();
    }
  }
  if (u.netTest) {
    ImGui::BeginDisabled();
    ui::TextButton("Testing...");
    ImGui::EndDisabled();
  } else if (ui::TextButton("Test my connection", false, 0,
                            "Asks two STUN servers for your public address, tries the router's automatic port opening and looks for IPv6")) {
    startNetTest(a);
  }
  if (!u.haveReport) return;
  const NetReport& r = u.lastReport;
  static const char* nat[] = {i18n::N_("unknown"), i18n::N_("no NAT"), i18n::N_("cone NAT (hole punching works)"), i18n::N_("symmetric NAT"),
                              i18n::N_("UDP blocked")};
  auto row = [&](const char* l, const std::string& v, bool mono) {
    label(l, x);
    if (mono) ImGui::PushFont(a.fontMono, theme::kFontNumber);
    ui::TextFit(v.c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x), P.text, true);
    if (mono) ImGui::PopFont();
  };
  row("Public address", r.publicAddress.empty() ? std::string(tr("unknown")) : r.publicAddress, !r.publicAddress.empty());
  row("Router", tr(nat[static_cast<int>(r.nat)]), false);
  row("Port opening", r.portMapping ? r.portMappingProtocol + " (" + r.mappedAddress + ")" : std::string(tr("no")), false);
  row("IPv6", r.ipv6 ? r.ipv6Address : std::string(tr("no")), r.ipv6);
  if (!r.summary.empty()) {
    label("", x);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textDim), "%s", r.summary.c_str());
    ImGui::PopTextWrapPos();
  }
}

// File > Open collaborative project...: choose the project's folder.
void openCollabFolder(App& a) {
  const auto known = ctlOf(a).knownProjects();
  const std::string start = known.empty() ? std::string() : fs::path(known.front().folder).parent_path().string();
  chooseFolder(a, "Open a collaborative project folder", start, [&a](const std::string& dir) {
    if (projectFileIn(dir).empty()) {
      a.notify(tr("That folder is not a collaborative project (it has no .gridline-collab folder)."), true);
      return;
    }
    a.guardUnsaved([&a, dir] { a.openProject(dir); });
  });
}

// ---- J-111e: the files at the end of collaboration (docs: "What happens to the files")

std::function<bool(const std::string&)>& openUrlFn() {
  static std::function<bool(const std::string&)> f;  // SDL_OpenURL (main.cpp; plugin builds have none)
  return f;
}

// "Show in Finder" / "Show in Files": the system file manager at `folder`.
void revealFolder(const std::string& folder) {
  std::string url = "file://";
  for (unsigned char ch : fs::absolute(fs::path(folder)).generic_string()) {
    if (std::isalnum(ch) || ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      url += static_cast<char>(ch);
    } else {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", ch);
      url += b;
    }
  }
  if (openUrlFn()) openUrlFn()(url);
}

const char* revealLabel() {
#if defined(__APPLE__)
  return "Show in Finder";
#elif defined(_WIN32)
  return "Show in Explorer";
#else
  return "Show in Files";
#endif
}

// A folder named `name` in `dir` that does not exist yet ("Song", "Song 2", ...).
std::string freshFolder(const std::string& dir, const std::string& name) {
  std::error_code ec;
  fs::path p = fs::path(dir) / name;
  for (int i = 2; fs::exists(p, ec) && i < 1000; ++i) p = fs::path(dir) / (name + " " + std::to_string(i));
  return p.string();
}

// Sizes of the open project's folder (refreshed every few seconds: walking the folder is not free).
const Ui::Sizes& folderSizes(const std::string& folder, double now) {
  Ui& u = U();
  if (u.sizes.folder != folder || now - u.sizes.at > 5.0) {
    u.sizes.folder = folder;
    u.sizes.at = now;
    u.sizes.total = folderSize(folder);
    u.sizes.audio = folderSize((fs::path(folder) / "Audio").string());
    u.sizes.history = folderSize((fs::path(folder) / ".gridline-collab").string());
  }
  return u.sizes;
}

// A normal project from a collaborative folder this computer no longer syncs (removed,
// stopped): the music and its audio in a new folder; then it opens.
void keepCopyOfFolder(App& a, const std::string& folder, const std::string& name) {
  chooseFolder(a, "Choose where the private copy goes", fs::path(folder).parent_path().string(), [&a, folder, name](const std::string& dir) {
    const std::string dest = freshFolder(dir, safeFolderName(name));
    Song song;
    std::string err, file;
    if (!loadProject(Session::projectFileOf(folder), song, &err) || !exportPlainProject(song, folder, dest, safeFolderName(name), false, &file, &err)) {
      a.notify(std::string(tr("Could not make the private copy")) + ": " + err, true);
      return;
    }
    a.guardUnsaved([&a, file] { a.openProject(file); });
    a.notify(fmt1(tr("Private copy saved in %s."), dest));
  });
}

void forgetPrompt(App& a, const Controller::KnownProject& k) {
  using St = Controller::KnownProject::State;
  a.confirm = {};
  a.confirm.open = true;
  a.confirm.title = fmt1(tr("Forget %s?"), k.name);
  a.confirm.subtitle = k.state == St::Missing ? std::string(tr("It leaves this list. Nothing on disk changes."))
                                              : fmt1(tr("It leaves this list. The folder %s stays as a normal project; its shared history and the other members' names are deleted."),
                                                     fs::path(k.folder).filename().string());
  a.confirm.okLabel = "Forget";
  const std::string f = k.folder;
  a.confirm.action = [&a, f] { ctlOf(a).forgetProject(f); };
}

// J-111f: a known project's name, with its folder when another one has the same name.
std::string knownLabel(const std::vector<Controller::KnownProject>& all, const Controller::KnownProject& k) {
  auto lower = [](std::string t) {
    for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return t;
  };
  for (const auto& o : all)
    if (o.folder != k.folder && lower(o.name) == lower(k.name)) {
      const fs::path f(k.folder);
      return k.name + "  (" + (f.parent_path().filename() / f.filename()).string() + ")";
    }
  return k.name;
}

// The collaborative projects on this computer, to reopen with one click; the ones
// that ended for this computer (removed, stopped) and the ones whose folder moved.
void recentProjects(App& a) {
  using St = Controller::KnownProject::State;
  const auto& P = pal();
  const auto known = ctlOf(a).knownProjects();
  ui::BeginSection(tr("YOUR COLLABORATIVE PROJECTS"));
  if (known.empty()) {
    note(tr("None on this computer yet. Projects you share or join appear here."));
  } else {
    const float w = ImGui::GetContentRegionAvail().x;
    int shown = 0;
    for (const auto& k : known) {
      if (++shown > 8) break;
      if (shown > 1) hairline(theme::kS1, theme::kS1);
      ImGui::PushID(k.folder.c_str());
      const char* b1 = k.state == St::Active ? "Open" : k.state == St::Missing ? "Locate..." : "Keep a private copy...";
      const char* b2 = k.state == St::Active ? nullptr : "Forget";
      const float bw = b2 ? buttonsWidth({b1, b2}) : ui::TextButtonWidth(b1);
      const float y0 = ImGui::GetCursorPosY();
      ImGui::BeginGroup();
      // the name, then the state as a pill
      const char* stateT = k.state == St::Removed ? tr("Removed") : k.state == St::Ended ? tr("Not shared any more") : k.state == St::Missing ? tr("Missing") : nullptr;
      const float pillW = stateT ? a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, stateT).x + 10 + theme::kS2 : 0.0f;
      ImGui::PushFont(a.fontBold, theme::kFontBody);
      ui::TextFit(k.name.c_str(), std::max(40.0f, w - bw - theme::kS3 - pillW), k.state == St::Active ? P.text : P.textDim);
      ImGui::PopFont();
      if (stateT) {
        const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
        const ImU32 tone = k.state == St::Missing ? P.hot : P.textDim;
        badge(a, ImGui::GetWindowDrawList(), ImVec2(r1.x + theme::kS2, r0.y + (r1.y - r0.y - 16) * 0.5f), stateT, k.state == St::Missing ? theme::lit(P.hot) : tone,
              k.state == St::Missing ? theme::mix(P.surface800, P.hot, P.dark ? 0.22f : 0.16f) : P.surface700);
      }
      ImGui::PushFont(a.fontRegular, theme::kFontCaption);
      std::string sub = k.folder;
      if (k.state == St::Removed) sub = fmt2(tr("%s removed you, %s"), k.byName.empty() ? std::string(tr("The admin")) : k.byName, clockTime(k.at, false));
      else if (k.state == St::Ended) sub = fmt2(tr("%s stopped sharing it, %s"), k.byName.empty() ? std::string(tr("The admin")) : k.byName, clockTime(k.at, false));
      else if (k.state == St::Missing) sub = fmt1(tr("Not found at %s"), k.folder);
      if (k.state == St::Active) ui::PathFit(sub.c_str(), w - bw - theme::kS3, P.textFaint);
      else ui::TextFit(sub.c_str(), w - bw - theme::kS3, P.textFaint);
      ImGui::PopFont();
      ImGui::EndGroup();
      if (k.state == St::Removed && !k.reason.empty() && ImGui::IsItemHovered()) ui::Tooltip((std::string("\xE2\x80\x9C") + k.reason + "\xE2\x80\x9D").c_str());
      const float h = ImGui::GetItemRectSize().y;
      ImGui::SameLine();
      ImGui::SetCursorPosY(y0 + std::max(0.0f, (h - ImGui::GetFrameHeight()) * 0.5f));
      alignRight(bw);
      const float by = ImGui::GetCursorPosY();  // both buttons on this row's line
      if (k.state == St::Active) {
        if (ui::TextButton(b1, false, 0, "Opens it and reconnects to the others (they need it open too)")) {
          const std::string f = k.folder;
          a.guardUnsaved([&a, f] { a.openProject(f); });
        }
      } else if (k.state == St::Missing) {
        if (ui::TextButton(b1, false, 0, "Moved or renamed it? Choose the folder where it is now")) {
          const std::string id = k.id;
          chooseFolder(a, "Where is the project folder now?", fs::path(k.folder).parent_path().string(), [&a, id](const std::string& dir) {
            std::string err;
            if (!ctlOf(a).locateProject(id, dir, &err)) a.notify(err, true);
          });
        }
      } else if (ui::TextButton(b1, true, 0, "A normal project in a new folder: the music and its audio, nothing shared")) {
        keepCopyOfFolder(a, k.folder, k.name);
      }
      if (b2) {
        ImGui::SameLine(0, theme::kS2);
        ImGui::SetCursorPosY(by);
        if (ui::TextButton(b2, false, 0, k.state == St::Missing ? "Take it off this list" : "Take it off this list; the folder stays as a normal project")) forgetPrompt(a, k);
      }
      ImGui::PopID();
    }
  }
  hairline(theme::kS2, theme::kS2);
  if (ui::TextButton("Open collaborative project...", false, 0, "Choose the project's folder (the one with the .gridline-collab folder)")) openCollabFolder(a);
  ui::EndSection();
}

// J-111h: "Share this project" (no collaborative project open): the name everyone sees, the folder
// it is copied into, then Make collaborative. Laid out like the rest of the panel.
void shareSection(App& a, bool gateOk) {
  Ui& u = U();
  const auto& P = pal();
  Controller& c = ctlOf(a);
  shareDefaults(a, false);
  ui::BeginSection(tr("SHARE THIS PROJECT"));
  const bool can = gateOk && networkBuiltIn();
  ImGui::BeginDisabled(!can);
  const float x = formX({"Name", "Folder"});
  label("Name", x);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  if (u.shareFocus && can) {
    ImGui::SetKeyboardFocusHere();
    u.shareFocus = false;
  }
  const bool enter = ImGui::InputTextWithHint("##sharename", tr("Project name"), u.shareName, sizeof u.shareName, ImGuiInputTextFlags_EnterReturnsTrue);
  ui::FocusRing();
  const std::string name = cleanProjectName(u.shareName);
  label("", x);
  nameCaption(u.shareName, name, tr("Everyone you invite sees this name. The folder is named after it."));
  // the folder: where it goes (Choose...), named after the project
  const std::string target = shareTarget(name);
  label("Folder", x);
  const float cw = ui::TextButtonWidth("Choose...##share");
  ui::PathFit(target.c_str(), std::max(60.0f, ImGui::GetContentRegionAvail().x - cw - theme::kS3), P.text, true);
  ImGui::SameLine(0, theme::kS3);
  alignRight(cw);
  if (ui::TextButton("Choose...##share", false, 0, "Where the project's folder is made"))
    chooseFolder(a, "Choose where the project folder goes", u.shareParent, [](const std::string& dir) { U().shareParent = dir; });
  if (u.shareChecked != target || u.now - u.shareCheckedAt > 1.0) {  // (looked at again every second)
    std::error_code ec;
    u.shareChecked = target;
    u.shareCheckedAt = u.now;
    u.shareTaken = fs::exists(fs::path(target) / ".gridline-collab", ec);
    u.shareExists = !u.shareTaken && fs::is_directory(target, ec) && !fs::is_empty(target, ec);
  }
  label("", x);
  ImGui::AlignTextToFramePadding();
  if (u.shareTaken) dim(tr("That folder already holds a collaborative project: choose another place or name."), theme::lit(P.danger));
  else if (u.shareExists) dim(tr("That folder already exists: the project is added to what is in it."), P.hot);
  else dim(tr("A new folder: the project and every audio file it uses are copied into it. Your project file stays as it is."), P.textFaint);
  ImGui::EndDisabled();
  if (!networkBuiltIn()) note(tr("This build of Gridline has no collaboration networking."));
  if (can && !c.profileReady() && noticeBar("##needname", tr("Choose a display name first: collaborators see it."), P.hot, "Settings...", false)) {
    a.settingsTab = 11;
    a.showSettings = true;
  }
  if (!u.makeErr.empty()) statusLine(u.makeErr, P.danger, theme::lit(P.danger));
  hairline(theme::kS2, theme::kS2);
  const float bw = std::max(120.0f, ui::TextButtonWidth("Make collaborative"));
  alignRight(bw);
  ImGui::BeginDisabled(!can || name.empty() || u.shareTaken);
  if (ui::TextButton("Make collaborative", true, bw, "Copies the project into the folder and shares it from there") || (enter && can && !name.empty() && !u.shareTaken))
    shareNow(a);
  ImGui::EndDisabled();
  ui::EndSection();
}

// No collaborative project open: what collaboration is, share this project or join one, reopen one.
void panelStart(App& a) {
  Ui& u = U();
  const auto& P = pal();
  header(a, tr("Work on this project together"), tr("Each person keeps their own windows and transport; only the project is shared."));
  beginBody("##collabbody");
  const bool gateOk = gateLine(a);
  if (!u.foreign.empty()) {
    ui::BeginSection(tr("PLUGINS THAT CAN'T BE SHARED"));
    statusLine(tr("This project uses plugins that are not part of Gridline."), P.hot);
    const float w = ImGui::GetContentRegionAvail().x;
    for (const auto& [plug, where] : u.foreign) {
      ImGui::TextUnformatted(plug.c_str());
      ImGui::SameLine(0, theme::kS2);
      ui::TextFit(where.c_str(), std::max(40.0f, w - ImGui::GetItemRectSize().x - theme::kS2), P.textFaint);
    }
    note(tr("Another computer might not have them, and their settings can't be merged. Replace or remove them, then try again."));
    ui::EndSection();
  }
  shareSection(a, gateOk);
  ui::BeginSection(tr("JOIN"));
  {
    ImGui::BeginDisabled(!gateOk || !networkBuiltIn());
    const float bw = ui::TextButtonWidth("Join collaborative project...");
    if (ui::TextButton("Join collaborative project...", false, bw)) openJoin(a);
    ImGui::SameLine(0, theme::kS3);
    ui::TextFit(tr("Use an invite code you were sent"), std::max(40.0f, ImGui::GetContentRegionAvail().x), P.textFaint, true);
    ImGui::EndDisabled();
  }
  ui::EndSection();
  recentProjects(a);
  ImGui::EndChild();
}

void panelWindow(App& a) {
  if (!a.showCollab) return;
  Ui& u = U();
  const auto& P = pal();
  ui::PlaceNextDialog(ImVec2(kPanelW, 640), ImGuiCond_Appearing, ImVec2(kPanelMinW, 320));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const bool vis = ImGui::Begin(ui::windowTitle("Collaboration").c_str(), &a.showCollab, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  Controller& c = ctlOf(a);
  Session* s = c.session();
  if (!s) {
    panelStart(a);
    ImGui::End();
    return;
  }
  Hub* hub = c.hub();
  {
    const Member* adm = s->member(s->admin());
    int online = 1;
    for (const auto& [site, pv] : s->peers()) online += pv.connected && s->isActive(site) ? 1 : 0;
    char right[64];
    std::snprintf(right, sizeof right, tr("%d of %d online"), online, static_cast<int>(s->activeMembers().size()));
    header(a, s->projectName(),  // J-111i: the name is renamed in place
           s->isAdmin() ? std::string(tr("You are the admin: you resolve conflicting changes.")) : fmt1(tr("Admin: %s"), adm ? adm->name : std::string()),
           right, online > 1 ? P.textDim : P.textFaint, true);
  }
  if (!s->ready()) {
    const Hub::JoinView& jv = hub->joinView();
    noticeBar("##notready", jv.status == Hub::JoinStatus::NeedReply ? std::string(tr("Waiting for the inviter: send them the reply code (Join window).")) : std::string(tr("Connecting to the inviter...")),
              P.hot);
    ImGui::Dummy(ImVec2(0, theme::kS1));
  }
  // conflicts
  if (const size_t open = s->conflicts().openCount(); open > 0) {
    char b[128];
    if (open == 1) std::snprintf(b, sizeof b, "%s", s->isAdmin() ? tr("1 change needs your review") : tr("1 change awaiting the admin's review"));
    else std::snprintf(b, sizeof b, s->isAdmin() ? tr("%d changes need your review") : tr("%d changes awaiting the admin's review"), static_cast<int>(open));
    if (noticeBar("##review", b, P.hot, s->isAdmin() ? "Review..." : nullptr)) a.showSyncReview = true;
    ImGui::Dummy(ImVec2(0, theme::kS1));
  }
  beginBody("##collabbody");
  ui::BeginSection(tr("PEOPLE"));
  {
    const float w = ImGui::GetContentRegionAvail().x;
    bool first = true;
    std::vector<Member> gone;
    int removedN = 0;
    for (const Member& m : s->members()) {
      if (!s->isActive(m.site)) {  // J-111e: removed, or left
        gone.push_back(m);
        removedN += s->memberState(m.site) == MemberState::Removed ? 1 : 0;
        continue;
      }
      if (!first) hairline(theme::kS1 - 2, theme::kS1 - 2);
      first = false;
      drawMember(a, *s, hub, m, w);
    }
    if (!gone.empty()) {
      hairline(theme::kS1, theme::kS1);
      char b[96];
      if (removedN == static_cast<int>(gone.size())) std::snprintf(b, sizeof b, tr("Removed (%d)"), removedN);
      else if (removedN == 0) std::snprintf(b, sizeof b, tr("Left (%d)"), static_cast<int>(gone.size()));
      else std::snprintf(b, sizeof b, tr("Removed or left (%d)"), static_cast<int>(gone.size()));
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textDim));
      ImGui::SetNextItemOpen(u.showGone, ImGuiCond_Always);
      if (ImGui::TreeNodeEx((std::string(b) + "##gone").c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding) !=
          u.showGone)
        u.showGone = !u.showGone;
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) ui::Tooltip(tr("People who are no longer in the project. Their earlier changes stay."));
      if (u.showGone)
        for (const Member& m : gone) drawGoneMember(a, *s, hub, m, w);
    }
  }
  ui::EndSection();
  // a peer that could not be reached directly: what still works
  for (const Member& m : s->members()) {
    if (m.site == s->site() || !hub || !s->isActive(m.site)) continue;
    if (hub->link(m.site).state != Hub::LinkView::State::Failed) continue;
    ui::BeginSection(tr("CONNECTION"));
    statusLine(fmt1(tr("Couldn't connect directly to %s"), m.name), P.danger);
    note(tr("A collaborator online with you both relays changes. A TURN server (Settings > Collaboration) or a reconnect code can also help."));
    if (ui::TextButton("Reconnect with a code...")) u.reconnectPeer = m.site;
    ImGui::SameLine(0, theme::kS2);
    netTestBlock(a, formX({"Public address", "Router", "Port opening", "IPv6"}));
    ui::EndSection();
    break;
  }
  if (hub && s->ready()) {
    drawReconnect(a, *s, *hub);
    drawInvite(a, *s, *hub);
  }
  ui::BeginSection(tr("THIS PROJECT"));
  {
    const std::string& folder = s->config().folder;
    const float x = formX({"Folder", "Files", "Cursors", "Sync", "Last synced"});
    label("Folder", x);
    {
      const float rw = ui::TextButtonWidth(revealLabel());
      ui::PathFit(folder.c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x - rw - theme::kS2), P.text, true);  // the whole path on hover
      ImGui::SameLine(0, theme::kS2);
      alignRight(rw);
      if (ui::TextButton(revealLabel())) revealFolder(folder);
    }
    // J-111e: what the folder holds, and a safe clean-up of audio nothing uses any more
    label("Files", x);
    {
      const Ui::Sizes& z = folderSizes(folder, u.now);
      ImGui::BeginGroup();
      ImGui::AlignTextToFramePadding();
      const std::string t = fmt1(tr("%s in all"), formatBytes(z.total)) + "  \xC2\xB7  " + fmt1(tr("audio %s"), formatBytes(z.audio)) + "  \xC2\xB7  " +
                            fmt1(tr("shared history %s"), formatBytes(z.history));
      ui::TextFit(t.c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x), P.textDim);
      if (ui::TextButton("Clean up unused audio...", false, 0, "Audio files nothing in the project, its history, undo or Sync review uses")) {
        u.unused = s->unusedAudio();
        u.unusedBytes = 0;
        for (const auto& f : u.unused) u.unusedBytes += f.second;
        u.cleanupOpen = true;
      }
      ImGui::EndGroup();
    }
    label("Cursors", x);
    bool cursors = a.settings.getBool("collab.showCursors");
    if (ImGui::Checkbox(tr("Show collaborators' cursors"), &cursors)) a.settings.set("collab.showCursors", cursors);
    label("Sync", x);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Peer to peer, no server: changes travel while two computers are online together."));
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ui::Hint("Every computer keeps the whole project, so you can always keep working");
    // J-111e: an abandoned project (nobody came back for a month): nothing is deleted by itself
    const uint64_t last = s->lastContactMs(), nowMs = wallMs();
    if (s->activeMembers().size() > 1) {
      label("Last synced", x);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textDim), "%s", last ? fmt1(tr("with a collaborator %s"), ago(last, nowMs)).c_str() : tr("not with anyone yet"));
    }
    const int idleDays = last && nowMs > last ? static_cast<int>((nowMs - last) / 86400000ull) : 0;
    if (idleDays >= 30 || std::getenv("GRIDLINE_DEV_ABANDONED")) {
      ImGui::Dummy(ImVec2(0, theme::kS1));
      char b[160];
      std::snprintf(b, sizeof b, tr("Nobody has synced with you for %d days."), std::max(idleDays, 30));
      if (noticeBar("##abandoned", b, P.textDim, "Make it a normal project...", false)) {
        u.leaveDialog = 3;
        u.leaveErr.clear();
      }
      note(tr("Nothing is deleted by itself. Keep it shared, or make it a normal project on this computer."));
    }
    hairline(theme::kS2, theme::kS2);
    const bool admin = s->isAdmin(), others = s->activeMembers().size() > 1;
    if (c.leaving()) {
      statusLine(std::string(tr("Telling your collaborators")) + dots(), P.hot, P.text, true);
    } else if (admin) {
      // the admin of others: stop sharing for everyone, or hand admin over and leave
      const float bw = others ? buttonsWidth({"Leave project...", "Stop sharing..."}) : ui::TextButtonWidth("Stop sharing...");
      alignRight(bw);
      if (others) {
        ImGui::BeginDisabled();
        ui::DangerButton("Leave project...");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ui::Tooltip(tr("You are the admin. Make someone else admin first (the ... menu next to them), then leave."));
        ImGui::SameLine(0, theme::kS2);
      }
      if (ui::DangerButton("Stop sharing...", false, 0, "For everyone: nothing syncs any more; each person keeps their copy")) {
        u.leaveDialog = 2;
        u.leaveErr.clear();
      }
      if (others) note(tr("To leave, make someone else admin first (the ... menu next to them)."));
    } else {
      alignRight(ui::TextButtonWidth("Leave project..."));
      if (ui::DangerButton("Leave project...", false, 0, "You stop syncing; keep your copy as a normal project or delete it")) {
        u.leaveDialog = 1;
        u.leaveMode = 0;
        u.leaveWait = false;
        u.leaveErr.clear();
      }
    }
  }
  ui::EndSection();
  ImGui::EndChild();
  ImGui::End();
}

// Joining with an invite: the session starts and connects to the inviter.
bool startJoin(App& a, const InviteInfo& info, const std::string& folder, std::string* err) {
  Ui& u = U();
  Controller& c = ctlOf(a);
  a.engine.stopToStart();
  a.engine.panic(false);
  c.net = netSettingsFrom(a.settings);
  if (!c.join(a.doc, info, folder, err)) return false;
  u.parsed = info;
  u.joinFolder = folder;
  u.joining = true;
  u.joinErr.clear();
  a.projectPath.clear();
  return true;
}

// Per frame: the joined project opens as soon as its first sync is in, whether
// or not the Join window is visible.
void finishJoin(App& a) {
  Ui& u = U();
  Session* s = a.collab ? a.collab->session() : nullptr;
  if (!u.joining || !s || !a.collab->hub() || !s->ready()) return;
  if (a.collab->hub()->joinView().status != Hub::JoinStatus::Connected) return;
  u.joining = false;
  a.projectPath = s->projectFile();
  a.projectFormat = SaveFormat::Gridline;
  a.openedFrom.clear();
  a.addRecent(a.projectPath);
  afterProjectReplaced(a);
  a.notify(fmt1(tr("Joined %s. Changes now sync both ways."), s->projectName()));
  devsig::reach("collab-joined");
  a.showJoin = false;
  a.showCollab = true;
}

// Join window steps: 1 the code, 2 where it goes, 3 connecting. Done steps
// show a check on the LED colour, the current one is lit.
void joinSteps(App& a, int current) {
  const auto& P = pal();
  static const char* kSteps[] = {i18n::N_("Invite code"), i18n::N_("Where to store it"), i18n::N_("Connect")};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 22, r = 9, w = ImGui::GetContentRegionAvail().x;
  float x = p.x;
  for (int i = 0; i < 3; ++i) {
    const bool done = i < current, cur = i == current;
    const ImU32 col = done ? theme::lit(P.led) : cur ? theme::lit(P.hot) : P.textFaint;
    const ImVec2 c(x + r, p.y + h * 0.5f);
    if (cur) dl->AddCircleFilled(c, r, theme::mix(P.surface800, P.hot, P.dark ? 0.22f : 0.16f), 20);
    dl->AddCircle(c, r, col, 20, 1.5f);
    if (done) {
      dl->AddLine(ImVec2(c.x - 4, c.y), ImVec2(c.x - 1, c.y + 3), col, 1.6f);
      dl->AddLine(ImVec2(c.x - 1, c.y + 3), ImVec2(c.x + 4, c.y - 3), col, 1.6f);
    } else {
      char n[4];
      std::snprintf(n, sizeof n, "%d", i + 1);
      const ImVec2 ts = a.fontBold->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, n);
      dl->AddText(a.fontBold, theme::kFontCaption, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, n);
    }
    const char* t = tr(kSteps[i]);
    const ImVec2 ts = (cur ? a.fontBold : a.fontRegular)->CalcTextSizeA(theme::kFontCaption + 1, 1e9f, 0, t);
    dl->AddText(cur ? a.fontBold : a.fontRegular, theme::kFontCaption + 1, ImVec2(x + 2 * r + theme::kS2, p.y + (h - ts.y) * 0.5f),
                cur ? P.text : done ? P.textDim : P.textFaint, t);
    x += 2 * r + theme::kS2 + ts.x;
    if (i < 2) {
      const float lx0 = x + theme::kS2, lx1 = std::min(p.x + w, x + theme::kS2 + 24);
      dl->AddLine(ImVec2(lx0, p.y + h * 0.5f), ImVec2(lx1, p.y + h * 0.5f), done ? theme::withAlpha(col, 0.6f) : P.line, 1.0f);
      x = lx1 + theme::kS2;
    }
  }
  ImGui::Dummy(ImVec2(std::min(w, x - p.x), h));
  ImGui::Dummy(ImVec2(0, theme::kS1));
}

// One step of the connection (lamp, what happens, how far it got).
void stepRow(const std::string& text, ImU32 dot, bool pulse, const std::string& detail = std::string()) {
  statusLine(text, dot, pulse || dot == pal().textFaint ? pal().textDim : pal().text, pulse);
  if (!detail.empty()) {
    ImGui::Indent(14);
    note(detail);
    ImGui::Unindent(14);
  }
}

void joinWindow(App& a) {
  if (!a.showJoin) return;
  Ui& u = U();
  const auto& P = pal();
  ui::PlaceNextDialog(ImVec2(kPanelW, 600), ImGuiCond_Appearing, ImVec2(kPanelMinW, 360));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const bool vis = ImGui::Begin(ui::windowTitle("Join collaborative project").c_str(), &a.showJoin, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  Controller& c = ctlOf(a);
  Session* s = c.session();
  if (u.joining && s && c.hub()) {
    const Hub::JoinView& jv = c.hub()->joinView();
    header(a, fmt1(tr("Joining %s"), u.parsed.projectName), fmt1(tr("Invited by %s"), u.parsed.inviterName));
    joinSteps(a, 2);
    beginBody("##joinbody", ui::DialogFooterHeight(1));
    ui::BeginSection(tr("CONNECTING"));
    const bool failed = jv.status == Hub::JoinStatus::Failed || jv.status == Hub::JoinStatus::Refused;
    const bool connected = jv.status == Hub::JoinStatus::Connected;
    // 1. reach the inviter
    switch (jv.status) {
      case Hub::JoinStatus::Connecting: stepRow(fmt1(tr("Reaching %s"), u.parsed.inviterName) + dots(), P.hot, true); break;
      case Hub::JoinStatus::NeedReply:
        stepRow(fmt1(tr("%s's computer can't be reached directly yet"), u.parsed.inviterName), P.hot, true);
        ImGui::Indent(14);
        note(tr("Send them this reply code; they paste it into their invite."));
        copyField(a, "##replycode", jv.reply, 3);
        if (ui::TextButton("Copy", true)) ImGui::SetClipboardText(jv.reply.c_str());
        ImGui::SameLine(0, theme::kS2);
        if (ui::TextButton("Save as file...")) saveCodeFile(a, jv.reply, "Gridline reply.txt");
        ImGui::Unindent(14);
        break;
      case Hub::JoinStatus::Connected: stepRow(fmt1(tr("Connected to %s"), u.parsed.inviterName), P.led, false); break;
      case Hub::JoinStatus::Refused:
      case Hub::JoinStatus::Failed: stepRow(jv.message, P.danger, false); break;
      case Hub::JoinStatus::None: break;
    }
    // 2. the safety code, 3. the project arriving
    if (connected) {
      size_t total = 0, done = 0;
      for (const auto& [site, pv] : s->peers()) total += pv.syncTotal, done += pv.syncDone;
      char b[128];
      std::snprintf(b, sizeof b, tr("Receiving the project: %d of %d changes"), static_cast<int>(done), static_cast<int>(total));
      stepRow(b, P.accent, true);  // (finishJoin, every frame, opens the project once it is in)
      if (total > 0) {
        ImGui::Indent(14);
        ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(-1, 4), "");
        ImGui::Unindent(14);
      }
    } else if (!failed) {
      stepRow(tr("Receive the project"), P.textFaint, false);
    }
    ui::EndSection();
    if (connected && !jv.safety.empty()) {
      ui::BeginSection(tr("SAFETY CODE"));
      safetyDigits(a, jv.safety);
      note(fmt1(tr("Compare it with %s by phone or chat. If it matches, nobody is in between."), u.parsed.inviterName));
      ui::EndSection();
    }
    if (jv.status == Hub::JoinStatus::Failed) {
      ui::BeginSection(tr("WHAT YOU CAN DO"));
      note(tr("Try again from another network, ask a collaborator who is online with the inviter to invite you, or set a TURN server in Settings > Collaboration."));
      ui::EndSection();
    }
    ImGui::EndChild();
    if (failed) {
      ui::DialogFooter(ui::TextButtonWidth("Close"));
      if (ui::TextButton("Close", true)) {
        u.joining = false;
        c.close();
        a.showJoin = false;
      }
    } else {
      ui::DialogFooter(ui::TextButtonWidth("Cancel##join"));
      if (ui::TextButton("Cancel##join", false, 0, "Stops joining; nothing is kept")) {
        u.joining = false;
        c.close();
        a.newProject();
      }
    }
    ImGui::End();
    return;
  }
  if (u.lastParsed != u.codeBuf) {
    u.lastParsed = u.codeBuf;
    u.parseErr.clear();
    u.parsedOk = u.lastParsed.find_first_not_of(" \n\r\t") != std::string::npos && parseInviteCode(u.lastParsed, u.parsed, &u.parseErr);
  }
  const bool expired = u.parsedOk && u.parsed.expired(static_cast<int64_t>(std::time(nullptr)));
  header(a, tr("Join a collaborative project"), tr("Someone shared a project with you: paste their invite code."));
  joinSteps(a, u.parsedOk && !expired ? 1 : 0);
  beginBody("##joinbody", ui::DialogFooterHeight(1));
  const bool gateOk = gateLine(a);
  ui::BeginSection(tr("INVITE CODE"));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(P.field));
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  ImGui::InputTextMultiline("##code", u.codeBuf, sizeof u.codeBuf, ImVec2(-1, ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2 + 2),
                            ImGuiInputTextFlags_WordWrap);
  ImGui::PopFont();
  ImGui::PopStyleColor();
  ui::FocusRing();
  if (u.lastParsed.find_first_not_of(" \n\r\t") == std::string::npos) {
    if (ui::TextButton("Paste", false, 0, "Paste the code from the clipboard"))
      if (const char* clip = ImGui::GetClipboardText()) std::snprintf(u.codeBuf, sizeof u.codeBuf, "%s", clip);
    ImGui::SameLine(0, theme::kS2);
    ui::TextFit(tr("It starts with gridline-invite: and came by chat, email or a file."), std::max(40.0f, ImGui::GetContentRegionAvail().x), P.textFaint, true);
  } else if (!u.parsedOk) {
    statusLine(u.parseErr.empty() ? std::string(tr("That is not an invite code.")) : u.parseErr, P.danger, theme::lit(P.danger));
  } else if (expired) {
    statusLine(tr("This invite expired: ask for a new code."), P.danger, theme::lit(P.danger));
  } else {
    statusLine(fmt1(tr("Invite from %s"), u.parsed.inviterName), P.led);
  }
  ui::EndSection();
  if (u.parsedOk) {
    ui::BeginSection(tr("INVITE"));
    const float x = formX({"Project", "From", "Fingerprint", "Expires"});
    label("Project", x);
    ui::TextFit(u.parsed.projectName.c_str(), ImGui::GetContentRegionAvail().x, P.text, true);
    label("From", x);
    ui::TextFit(u.parsed.inviterName.c_str(), ImGui::GetContentRegionAvail().x, P.text, true);
    label("Fingerprint", x);
    ImGui::PushFont(a.fontMono, theme::kFontNumber);
    ui::TextFit(u.parsed.inviterFingerprint().c_str(), ImGui::GetContentRegionAvail().x, P.text, true);
    ImGui::PopFont();
    label("Expires", x);
    ui::TextFit(expired ? tr("expired: ask for a new code") : clockTime(static_cast<uint64_t>(u.parsed.expires) * 1000, false).c_str(),
                ImGui::GetContentRegionAvail().x, expired ? theme::lit(P.danger) : P.textDim, true);
    if (compareVersions(u.parsed.appVersion, GRIDLINE_VERSION) != 0) {
      label("Version", x);
      ImGui::PushTextWrapPos(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(theme::lit(P.hot)), "%s",
                         fmt2(tr("They are on Gridline %s and you are on %s: you both need the same version."), u.parsed.appVersion, GRIDLINE_VERSION).c_str());
      ImGui::PopTextWrapPos();
    }
    ui::EndSection();
    ui::BeginSection(tr("WHERE TO STORE IT"));
    if (u.joinFolder.empty()) u.joinFolder = (fs::path(platform::homeEnv() ? platform::homeEnv() : ".") / safeFolderName(u.parsed.projectName)).string();
    const std::string have = c.knownFolder(u.parsed.projectId);
    const float x2 = formX({"Folder"});
    if (!have.empty()) {
      // re-invited: the copy on this computer reconnects, nothing new is made
      label("Folder", x2);
      ui::PathFit(have.c_str(), ImGui::GetContentRegionAvail().x, P.text, true);
      note(tr("You already have this project here. Joining opens it and reconnects you; nothing is copied again."));
    } else {
      label("Folder", x2);
      const float cw = ui::TextButtonWidth("Choose...");
      ui::PathFit(u.joinFolder.c_str(), std::max(60.0f, ImGui::GetContentRegionAvail().x - cw - theme::kS3), P.text, true);
      ImGui::SameLine(0, theme::kS3);
      alignRight(cw);
      if (ui::TextButton("Choose...")) {
        const std::string start = fs::path(u.joinFolder).parent_path().string();
        chooseFolder(a, "Choose where the project folder goes", start, [](const std::string& dir) {
          U().joinFolder = (fs::path(dir) / safeFolderName(U().parsed.projectName)).string();
        });
      }
      note(tr("The project and its audio files are copied here and kept up to date."));
    }
    ui::EndSection();
  }
  if (!u.joinErr.empty()) statusLine(u.joinErr, P.danger, theme::lit(P.danger));
  ImGui::EndChild();
  const float jw = std::max(96.0f, ui::TextButtonWidth("Join")), cw = ui::TextButtonWidth("Cancel##joinwin");
  ui::DialogFooter(cw + theme::kS2 + jw);
  if (ui::TextButton("Cancel##joinwin", false, cw)) a.showJoin = false;
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(!u.parsedOk || expired || !gateOk);
  if (ui::TextButton("Join", true, jw)) {
    if (!c.profileReady()) {
      a.settingsTab = 11;
      a.showSettings = true;
      a.notify(tr("Choose a display name in Settings > Collaboration first."));
    } else {
      const std::string target = c.joinTarget(u.parsed, u.joinFolder);
      if (Session::isCollabFolder(target) && Session::projectIdOf(target) != u.parsed.projectId) {
        u.joinErr = tr("That folder already holds another collaborative project. Choose another folder.");
      } else {
        std::string err;
        if (!startJoin(a, u.parsed, u.joinFolder, &err)) u.joinErr = err;
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::End();
}

// One side of a conflict: who, when, what they changed; Keep / Preview at the
// bottom (all columns of a card end on the same line: `lines` rows of changes).
void conflictColumn(App& a, Session& s, const ConflictTracker& ct, const ConflictObject& c, const ConflictSide& sd, float colW, int lines,
                    bool newest, uint64_t nowMs) {
  Ui& u = U();
  const auto& P = pal();
  const Song& song = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lineH = ImGui::GetTextLineHeightWithSpacing(), headH = 36, pad = theme::kS2;
  const float bodyH = static_cast<float>(lines) * lineH, btnH = s.isAdmin() ? ImGui::GetFrameHeight() + theme::kS2 : 0.0f;
  const float colH = pad + headH + theme::kS1 + bodyH + btnH + pad;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // the column's face first (its contents draw over it)
  dl->AddRectFilled(p, ImVec2(p.x + colW, p.y + colH), P.dark ? theme::mix(P.surface800, P.surface900, 0.4f) : P.surface800, theme::kRadiusLg);
  dl->AddRect(p, ImVec2(p.x + colW, p.y + colH), newest && u.reviewTab == 0 ? theme::withAlpha(P.hot, 0.55f) : P.line, theme::kRadiusLg);
  uint32_t color = 0;
  std::string avatar;
  if (const Member* m = s.member(sd.site)) {
    color = defaultColor(m->signPublic);
    if (const Profile* known = ctlOf(a).profiles().known(m->fingerprint()))
      if (known->color) color = known->color;
    avatar = pictureOf(a, s, *m);  // J-111g
  }
  if (PeerView* pv = s.peer(sd.site))
    if (pv->profile.color) color = pv->profile.color;
  if (sd.site == s.site()) {
    color = s.config().profile.color;
    avatar = s.config().profile.avatar;
  }
  const float ix = p.x + pad, iw = colW - 2 * pad;
  drawAvatar(a, dl, ImVec2(ix + 13, p.y + pad + 15), 13, sd.name, color, avatar, -1);
  const PeerView* pv = s.peer(sd.site);
  const bool approx = pv && pv->clockApprox;
  std::string who = sd.name.empty() ? std::string(tr("(no name)")) : sd.name;
  if (sd.site == s.site()) who += std::string("  (") + tr("you") + ")";
  float tagW = 0;
  const char* tag = tr("newest");
  if (newest && u.reviewTab == 0) tagW = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, tag).x + 10;
  theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(ix + 34, p.y + pad), iw - 34 - tagW - theme::kS1, P.text, who.c_str());
  if (tagW > 0) badge(a, dl, ImVec2(ix + iw - tagW, p.y + pad + 1), tag, theme::lit(P.hot), theme::mix(P.surface800, P.hot, P.dark ? 0.22f : 0.16f));
  const std::string when = clockTime(Hlc::msOf(sd.lastT), approx) + "  \xC2\xB7  " + ago(Hlc::msOf(sd.lastT), nowMs);
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(ix + 34, p.y + pad + 18), iw - 34, P.textDim, when.c_str());
  dl->AddLine(ImVec2(ix, p.y + pad + headH), ImVec2(ix + iw, p.y + pad + headH), P.line);
  // that side's change log for the object, oldest first
  ImGui::SetCursorScreenPos(ImVec2(ix, p.y + pad + headH + theme::kS1));
  ImGui::BeginGroup();
  int shown = 0;
  for (const SideOp* op : sd.ops) {
    if (++shown > 8) {
      char b[64];
      std::snprintf(b, sizeof b, tr("and %d more"), static_cast<int>(sd.ops.size()) - 8);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", b);
      break;
    }
    const std::string line = describe(song, *op);
    ui::TextFit(line.c_str(), iw, ct.isResolved(*op) ? P.textFaint : P.text);
  }
  ImGui::EndGroup();
  if (s.isAdmin()) {
    ImGui::SetCursorScreenPos(ImVec2(ix, p.y + pad + headH + theme::kS1 + bodyH + theme::kS2));
    ImGui::PushID(static_cast<int>(sd.site & 0x7fffffff));
    if (ui::TextButton(u.reviewTab == 0 ? "Keep this one" : "Restore this one", true)) {
      if (s.previewing()) s.endPreview();
      std::string err;
      if (!s.resolve(c.object, sd.site, &err)) a.notify(err, true);
    }
    const bool songObject = c.object.rfind("song", 0) == 0;  // (the project's name has nothing to hear)
    if (songObject) ImGui::SameLine(0, theme::kS2);
    const bool previewingThis = s.previewing() && u.previewObject == c.object && u.previewSite == sd.site;
    if (songObject && ui::TextButton(previewingThis ? "Stop preview" : "Preview", previewingThis, 0, "Hear and see this side for a moment (not saved, not sent)")) {
      if (previewingThis) {
        s.endPreview();
        u.previewObject.clear();
      } else {
        if (s.previewing()) s.endPreview();
        if (s.preview(c.object, sd.site)) {
          u.previewObject = c.object;
          u.previewSite = sd.site;
        }
      }
    }
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(colW, colH));
}

void reviewWindow(App& a) {
  if (!a.showSyncReview) return;
  Ui& u = U();
  const auto& P = pal();
  Session* s = session(a);
  if (!s) {
    a.showSyncReview = false;
    return;
  }
  ui::PlaceNextDialog(ImVec2(780, 560), ImGuiCond_Appearing, ImVec2(kPanelMinW, 360));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const bool vis = ImGui::Begin(ui::windowTitle("Sync review").c_str(), &a.showSyncReview, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!vis) {
    if (!a.showSyncReview && s->previewing()) s->endPreview();
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  const ConflictTracker& ct = s->conflicts();
  const auto openList = ct.open();
  const auto settled = ct.settled();
  header(a, tr("Changes made while you were apart"),
         s->isAdmin() ? std::string(tr("Both sides changed the same things. The newest change is in place; choose what to keep. Nothing is lost."))
                      : std::string(tr("The admin chooses what to keep. Until then the newest change is in place.")));
  char t0[64], t1[64];
  std::snprintf(t0, sizeof t0, "%s (%d)", tr("Open"), static_cast<int>(openList.size()));
  std::snprintf(t1, sizeof t1, "%s (%d)", tr("Resolved"), static_cast<int>(settled.size()));
  const char* tabs[] = {t0, t1};
  ui::TabStrip("##revtabs", tabs, 2, &u.reviewTab);
  const auto& list = u.reviewTab == 0 ? openList : settled;
  const uint64_t nowMs = wallMs();
  if (s->previewing()) {
    std::string who;
    for (const ConflictObject* c : openList)
      if (c->object == u.previewObject)
        for (const auto& sd : ConflictTracker::sides(*c))
          if (sd.site == u.previewSite) who = sd.name;
    if (noticeBar("##preview", who.empty() ? std::string(tr("Previewing a side: not saved or sent")) : fmt1(tr("Previewing %s's side: not saved or sent"), who), P.hot,
                  "Stop preview")) {
      s->endPreview();
      u.previewObject.clear();
    }
    ImGui::Dummy(ImVec2(0, theme::kS1));
  }
  // bulk actions
  if (u.reviewTab == 0 && s->isAdmin() && !openList.empty()) {
    std::map<SiteId, std::string> people;
    for (const ConflictObject* c : openList)
      for (const auto& side : ConflictTracker::sides(*c)) people[side.site] = side.name;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("For every change:"));
    ImGui::SameLine(0, theme::kS2);
    for (const auto& [site, name] : people) {
      ImGui::PushID(static_cast<int>(site & 0x7fffffff));
      const std::string l = fmt1(tr("Keep all of %s's"), name);
      if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(l.c_str()).x + 2 * ImGui::GetStyle().FramePadding.x) ImGui::NewLine();
      if (ui::TextButton(l.c_str())) {
        for (const ConflictObject* c : ct.open()) s->resolve(c->object, site);
      }
      ImGui::PopID();
      ImGui::SameLine(0, theme::kS2);
    }
    auto keepBy = [&](bool newest) {
      std::vector<std::pair<std::string, SiteId>> picks;
      for (const ConflictObject* c : ct.open()) {
        const auto sides = ConflictTracker::sides(*c);
        if (sides.empty()) continue;
        const ConflictSide* best = &sides[0];
        for (const auto& sd : sides)
          if (newest ? sd.lastT > best->lastT : sd.firstT < best->firstT) best = &sd;
        picks.emplace_back(c->object, best->site);
      }
      for (const auto& [obj, site] : picks) s->resolve(obj, site);
    };
    if (ImGui::GetContentRegionAvail().x < buttonsWidth({"Keep newest", "Keep oldest"})) ImGui::NewLine();
    if (ui::TextButton("Keep newest")) keepBy(true);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Keep oldest")) keepBy(false);
    ImGui::Dummy(ImVec2(0, theme::kS1));
  }
  beginBody("##conflicts");
  if (list.empty()) {
    const ImVec2 mn = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ui::EmptyState(ImGui::GetWindowDrawList(), mn, ImVec2(mn.x + sz.x, mn.y + sz.y), u.reviewTab == 0 ? tr("Nothing to review") : tr("Nothing resolved yet"),
                   u.reviewTab == 0 ? tr("Every change merged by itself.") : tr("Resolved changes appear here."));
  }
  const Song& song = a.doc.song();
  for (const ConflictObject* c : list) {
    ImGui::PushID(c->object.c_str());
    const auto sides = ConflictTracker::sides(*c);
    ui::BeginSection(nullptr);
    {  // the card's title: what was changed, how many sides
      const std::string title = objectName(song, c->object);
      char right[48];
      std::snprintf(right, sizeof right, tr("%d versions"), static_cast<int>(sides.size()));
      const float rw = ImGui::CalcTextSize(right).x;
      ImGui::PushFont(a.fontBold, theme::kFontBody);
      ui::TextFit(title.c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x - rw - theme::kS3), P.text);
      ImGui::PopFont();
      ImGui::SameLine();
      alignRight(rw);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", right);
    }
    // side by side; on a narrow window the columns wrap (at least 200 px each)
    size_t newestK = 0;
    int lines = 1;
    for (size_t k = 0; k < sides.size(); ++k) {
      if (sides[k].lastT > sides[newestK].lastT) newestK = k;
      lines = std::max(lines, static_cast<int>(std::min<size_t>(sides[k].ops.size(), 9)));
    }
    const float avail = ImGui::GetContentRegionAvail().x, gap = theme::kS3;
    const int perRow = std::max(1, std::min(static_cast<int>(sides.size()), static_cast<int>((avail + gap) / (200.0f + gap))));
    const float colW = (avail - gap * static_cast<float>(perRow - 1)) / static_cast<float>(perRow);
    for (size_t k = 0; k < sides.size(); ++k) {
      if (k % static_cast<size_t>(perRow)) ImGui::SameLine(0, gap);
      ImGui::PushID(static_cast<int>(k));
      conflictColumn(a, *s, ct, *c, sides[k], colW, lines, k == newestK && sides.size() > 1, nowMs);
      ImGui::PopID();
    }
    ui::EndSection();
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::End();
}

void refusalPopup(App& a) {
  Ui& u = U();
  if (!u.refusalPending) return;
  u.refusalPending = false;
  if (u.refusal.weAreOlder) {
    updui::showRequired(a, fmtVersionOlder(u.refusal), u.refusal.theirVersion);
  } else {
    a.notify(fmtVersionNewer(u.refusal), true);
  }
}

void banner(App& a) {
  Ui& u = U();
  if (!active(a) || u.bannerClosed) return;
  const GateState& g = updui::gate(a);
  if (g.kind != GateState::Kind::UpdateRequired) return;
  // never kick anyone out mid-session: a banner for next time
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const std::string text = fmt1(tr("Gridline %s is out. You'll need it next time you open this project."), g.latest);
  const float w = std::min(vp->WorkSize.x - 40, ImGui::CalcTextSize(text.c_str()).x + 220);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f, vp->WorkPos.y + theme::kTopBarH + 8));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface800));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS3, theme::kS2));
  ImGui::Begin("##collabbanner", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::AlignTextToFramePadding();
  ui::StatusDot(P.hot);
  ImGui::TextColored(theme::vec(P.text), "%s", text.c_str());
  ImGui::SameLine(0, theme::kS3);
  if (ui::TextButton("Update", true)) updui::showRequired(a, fmt1(tr("Update to Gridline %s"), g.latest), g.latest);
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Later")) u.bannerClosed = true;
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

ImGuiWindow* windowByKey(const std::string& key) {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  for (ImGuiWindow* w : g.Windows) {
    if (!w->WasActive || w->Hidden || (w->Flags & ImGuiWindowFlags_Tooltip)) continue;
    if ((w->Flags & ImGuiWindowFlags_ChildWindow) && !w->DockIsActive) continue;  // child regions (docked windows count)
    if (w->DockNode && !w->DockTabIsVisible) continue;
    if (key == ui::stableWindowName(w->Name)) return w;
  }
  return nullptr;
}

// ---- J-111e dialogs: remove someone, removed / stopped, leave / stop sharing, clean up, duplicate folder

// A centred Settings-style modal `width` px wide, opened while `want` holds. It waits
// while another modal is up (the file dialog, unsaved changes): opening one at the
// root of the popup stack would close the other.
bool beginModal(const char* id, bool want, float width) {
  if (!want) {
    if (ImGui::IsPopupOpen(id)) {
      if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    }
    return false;
  }
  if (!ImGui::IsPopupOpen(id)) {
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return false;
    ImGui::OpenPopup(id);
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  width = std::min(width, vp->WorkSize.x - 2 * theme::kS4);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, ui::DialogMaxHeight()));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  return open;
}

// Footer of a modal: hairline, then `width` px of buttons on the right.
void modalFooter(float width) {
  hairline(theme::kS2, theme::kS2);
  alignRight(width);
}

std::string memberName(Session& s, SiteId site) {
  if (const PeerView* pv = s.peer(site); pv && !pv->profile.name.empty()) return pv->profile.name;
  const Member* m = s.member(site);
  return m && !m->name.empty() ? m->name : std::string(tr("(no name)"));
}

// The admin removes someone: what happens, an optional note to them, Cancel / Remove.
void removeDialog(App& a) {
  Ui& u = U();
  const auto& P = pal();
  Session* s = session(a);
  const bool want = u.removeTarget && s && s->isAdmin() && s->isActive(u.removeTarget) && u.removeTarget != s->site();
  if (!want) u.removeTarget = 0;
  if (!beginModal("##collabremove", want, 480)) return;
  const std::string name = memberName(*s, u.removeTarget);
  const PeerView* pv = s->peer(u.removeTarget);
  header(a, fmt2(tr("Remove %s from %s?"), name, s->projectName()), tr("Only you, the admin, can do this."));
  ui::BeginSection(tr("WHAT HAPPENS"));
  note(fmt1(tr("%s stops syncing at once, on every computer in the project, and can't connect again. Invites they have stop working."), name), P.textDim);
  note(tr("Their changes so far stay, and Sync review still shows they made them. Anything they change from now on is dropped."), P.textDim);
  note(tr("Their copy on their own computer stays theirs: Gridline can't take back what they already have."), P.textDim);
  if (!pv || !pv->connected) note(fmt1(tr("%s is offline: they find out the next time they connect to anyone in the project."), name), P.textDim);
  note(tr("There is no undo. To bring them back, re-invite them (People > Removed)."));
  ui::EndSection();
  ui::BeginSection(tr("NOTE TO THEM (OPTIONAL)"));
  ImGui::SetNextItemWidth(-1);
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::InputTextWithHint("##reason", tr("For example: the project is finished"), u.removeReason, sizeof u.removeReason);
  ui::FocusRing();
  note(tr("Shown to them with the removal. Everyone in the project can read it."));
  ui::EndSection();
  if (!u.removeErr.empty()) statusLine(u.removeErr, P.danger, theme::lit(P.danger));
  modalFooter(buttonsWidth({"Cancel##rm", "Remove"}));
  if (ui::TextButton("Cancel##rm") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    u.removeTarget = 0;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::DangerButton("Remove", true, 0, "Removes them for everyone")) {
    std::string err;
    if (s->removeMember(u.removeTarget, cleanName(u.removeReason), &err)) {
      a.notify(fmt1(tr("%s was removed from the project."), name));
      u.removeTarget = 0;
      ImGui::CloseCurrentPopup();
    } else {
      u.removeErr = err;
    }
  }
  ImGui::EndPopup();
}

// The open project stopped syncing by itself: an admin removed you, or stopped sharing.
// Editing is off (a modal); playback goes on. Keep a private copy, or close the project.
void exitDialog(App& a) {
  Ui& u = U();
  const auto& P = pal();
  Controller* c = a.collab.get();
  const Controller::Exited* e = c ? c->exited() : nullptr;
  // the folder chooser closed (cancelled): the dialog comes back
  if (u.exitChoosing && !a.fileDialog.open && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    u.exitChoosing = false;
  if (!beginModal("##collabexit", e && !u.exitChoosing && !a.fileDialog.open, 500)) return;
  const std::string who = e->byName.empty() ? std::string(tr("The admin")) : e->byName;
  const std::string proj = e->projectName.empty() ? std::string(tr("this project")) : e->projectName;
  if (e->kind == ExitKind::Removed)
    header(a, fmt2(tr("You were removed from %s by %s"), proj, who), e->at ? clockTime(e->at, false) : std::string());
  else
    header(a, fmt2(tr("%s stopped sharing %s"), who, proj), e->at ? clockTime(e->at, false) : std::string());
  if (!e->reason.empty()) {
    ui::BeginSection(fmt1(tr("NOTE FROM %s"), who).c_str());
    dim("\xE2\x80\x9C" + e->reason + "\xE2\x80\x9D", P.text);
    ui::EndSection();
  }
  ui::BeginSection(tr("YOUR COPY"));
  note(tr("Nothing syncs any more, so editing is off here. Playback keeps going."), P.textDim);
  note(tr("Keep a private copy to go on working alone: a normal project in a new folder with the music and its audio, without the shared history or anyone's details."),
       P.textDim);
  note(fmt1(tr("Close project leaves the folder %s as it is. It stays in your collaborative projects until you forget it."), fs::path(e->folder).filename().string()));
  ui::EndSection();
  if (!u.exitErr.empty()) statusLine(u.exitErr, P.danger, theme::lit(P.danger));
  modalFooter(buttonsWidth({"Close project", "Keep a private copy..."}));
  if (ui::TextButton("Close project", false, 0, "A new empty project; the folder stays as it is")) {
    u.exitErr.clear();
    ImGui::CloseCurrentPopup();
    a.newProject();  // (the folder's project file was written when it stopped)
    if (a.collab) a.collab->dismissExit();
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Keep a private copy...", true, 0, "Choose where the new folder goes")) {
    u.exitErr.clear();
    u.exitChoosing = true;
    ImGui::CloseCurrentPopup();
    const std::string name = e->projectName.empty() ? std::string("Project") : e->projectName;
    chooseFolder(a, "Choose where the private copy goes", fs::path(e->folder).parent_path().string(), [&a, name](const std::string& dir) {
      Ui& uu = U();
      uu.exitChoosing = false;
      if (!a.collab || !a.collab->exited()) return;
      const std::string dest = freshFolder(dir, safeFolderName(name));
      std::string file, err;
      if (!a.collab->keepPrivateCopy(a.doc, dest, &file, &err)) {
        uu.exitErr = err;
        return;
      }
      a.openProject(file);  // a normal project now (the exit is dismissed with the old one)
      a.collab->dismissExit();
      a.notify(fmt1(tr("Private copy saved in %s."), dest));
      devsig::reach("collab-copy-done");
    });
  }
  ImGui::EndPopup();
}

// What "keep" leaves and deletes, with sizes (Leave, Stop sharing, Make it a normal project).
void keepCaptions(const std::string& folder) {
  const Ui::Sizes& z = folderSizes(folder, U().now);
  const uint64_t kept = z.total > z.history ? z.total - z.history : 0;
  note(fmt2(tr("Stays: the project and its audio (%s) in %s, as a normal project."), formatBytes(kept), fs::path(folder).filename().string()), pal().textDim);
  note(fmt1(tr("Deleted: the shared history and the other members' names and pictures (%s)."), formatBytes(z.history)));
}

// Leave (1), Stop sharing (2), Make it a normal project (3: abandoned).
void leaveDialog(App& a) {
  Ui& u = U();
  const auto& P = pal();
  Controller* c = a.collab.get();
  Session* s = c ? c->session() : nullptr;
  const bool want = u.leaveDialog != 0 && s && !c->leaving();
  if (!want && !(c && c->leaving())) u.leaveDialog = 0;
  if (!beginModal("##collableave", want, 500)) return;
  const int kind = u.leaveDialog;
  const std::string& folder = s->config().folder;
  const std::string folderName = fs::path(folder).filename().string();
  const size_t unacked = s->unackedBatches();
  bool anyoneOnline = false;
  for (const auto& [site, pv] : s->peers()) anyoneOnline = anyoneOnline || (pv.connected && s->isActive(site));
  const bool others = s->activeMembers().size() > 1;
  auto close = [&] {
    u.leaveDialog = 0;
    u.leaveWait = false;
    ImGui::CloseCurrentPopup();
  };
  auto go = [&] {
    std::string err;
    bool ok = false;
    u.leaveName = s->projectName();
    if (kind == 1) ok = c->requestLeave(a.doc, u.leaveMode == 1 ? Controller::LeaveMode::Delete : Controller::LeaveMode::Keep, &err);
    else if (kind == 2) ok = c->requestStopSharing(a.doc, &err);
    else {
      std::string file;
      ok = c->convertToNormal(a.doc, &file, &err);
      if (ok) {
        a.projectPath = file;
        a.projectFormat = SaveFormat::Gridline;
        a.openedFrom.clear();
        a.addRecent(file);
        a.doc.markSaved();
        a.notify(fmt1(tr("%s is a normal project now."), u.leaveName));
      }
    }
    if (ok) {
      close();
    } else {
      u.leaveErr = err.empty() ? std::string(tr("That didn't work.")) : err;
      u.leaveWait = false;
    }
  };
  if (kind == 1) {
    header(a, fmt1(tr("Leave %s?"), s->projectName()), tr("The others keep working on it without you. Only a new invite brings you back."));
    ui::BeginSection(tr("YOUR COPY"));
    ImGui::RadioButton(tr("Keep it as a normal project"), &u.leaveMode, 0);
    ImGui::Indent(theme::kS6);
    keepCaptions(folder);
    ImGui::Unindent(theme::kS6);
    ImGui::Dummy(ImVec2(0, theme::kS1));
#if defined(_WIN32)
    ImGui::BeginDisabled();
#endif
    ImGui::RadioButton(tr("Delete it"), &u.leaveMode, 1);
#if defined(_WIN32)
    ImGui::EndDisabled();
#endif
    ImGui::Indent(theme::kS6);
    note(fmt2(tr("The folder %s (%s) goes to the Trash, with its audio. Empty the Trash to free the space."), folderName,
              formatBytes(folderSizes(folder, u.now).total)),
         P.textDim);
    ImGui::Unindent(theme::kS6);
    ui::EndSection();
  } else if (kind == 2) {
    header(a, fmt1(tr("Stop sharing %s?"), s->projectName()), tr("For everyone: nothing syncs any more, and nobody can join."));
    ui::BeginSection(tr("WHAT HAPPENS"));
    note(tr("Each collaborator keeps their copy and can keep it as a private project. Whoever is offline finds out the next time they are online with someone who knows."),
         P.textDim);
    note(tr("There is no undo: to work together again, make the project collaborative again and invite everyone."));
    ui::EndSection();
    ui::BeginSection(tr("YOUR COPY"));
    keepCaptions(folder);
    ui::EndSection();
  } else {
    const uint64_t last = s->lastContactMs(), nowMs = wallMs();
    const int days = last && nowMs > last ? static_cast<int>((nowMs - last) / 86400000ull) : 0;
    header(a, tr("Make it a normal project?"), days > 0 ? fmt1(tr("Nobody has synced with you since %s."), clockTime(last, false)) : std::string());
    ui::BeginSection(tr("WHAT HAPPENS"));
    if (s->isAdmin() && others) note(tr("You are the admin: this stops sharing for everyone. They keep their copies."), P.textDim);
    else if (others) note(tr("You leave the project; the others keep it and learn it when they are online with you or someone who knows."), P.textDim);
    else note(tr("Nobody else is in the project any more."), P.textDim);
    ui::EndSection();
    ui::BeginSection(tr("YOUR COPY"));
    keepCaptions(folder);
    ui::EndSection();
  }
  // changes nobody has yet
  if (unacked > 0 && others) {
    char b[160];
    std::snprintf(b, sizeof b, tr("No collaborator has confirmed %d of your changes yet."), static_cast<int>(unacked));
    if (u.leaveWait) {
      statusLine(std::string(tr("Waiting for a collaborator to receive them")) + dots(), P.hot, P.text, true);
      note(anyoneOnline ? tr("This takes a moment.") : tr("Nobody is online. They need to open the project."));
    } else {
      statusLine(b, P.hot);
      note(tr("Leave now and nobody gets them. Wait for sync goes on as soon as a collaborator has them."));
    }
  } else if (u.leaveWait) {
    go();  // everything arrived: on with it
  }
  if (!u.leaveErr.empty()) statusLine(u.leaveErr, P.danger, theme::lit(P.danger));
  const bool waitOffer = unacked > 0 && others && !u.leaveWait && kind != 3;
  const char* action = kind == 1 ? (unacked > 0 && others ? "Leave anyway" : u.leaveMode == 1 ? "Leave and delete" : "Leave")
                       : kind == 2 ? (unacked > 0 && others ? "Stop sharing anyway" : "Stop sharing")
                                   : "Make it a normal project";
  const float bw = waitOffer ? buttonsWidth({"Cancel##leave", "Wait for sync", action}) : buttonsWidth({"Cancel##leave", action});
  if (ImGui::IsWindowAppearing()) u.leaveErr.clear();
  modalFooter(bw);
  if (ui::TextButton("Cancel##leave") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
  if (waitOffer) {
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Wait for sync", true, 0, "Goes on by itself once a collaborator has your changes")) u.leaveWait = true;
  }
  ImGui::SameLine(0, theme::kS2);
  if (u.leaveDialog && ui::DangerButton(action, true)) go();
  ImGui::EndPopup();
}

// Clean up unused audio: what goes, how much, what is kept.
void cleanupDialog(App& a) {
  Ui& u = U();
  const auto& P = pal();
  Session* s = session(a);
  if (!s) u.cleanupOpen = false;
  if (!beginModal("##collabclean", u.cleanupOpen, 480)) return;
  header(a, tr("Clean up unused audio?"), tr("Audio files in the project folder that nothing uses any more."));
  ui::BeginSection(tr("WHAT GOES"));
  if (u.unused.empty()) {
    note(tr("Nothing to clean up: every audio file is used by the project, its history, undo or Sync review."), P.textDim);
  } else {
    char b[160];
    std::snprintf(b, sizeof b, tr("%d files, %s"), static_cast<int>(u.unused.size()), formatBytes(u.unusedBytes).c_str());
    statusLine(b, P.hot);
    ImGui::PushFont(a.fontMono, theme::kFontLabel);
    int n = 0;
    for (const auto& [file, bytes] : u.unused) {
      if (++n > 5) {
        ImGui::TextColored(theme::vec(P.textFaint), "%s", fmt1(tr("and %s more"), std::to_string(u.unused.size() - 5)).c_str());
        break;
      }
      ui::TextFit((file + "  " + formatBytes(bytes)).c_str(), ImGui::GetContentRegionAvail().x, P.textFaint);
    }
    ImGui::PopFont();
    note(tr("Kept: everything the project, the shared history, undo / redo or Sync review still refers to. Collaborators' copies are not touched."), P.textDim);
    note(tr("The files are deleted from this computer, not moved to the Trash."));
  }
  ui::EndSection();
  const char* act = "Delete unused files";
  modalFooter(u.unused.empty() ? ui::TextButtonWidth("Close##clean") : buttonsWidth({"Cancel##clean", act}));
  if (u.unused.empty()) {
    if (ui::TextButton("Close##clean", true) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      u.cleanupOpen = false;
      ImGui::CloseCurrentPopup();
    }
  } else {
    if (ui::TextButton("Cancel##clean") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      u.cleanupOpen = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, theme::kS2);
    if (ui::DangerButton(act, true)) {
      const uint64_t freed = s->cleanUnusedAudio();
      u.sizes.at = -100;
      a.notify(fmt1(tr("Freed %s."), formatBytes(freed)));
      u.cleanupOpen = false;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndPopup();
}

// Opening a folder that is a copy of a project this computer has elsewhere: both must
// never sync as the same member.
void duplicateDialog(App& a) {
  Ui& u = U();
  const auto& P = pal();
  if (!beginModal("##collabdup", !u.dupFolder.empty(), 520)) return;
  header(a, tr("This folder is a copy"), tr("The same collaborative project is also at another place on this computer. Both can't sync as you, so only one of them can be the shared one."));
  ui::BeginSection(tr("FOLDERS"));
  const float x = formX({"This copy", "The original"});
  label("This copy", x);
  ui::PathFit(u.dupFolder.c_str(), ImGui::GetContentRegionAvail().x, P.text, true);
  label("The original", x);
  ui::PathFit(u.dupOf.c_str(), ImGui::GetContentRegionAvail().x, P.text, true);
  ui::EndSection();
  ui::BeginSection(tr("WHAT YOU CAN DO"));
  const float bw = std::max({ui::TextButtonWidth("Open the original"), ui::TextButtonWidth("Make this a private copy"), ui::TextButtonWidth("Use this folder instead")});
  const float cw = std::max(40.0f, ImGui::GetContentRegionAvail().x - bw - theme::kS3);
  std::string open;
  if (ui::TextButton("Open the original", true, bw)) open = u.dupOf;
  ImGui::SameLine(0, theme::kS3);
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("The shared one; this copy stays untouched."));
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  if (ui::TextButton("Make this a private copy", false, bw)) {
    Song song;
    std::string err, file;
    const std::string name = Session::projectNameOf(u.dupFolder);
    if (loadProject(Session::projectFileOf(u.dupFolder), song, &err) &&
        exportPlainProject(song, u.dupFolder, u.dupFolder, name.empty() ? fs::path(u.dupFolder).filename().string() : name, true, &file, &err)) {
      open = file;
    } else {
      a.notify(err, true);
    }
  }
  ImGui::SameLine(0, theme::kS3);
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("A normal project of its own: its shared history is deleted, nothing syncs."));
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  if (ui::TextButton("Use this folder instead", false, bw)) {
    ctlOf(a).preferFolder(u.dupFolder);
    open = u.dupFolder;
  }
  ImGui::SameLine(0, theme::kS3);
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("This copy becomes the shared one; don't open the original as shared again."));
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  ui::EndSection();
  modalFooter(ui::TextButtonWidth("Cancel##dup"));
  if (ui::TextButton("Cancel##dup") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    u.dupFolder.clear();
    ImGui::CloseCurrentPopup();
  }
  if (!open.empty()) {
    u.dupFolder.clear();
    ImGui::CloseCurrentPopup();
    a.guardUnsaved([&a, open] { a.openProject(open); });
  }
  ImGui::EndPopup();
}

// A leave / stop sharing finished: the kept project (or nothing, when deleted).
void leaveFinished(App& a) {
  if (!a.collab || a.collab->leaving()) return;
  const Controller::LeaveResult r = a.collab->takeLeaveResult();
  if (!r.done) return;
  Ui& u = U();
  const std::string name = u.leaveName.empty() ? std::string(tr("the project")) : u.leaveName;
  if (r.deleted) {
    a.newProject();
    a.notify(fmt1(tr("You left %s. Its folder is in the Trash."), name));
  } else if (!r.file.empty()) {
    a.projectPath = r.file;
    a.projectFormat = SaveFormat::Gridline;
    a.openedFrom.clear();
    a.addRecent(r.file);
    a.doc.markSaved();
    a.notify(fmt1(tr("%s is a normal project on this computer now. Nothing syncs any more."), name));
  }
  if (!r.error.empty()) a.notify(r.error, true);
  devsig::reach("collab-left");
}

}  // namespace

// ------------------------------------------------------------------- api ----
void setOpenUrl(std::function<bool(const std::string&)> f) { openUrlFn() = std::move(f); }

Controller* controller(App& a) {
  if (a.embedded) return nullptr;
  return &ctlOf(a);
}

Session* session(App& a) { return a.collab ? a.collab->session() : nullptr; }

bool active(const App& a) { return a.collab && a.collab->active(); }

void closeSession(App& a) {
  if (!a.collab) return;
  a.collab->dismissExit();  // J-111e: the removed / stopped project is replaced too
  if (!a.collab->active()) return;
  a.collab->close();  // (a leave on its way finishes first)
  a.collab->takeLeaveResult();
  Ui& u = U();
  u.joining = false;
  u.inviteId.clear();
  u.reinviteFor.clear();
  u.removeTarget = 0;
  u.leaveDialog = 0;
  u.cleanupOpen = false;
}

std::string projectFileIn(const std::string& path) {
  if (path.empty()) return {};
  std::error_code ec;
  fs::path p = fs::path(path).lexically_normal();
  if (p.has_filename() && p.filename() == ".") p = p.parent_path();
  if (!fs::is_directory(p, ec)) return {};
  if (p.filename() == ".gridline-collab") p = p.parent_path();
  if (!Session::isCollabFolder(p.string())) return {};
  return Session::projectFileOf(p.string());
}

bool openIfCollab(App& a, const std::string& projectFile, bool* opened) {
  if (opened) *opened = false;
  if (a.embedded) return false;
  const std::string folder = Session::folderOfProjectFile(projectFile);
  if (folder.empty()) return false;
  // the project needs at least the version that last wrote it, and the newest release
  const std::string minV = Session::minAppVersionOf(folder);
  const GateState& g = updui::gate(a);
  std::string need;
  if (!minV.empty() && compareVersions(GRIDLINE_VERSION, minV) < 0) need = minV;
  else if (g.kind == GateState::Kind::UpdateRequired) need = g.latest;
  if (!need.empty()) {
    updui::showRequired(a, fmt1(tr("This is a collaborative project. Update to Gridline %s to open it."), need), need);
    return true;
  }
  Controller& c = ctlOf(a);
  if (!c.profileReady()) {
    a.settingsTab = 11;
    a.showSettings = true;
    a.notify(tr("This is a collaborative project: choose a display name in Settings > Collaboration, then open it again."), true);
    return true;
  }
  a.engine.stopToStart();
  a.engine.panic(false);
  c.net = netSettingsFrom(a.settings);
  std::string err, upd;
  if (!c.open(a.doc, folder, &err, &upd)) {
    if (!c.duplicateOf().empty()) {  // J-111e: a copy of a project that is also elsewhere: ask
      Ui& u = U();
      u.dupFolder = folder;
      u.dupOf = c.duplicateOf();
      return true;
    }
    if (!upd.empty()) updui::showRequired(a, fmt1(tr("This is a collaborative project. Update to Gridline %s to open it."), upd), upd);
    else a.notify(std::string(tr("Could not open the collaborative project")) + ": " + err, true);
    if (upd.empty()) a.newProject();  // nothing half-open stays behind
    return true;
  }
  if (c.exited()) {
    // J-111e: removed or stopped while this computer was away: the music opens (for a
    // private copy); nothing syncs and Save never writes into the shared folder
    a.projectPath.clear();
    a.projectFormat = SaveFormat::Gridline;
    a.openedFrom.clear();
    afterProjectReplaced(a);
    a.doc.markSaved();
    if (opened) *opened = true;
    return true;
  }
  a.projectPath = c.session()->projectFile();
  a.projectFormat = SaveFormat::Gridline;
  a.openedFrom.clear();
  a.addRecent(a.projectPath);
  afterProjectReplaced(a);
  a.notify(fmt1(tr("Opened %s (collaborative). Reconnecting to collaborators..."), c.session()->projectName()));
  a.showCollab = true;  // who is online, and how to reconnect
  if (opened) *opened = true;
  return true;
}

bool pluginAllowed(const App& a, const std::string& type) { return !active(a) || !isHostedType(type); }

bool fileLoading(App& a, const std::string& path, float* progress) {
  Session* s = session(a);
  if (!s || path.empty()) return false;
  if (path.find("/Audio/") != std::string::npos || path.find("\\Audio\\") != std::string::npos) return s->fileLoading(path, progress);
  if (path.front() == '{' || path.front() == '[') {  // JSON plugin data: any of its files
    PluginState st;
    st.data = path;
    bool any = false;
    float worst = 1.0f;
    forEachStateFileRef(st, [&](std::string& p) {
      float pr = 0;
      if (s->fileLoading(p, &pr)) {
        any = true;
        worst = std::min(worst, pr);
      }
    });
    if (any && progress) *progress = worst;
    return any;
  }
  return false;
}

float flash(App& a, const std::string& object, ImU32* color) {
  if (!active(a)) return 0;
  Ui& u = U();
  auto it = u.flashes.find(object);
  if (it == u.flashes.end()) return 0;
  const double left = it->second.until - a.now;
  if (left <= 0) {
    u.flashes.erase(it);
    return 0;
  }
  if (color) *color = colorOf(it->second.color);
  return static_cast<float>(std::clamp(left / kFlashSec, 0.0, 1.0));
}

void drawLoading(ImDrawList* dl, ImVec2 a0, ImVec2 b0, float progress, bool compact) {
  const auto& P = pal();
  if (b0.x - a0.x < 2 || b0.y - a0.y < 2) return;
  dl->PushClipRect(a0, b0, true);
  if (!compact) {
    dl->AddRectFilled(a0, b0, theme::withAlpha(P.well, 0.55f));
    // moving diagonal stripes
    const float t = static_cast<float>(std::fmod(ImGui::GetTime() * 18.0, 16.0));
    const ImU32 stripe = theme::withAlpha(P.paper, P.dark ? 0.07f : 0.12f);
    for (float x = a0.x - (b0.y - a0.y) - 16 + t; x < b0.x; x += 16)
      dl->AddQuadFilled(ImVec2(x, b0.y), ImVec2(x + 8, b0.y), ImVec2(x + 8 + (b0.y - a0.y), a0.y), ImVec2(x + (b0.y - a0.y), a0.y), stripe);
    char b[48];
    std::snprintf(b, sizeof b, "%s %.0f%%", tr("Loading"), std::clamp(progress, 0.0f, 1.0f) * 100.0f);
    const ImVec2 ts = ImGui::CalcTextSize(b);
    if (b0.x - a0.x > ts.x + 8 && b0.y - a0.y > ts.y)
      dl->AddText(ImVec2(a0.x + 4, a0.y + (b0.y - a0.y - ts.y) * 0.5f), P.text, b);
  }
  const float h = compact ? (b0.y - a0.y) : 3.0f;
  const ImVec2 bar0(a0.x, b0.y - h);
  dl->AddRectFilled(bar0, b0, theme::withAlpha(P.surface950, 0.8f));
  dl->AddRectFilled(bar0, ImVec2(a0.x + (b0.x - a0.x) * std::clamp(progress, 0.0f, 1.0f), b0.y), P.accent);
  dl->PopClipRect();
}

void openPanel(App& a) { a.showCollab = true; }

void openJoin(App& a) {
  Ui& u = U();
  if (!u.joining) {
    u.codeBuf[0] = 0;
    if (const char* dev = std::getenv("GRIDLINE_DEV_JOIN_CODE"); dev && a.uiSoundsDisabled)  // screenshots only
      std::snprintf(u.codeBuf, sizeof u.codeBuf, "%s", dev);
    u.lastParsed.clear();
    u.parsedOk = false;
    u.joinFolder.clear();
    u.joinErr.clear();
  }
  a.showJoin = true;
}

void demoFrame(App& a, double now);

void frame(App& a, double now) {
  Ui& u = U();
  u.now = now;
  if (u.demo) demoFrame(a, now);
  if (u.renameAt == 2 && !a.showInfo && !a.showSettings) u.renameAt = 0;  // J-111h: its window was closed
  if (u.renameAt == 1 && !a.showCollab) finishTitleRename(a, true);  // closed while renaming: clicked away
  // no App Nap while collaborating (the network thread keeps links alive anyway; this keeps edits flowing)
  host::native::keepAwake(!a.embedded && a.collab && a.collab->active());
  if (a.embedded || !a.collab) return;
  Controller& c = *a.collab;
  Session* s = c.session();
  if (s) {
    const ImGuiIO& io = ImGui::GetIO();
    s->setGesture(io.MouseDown[0] || io.MouseDown[1]);
    s->setPresence(u.presence);
  }
  // J-111: a collaborator's change never makes sound here; but a plugin playing on its own here
  // whose channel / sample a collaborator removed or replaced stops cleanly
  const OwnPlaySig own = s ? ownPlaySignature(a.engine, a.doc.song()) : OwnPlaySig();
  const uint64_t applied = s ? s->incrementalApplies() + s->fullApplies() : 0;
  c.frame(now);
  leaveFinished(a);  // J-111e
  s = c.session();
  if (!s) return;
  finishJoin(a);
  if (s->incrementalApplies() + s->fullApplies() != applied) stopOwnPlaybackIfChanged(a.engine, a.doc.song(), own);
  if (s->ready()) a.doc.markSaved();  // saved continuously (op log + project file)
  // J-111f: the project file was renamed after the shared name: the window, Recent files and saves follow it
  if (s->ready() && !u.joining && !a.projectPath.empty() && a.projectPath != s->projectFile() &&
      fs::path(a.projectPath).parent_path() == fs::path(s->projectFile()).parent_path()) {
    a.recentFiles.erase(std::remove(a.recentFiles.begin(), a.recentFiles.end(), a.projectPath), a.recentFiles.end());
    a.projectPath = s->projectFile();
    a.addRecent(a.projectPath);
  }
  if (s->takeReviewRequest() && !(u.demo && u.demo->quietReview)) {
    a.showSyncReview = true;
    a.notify(tr("Some changes made while you were apart touch the same things. Review them in Sync review."));
  }
  // follow a collaborator's playhead
  if (u.follow) {
    const PeerView* pv = s->peer(u.follow);
    if (!pv || !pv->connected) {
      u.follow = 0;
    } else if (pv->presence.playing) {
      const Tick want = static_cast<Tick>(pv->presence.songTick);
      if (!a.engine.playing()) a.engine.play();
      if (std::fabs(static_cast<double>(a.engine.position()) - static_cast<double>(want)) > a.doc.song().ppq) a.engine.setPosition(want);
    } else if (a.engine.playing()) {
      a.engine.stop();
    }
  }
  refusalPopup(a);
}

// ---- scripted runs (--dev-input "<frame> collab <command> ..."; see main.cpp) ----
namespace {

bool readCodeFile(const std::string& path, const char* prefix, std::string& code) {
  std::ifstream f(path);
  std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (all.find(prefix) == std::string::npos) return false;
  code = all;
  return true;
}

void writeFileAtomic(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  std::ofstream(tmp) << text << "\n";
  std::error_code ec;
  fs::rename(tmp, path, ec);
}

SiteId firstOtherMember(const Session& s) {
  for (const Member& m : s.activeMembers())
    if (m.site != s.site()) return m.site;
  return 0;
}

// A member by display name (dev commands).
SiteId memberNamed(Session& s, const std::string& name) {
  for (const Member& m : s.members())
    if (m.site != s.site() && (memberName(s, m.site) == name || m.name == name)) return m.site;
  return 0;
}

std::string devReport(App& a) {
  Session* s = session(a);
  Json r = Json::makeObject();
  r.object["active"] = Json::makeNumber(s ? 1 : 0);
  // J-111e: stopped by itself (removed / stopped sharing), and the project the app holds
  const Controller::Exited* ex = a.collab ? a.collab->exited() : nullptr;
  r.object["exited"] = Json::makeString(!ex ? "" : ex->kind == ExitKind::Removed ? "removed" : "ended");
  if (ex) {
    r.object["exitedBy"] = Json::makeString(ex->byName);
    r.object["exitedReason"] = Json::makeString(ex->reason);
    r.object["exitedFolder"] = Json::makeString(ex->folder);
  }
  r.object["appProject"] = Json::makeString(a.projectPath);
  if (s) {
    bool connected = false;
    size_t unseen = 0;
    for (const auto& [site, pv] : s->peers()) {
      connected = connected || pv.connected;
      unseen += pv.unseen;
    }
    r.object["ready"] = Json::makeNumber(s->ready() ? 1 : 0);
    r.object["connected"] = Json::makeNumber(connected ? 1 : 0);
    r.object["members"] = Json::makeNumber(static_cast<double>(s->members().size()));
    r.object["activeMembers"] = Json::makeNumber(static_cast<double>(s->activeMembers().size()));
    r.object["removed"] = Json::makeNumber(static_cast<double>(s->removals().size()));
    int left = 0;
    for (const Member& m : s->members()) left += s->memberState(m.site) == MemberState::Left ? 1 : 0;
    r.object["left"] = Json::makeNumber(left);
    r.object["unseen"] = Json::makeNumber(static_cast<double>(unseen));
    r.object["projectFile"] = Json::makeString(s->projectFile());
    r.object["name"] = Json::makeString(s->projectName());
    r.object["title"] = Json::makeString(a.windowTitle());
    r.object["appProject"] = Json::makeString(a.projectPath);
    r.object["song"] = Json::makeString(sharedSongDigest(a.doc.song()));
    Json names = Json::makeArray();
    for (const auto& c : a.doc.song().channels) names.array.push_back(Json::makeString(c.name));
    r.object["channels"] = std::move(names);
  }
  return jsonStringify(r, 2);
}

// J-111d: dev anchors. "<window> <kind> <a> <b> <cx> <cy>"; a / b may be chN (the id of
// channel N), mxN (mixer track N), pat (the current pattern), sel (the selected channel);
// cx / cy may be in bars ("2.5bar").
bool devAnchor(App& a, const std::vector<std::string>& args, size_t at, Presence& p) {
  if (args.size() < at + 6) return false;
  const Song& s = a.doc.song();
  auto id = [&](const std::string& v) -> uint32_t {
    if (v.rfind("ch", 0) == 0) {
      const size_t i = static_cast<size_t>(std::atoi(v.c_str() + 2));
      return i < s.channels.size() ? s.channels[i].id : 0;
    }
    if (v.rfind("mx", 0) == 0) {
      const size_t i = static_cast<size_t>(std::atoi(v.c_str() + 2));
      return i < s.mixer.size() ? s.mixer[i].id : 0;
    }
    if (v == "pat") return a.curPattern;
    if (v == "sel") return a.selChannel;
    return static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
  };
  p = Presence();
  p.window = args[at];
  p.kind = canchor::kindByName(args[at + 1]);
  if (p.kind < 0) return false;
  p.a = id(args[at + 2]);
  p.b = id(args[at + 3]);
  auto coord = [&](const std::string& v) {  // "2.5bar": bars of the song (ticks)
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end && std::string(end) == "bar" ? f * static_cast<float>(s.barTicks()) : f;
  };
  p.cx = coord(args[at + 4]);
  p.cy = coord(args[at + 5]);
  if (p.kind == canchor::Window) p.x = p.cx, p.y = p.cy;
  return true;
}

Json presenceJson(const Presence& p) {
  Json j = Json::makeObject();
  j.object["window"] = Json::makeString(p.window);
  j.object["kind"] = Json::makeString(canchor::kindName(p.kind));
  j.object["a"] = Json::makeNumber(p.a);
  j.object["b"] = Json::makeNumber(p.b);
  j.object["cx"] = Json::makeNumber(p.cx);
  j.object["cy"] = Json::makeNumber(p.cy);
  j.object["x"] = Json::makeNumber(p.x);
  j.object["y"] = Json::makeNumber(p.y);
  j.object["plugin"] = Json::makeNumber(p.plugin);  // J-111g
  j.object["pluginSlot"] = Json::makeNumber(p.pluginSlot);
  return j;
}

// Collaborators' cursors as shown here, and what each one is over here (the same
// anchor analysis as for this user's pointer): a cursor maps right when that is
// the anchor its owner reported.
std::string cursorReport(App& a) {
  Ui& u = U();
  Json r = Json::makeObject();
  r.object["me"] = presenceJson(u.presence);
  const ImGuiIO& io = ImGui::GetIO();
  r.object["display"] = Json::makeString(std::to_string(io.DisplaySize.x) + "x" + std::to_string(io.DisplaySize.y));
  r.object["fbScale"] = Json::makeNumber(io.DisplayFramebufferScale.x);
  r.object["barTicks"] = Json::makeNumber(static_cast<double>(a.doc.song().barTicks()));
  r.object["layer"] = Json::makeString("foreground");  // drawn into each viewport's foreground list, after every window
  r.object["arrowH"] = Json::makeNumber(u.pointerH > 4.0f ? u.pointerH : 20.0f);  // J-111g: logical units
  r.object["pointerSource"] = Json::makeString(u.pointerSource);
  Json peers = Json::makeArray();
  if (Session* s = session(a))
    for (const auto& [site, sc] : u.shownCursors) {
      const PeerView* pv = s->peer(site);
      if (!pv) continue;
      Json j = presenceJson(pv->presence);
      j.object["name"] = Json::makeString(pv->profile.name);
      j.object["state"] = Json::makeString(sc.want ? "shown" : "hidden");
      j.object["why"] = Json::makeString(sc.why);
      j.object["vis"] = Json::makeNumber(sc.vis);
      j.object["minVis"] = Json::makeNumber(sc.minVis);  // J-111g: since "collab trace-reset"
      j.object["holding"] = Json::makeNumber(sc.holding ? 1 : 0);
      j.object["occluded"] = Json::makeNumber(sc.occluded ? 1 : 0);
      j.object["covered"] = Json::makeNumber(std::round(sc.covered * 1000) / 1000);  // J-111h
      j.object["alpha"] = Json::makeNumber(sc.vis * sc.idle);
      j.object["drawnX"] = Json::makeNumber(sc.pos.x);
      j.object["drawnY"] = Json::makeNumber(sc.pos.y);
      j.object["drawn"] = Json::makeNumber(sc.vis > 0 && sc.drawnFrame >= 0 && sc.drawnFrame >= ImGui::GetFrameCount() - 1 ? 1 : 0);
      ImGuiWindow* top = nullptr;
      ImGuiWindow* under = nullptr;
      ImGui::FindHoveredWindowEx(sc.pos, false, &top, &under);
      j.object["topWindow"] = Json::makeString(top ? ui::stableWindowName(canchor::rootOf(top)->Name) : "");
      if (ImGuiWindow* w = windowByKey(pv->presence.window)) {  // how far the old mapping (fraction of the window) would be
        const float lx = w->Pos.x + pv->presence.x * w->Size.x - sc.pos.x, ly = w->Pos.y + pv->presence.y * w->Size.y - sc.pos.y;
        j.object["windowFractionOff"] = Json::makeNumber(std::round(std::sqrt(lx * lx + ly * ly) * 10) / 10);
      }
      canchor::Anchor an;
      Json h = Json::makeObject();
      const bool ok = sc.want && canchor::anchorAt(sc.probe, an);
      h.object["ok"] = Json::makeNumber(ok ? 1 : 0);
      if (ok) {
        h.object["kind"] = Json::makeString(canchor::kindName(an.kind));
        h.object["a"] = Json::makeNumber(an.a);
        h.object["b"] = Json::makeNumber(an.b);
        h.object["cx"] = Json::makeNumber(an.cx);
        h.object["cy"] = Json::makeNumber(an.cy);
      }
      j.object["under"] = std::move(h);
      peers.array.push_back(std::move(j));
    }
  r.object["peers"] = std::move(peers);
  return jsonStringify(r, 2);
}

}  // namespace

bool devCommand(App& a, const std::vector<std::string>& args, int frame, ImVec2* pointer) {
  Ui& u = U();
  const std::string cmd = args.empty() ? std::string() : args[0];
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
  Controller& c = ctlOf(a);
  if (cmd == "profile") {  // collab profile <name>
    Profile p = c.profiles().profile();
    p.name = arg(1);
    c.setProfile(p);
  } else if (cmd == "new") {  // collab new <folder> [name]: the open project becomes collaborative there
    const std::string folder = arg(1);
    if (!makeCollaborativeAt(a, folder, args.size() > 2 ? arg(2) : safeFolderName(fs::path(folder).filename().string())))
      std::printf("collab frame %d: new failed: %s\n", frame, u.makeErr.c_str());
  } else if (cmd == "invite") {  // collab invite <file> [minutes]: the code goes to the file once ready
    if (Hub* h = c.hub()) {
      u.dev.inviteFile = arg(1);
      u.dev.inviteId = h->createInvite(args.size() > 2 ? std::max(1, std::atoi(arg(2).c_str())) : 60);
      u.inviteId = u.dev.inviteId;
    }
  } else if (cmd == "join") {  // collab join <code file> <folder>: waits for the file
    u.dev.joinCodeFile = arg(1);
    u.dev.joinFolder = arg(2);
  } else if (cmd == "open") {  // collab open <project file or folder>: the normal open path
    a.openProject(arg(1));
  } else if (cmd == "edit") {  // collab edit <name>: a channel with that name
    a.doc.addChannel(arg(1), "gl.synth");
    a.syncEngine();
  } else if (cmd == "reconnect-code") {  // collab reconnect-code <file>: ours, for the first other member
    u.dev.reconnectOut = arg(1);
  } else if (cmd == "reconnect") {  // collab reconnect <file>: theirs (waits for the file)
    u.dev.reconnectIn = arg(1);
  } else if (cmd == "remove") {  // J-111e: collab remove <name> [reason ...]: the admin removes them
    Session* s = c.session();
    const SiteId who = s ? memberNamed(*s, arg(1)) : 0;
    std::string reason, err;
    for (size_t i = 2; i < args.size(); ++i) reason += (i > 2 ? " " : "") + args[i];
    if (!who || !s->removeMember(who, reason, &err)) std::printf("collab frame %d: remove failed: %s\n", frame, who ? err.c_str() : "no such member");
  } else if (cmd == "reinvite") {  // collab reinvite <name> <file>: an admin's invite that lets a removed member back in
    Session* s = c.session();
    const SiteId who = s ? memberNamed(*s, arg(1)) : 0;
    if (Hub* h = c.hub(); h && who) {
      u.dev.inviteFile = arg(2);
      u.dev.inviteId = h->createInvite(60, who);
      u.inviteId = u.dev.inviteId;
    }
  } else if (cmd == "keep-copy") {  // collab keep-copy <folder>: removed / stopped: a private copy there, opened
    std::string file, err;
    if (c.exited() && c.keepPrivateCopy(a.doc, arg(1), &file, &err)) {
      a.openProject(file);
      c.dismissExit();
      devsig::reach("collab-copy-done");
    } else {
      std::printf("collab frame %d: keep-copy failed: %s\n", frame, c.exited() ? err.c_str() : "not removed / stopped");
    }
  } else if (cmd == "leave") {  // collab leave keep|delete
    std::string err;
    u.leaveName = c.session() ? c.session()->projectName() : std::string();
    if (!c.requestLeave(a.doc, arg(1) == "delete" ? Controller::LeaveMode::Delete : Controller::LeaveMode::Keep, &err))
      std::printf("collab frame %d: leave refused: %s\n", frame, err.c_str());
  } else if (cmd == "stop-sharing") {  // collab stop-sharing (admin)
    std::string err;
    u.leaveName = c.session() ? c.session()->projectName() : std::string();
    if (!c.requestStopSharing(a.doc, &err)) std::printf("collab frame %d: stop sharing refused: %s\n", frame, err.c_str());
  } else if (cmd == "close") {  // collab close: "Close project" of the removed / stopped dialog
    a.newProject();
  } else if (cmd == "ui") {  // collab ui remove <name> | gone | leave | stop | cleanup: dialogs for screenshots
    Session* s = c.session();
    a.showCollab = true;
    if (arg(1) == "remove" && s) u.removeTarget = memberNamed(*s, arg(2));
    else if (arg(1) == "gone") u.showGone = true;
    else if (arg(1) == "leave") u.leaveDialog = 1;
    else if (arg(1) == "stop") u.leaveDialog = 2;
    else if (arg(1) == "cleanup" && s) {
      u.unused = s->unusedAudio();
      u.unusedBytes = 0;
      for (const auto& f : u.unused) u.unusedBytes += f.second;
      u.cleanupOpen = true;
    }
  } else if (cmd == "demo-remove-me") {  // dev demo: Sam becomes admin and removes this user (the removed dialog, for screenshots)
    if (u.demo && c.session() && u.demo->session) {
      std::string reason;
      for (size_t i = 1; i < args.size(); ++i) reason += (i > 1 ? " " : "") + args[i];
      u.demo->removeReason = reason;
      c.session()->transferAdmin(u.demo->session->site());
      u.demo->step = 10;
    }
  } else if (cmd == "rename") {  // collab rename "<name>" (J-111f): renames the project for everyone
    std::string err;
    if (!c.rename(arg(1), &err)) std::printf("collab frame %d: rename failed: %s\n", frame, err.c_str());
  } else if (cmd == "demo-rename") {  // collab demo-rename "<name>": the simulated collaborator renames (--show collab-demo)
    if (u.demo && u.demo->session) u.demo->session->rename(arg(1));
  } else if (cmd == "project-info") {  // collab project-info [rename]: the Project info window (its Shared name row, renaming there)
    a.showInfo = true;
    if (arg(1) == "rename") openRename(a, 2);
  } else if (cmd == "rename-window") {  // collab rename-window: J-111h, renaming in the Collaboration panel's header (screenshots)
    a.showCollab = true;
    openRename(a, 1);
  } else if (cmd == "rename-type") {  // collab rename-type "<text>": what the open rename field holds
    ImGui::ClearActiveID();  // (a field being edited keeps its own copy of the text)
    std::snprintf(u.projNameBuf, sizeof u.projNameBuf, "%s", arg(1).c_str());
    if (arg(2) == "folder") u.alsoFolder = true;
    u.nameFocus = true;
    u.renameFrame = ImGui::GetFrameCount();
  } else if (cmd == "rename-ok") {  // collab rename-ok: its Rename button
    if (u.renameAt) commitRename(a, cleanProjectName(u.projNameBuf));
  } else if (cmd == "name-window") {  // collab name-window: J-111h, the panel's Share this project, name field focused
    makeCollaborative(a);
  } else if (cmd == "share-name") {  // collab share-name "<text>": what Share this project's name field holds
    a.showCollab = true;
    shareDefaults(a, false);
    ImGui::ClearActiveID();  // (a field being edited keeps its own copy of the text)
    std::snprintf(u.shareName, sizeof u.shareName, "%s", arg(1).c_str());
    u.shareFocus = true;
  } else if (cmd == "share-folder") {  // collab share-folder <dir>: where its folder is made (Choose...)
    shareDefaults(a, false);
    u.shareParent = arg(1);
  } else if (cmd == "section-probe") {  // collab section-probe: J-111h, an auto-sizing window with a full-width section
    u.dev.sectionProbe = true;
  } else if (cmd == "share") {  // collab share: its Make collaborative button
    shareDefaults(a, false);
    if (!shareNow(a)) std::printf("collab frame %d: share failed: %s\n", frame, u.makeErr.c_str());
  } else if (cmd == "report") {  // collab report <file>
    writeFileAtomic(arg(1), devReport(a));
  } else if (cmd == "cursors") {  // collab cursors <file>: J-111d, collaborators' cursors as shown here (JSON)
    writeFileAtomic(arg(1), cursorReport(a));
  } else if (cmd == "pixel") {  // collab pixel <file>: J-111d, the colour drawn under the first shown cursor
    u.dev.pixelFile = arg(1);
  } else if (cmd == "point") {  // collab point <anchor>: J-111d, this pointer to where the anchor is here
    Presence p;
    ImVec2 at;
    bool inView = false, ok = false;
    if (devAnchor(a, args, 1, p)) {
      if (p.kind == canchor::Window) {
        if (ImGuiWindow* w = windowByKey(p.window)) at = ImVec2(w->Pos.x + p.x * w->Size.x, w->Pos.y + p.y * w->Size.y), ok = inView = true;
      } else {
        canchor::Anchor an;
        an.kind = p.kind, an.a = p.a, an.b = p.b, an.cx = p.cx, an.cy = p.cy;
        ok = canchor::resolve(an, at, inView);
      }
    }
    std::printf("collab point frame %d: %s at %.1f, %.1f%s\n", frame, ok ? "moved" : "not shown", static_cast<double>(at.x), static_cast<double>(at.y),
                ok && !inView ? " (outside the visible part)" : "");
    std::fflush(stdout);
    if (ok && pointer) {
      *pointer = at;
      return true;
    }
  } else if (cmd == "picture") {  // collab picture <file | #RRGGBB>: J-111g, this user's profile picture (a colour: a plain square)
    std::string bytes, jpg, err;
    const std::string v = arg(1);
    if (v.size() == 7 && v[0] == '#') {
      const uint32_t rgb = static_cast<uint32_t>(std::strtoul(v.c_str() + 1, nullptr, 16));
      png::Image img;
      img.width = img.height = 64;
      img.channels = 3;
      for (int i = 0; i < 64 * 64; ++i)
        for (int k : {16, 8, 0}) img.pixels.push_back(static_cast<uint8_t>((rgb >> k) & 0xFF));
      const std::vector<uint8_t> enc = png::encode(img);
      bytes.assign(enc.begin(), enc.end());
    } else {
      readFileText(v, bytes, kMaxAvatarInputBytes);
    }
    if (sanitizeAvatar(bytes, jpg, &err)) {
      Profile p = c.profiles().profile();
      p.avatar = jpg;
      c.setProfile(p);
    } else {
      std::printf("collab frame %d: picture refused: %s\n", frame, err.c_str());
    }
  } else if (cmd == "panel") {  // collab panel: J-111g, the Collaboration panel (People list)
    a.showCollab = true;
  } else if (cmd == "pictures") {  // collab pictures <file>: J-111g, each member's picture as received and drawn here (after drawing)
    u.dev.picturesFile = arg(1);
  } else if (cmd == "open-editor") {  // collab open-editor <ch index>|none: J-111g, that channel's plugin editor (Gridline's own)
    const Song& sg = a.doc.song();
    const std::string v = arg(1);
    const size_t i = static_cast<size_t>(std::atoi(v.rfind("ch", 0) == 0 ? v.c_str() + 2 : v.c_str()));
    if (i < sg.channels.size()) a.settingsChannel = sg.channels[i].id;
    if (v == "none") a.settingsChannel = kNoId;  // J-111h: collab open-editor none closes it
  } else if (cmd == "demo-cursor-plugin") {  // collab demo-cursor-plugin <ch index | none>: J-111g, the simulated pointer is in that plugin's editor
    if (u.demo) {
      const Song& sg = a.doc.song();
      const std::string v = arg(1);
      const size_t i = static_cast<size_t>(std::atoi(v.rfind("ch", 0) == 0 ? v.c_str() + 2 : v.c_str()));
      u.demo->cursor.plugin = v != "none" && i < sg.channels.size() ? sg.channels[i].id : 0;
      u.demo->cursor.pluginSlot = -1;
    }
  } else if (cmd == "trace-reset") {  // collab trace-reset: J-111g, cursors' lowest visibility starts again (minVis in reports)
    for (auto& [site, sc] : u.shownCursors) sc.minVis = sc.vis;
  } else if (cmd == "demo-cursor") {  // collab demo-cursor <anchor> [x y]: the simulated collaborator's pointer
    if (u.demo && devAnchor(a, args, 1, u.demo->cursor)) {
      if (args.size() > 8) u.demo->cursor.x = std::strtof(arg(7).c_str(), nullptr), u.demo->cursor.y = std::strtof(arg(8).c_str(), nullptr);
      u.demo->cursorSet = true;
    }
  } else if (cmd == "demo-cursor-edge") {  // collab demo-cursor-edge <window> <cover window> <dx>: J-111h, the simulated
    // collaborator's pointer in <window> (window anchor), `dx` px right of <cover window>'s left edge, at its middle height
    ImGuiWindow* w = windowByKey(arg(1));
    ImGuiWindow* cover = windowByKey(arg(2));
    if (u.demo && w && cover) {
      Presence p;
      p.window = arg(1);
      p.kind = canchor::Window;
      p.x = (cover->Pos.x + std::strtof(arg(3).c_str(), nullptr) - w->Pos.x) / w->Size.x;
      p.y = (cover->Pos.y + cover->Size.y * 0.5f - w->Pos.y) / w->Size.y;
      p.cx = p.x, p.cy = p.y;
      u.demo->cursor = p;
      u.demo->cursorSet = true;
    } else {
      std::printf("collab frame %d: demo-cursor-edge: no window %s / %s\n", frame, arg(1).c_str(), arg(2).c_str());
    }
  } else if (cmd == "window") {  // collab window settings|info|none: J-111h, opens that window (or closes them)
    a.showSettings = arg(1) == "settings";
    a.showInfo = arg(1) == "info";
  } else if (cmd == "stall") {  // collab stall <seconds>: the interface thread blocks (a paused interface)
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(std::atof(arg(1).c_str()) * 1000)));
  } else if (cmd == "links") {  // link states to stdout
    std::printf("collab links frame %d:\n%s", frame, c.hub() ? c.hub()->describeLinks().c_str() : "  (no session)\n");
    if (Session* s = c.session())
      for (const auto& [site, pv] : s->peers())
        std::printf("  peer %s %s connected %d unseen %zu sync %zu/%zu\n", siteHex(site).c_str(), pv.profile.name.c_str(), pv.connected ? 1 : 0,
                    pv.unseen, pv.syncDone, pv.syncTotal);
  } else {
    std::printf("collab frame %d: unknown command '%s'\n", frame, cmd.c_str());
  }
  std::fflush(stdout);
  return false;
}

void devFrame(App& a) {
  Ui& u = U();
  Controller* c = a.collab.get();
  Session* s = c ? c->session() : nullptr;
  Hub* h = c ? c->hub() : nullptr;
  if (!u.dev.inviteFile.empty() && h) {
    const Hub::InviteView* iv = h->invite(u.dev.inviteId);
    if (iv && iv->status >= Hub::InviteStatus::Ready && !iv->code.empty()) {
      writeFileAtomic(u.dev.inviteFile, iv->code);
      u.dev.inviteFile.clear();
      devsig::reach("collab-invite-ready");
    }
  }
  if (!u.dev.joinCodeFile.empty()) {
    std::string code;
    if (readCodeFile(u.dev.joinCodeFile, kInvitePrefix, code)) {
      InviteInfo info;
      std::string err;
      if (!parseInviteCode(code, info, &err) || !startJoin(a, info, u.dev.joinFolder, &err)) {
        std::printf("collab: join failed: %s\n", err.c_str());
        devsig::reach("collab-join-failed");
      }
      u.dev.joinCodeFile.clear();
    }
  }
  if (!u.dev.reconnectOut.empty() && s && h)
    if (const SiteId peer = firstOtherMember(*s)) {
      const std::string code = h->reconnectCode(peer);
      if (!code.empty()) {
        writeFileAtomic(u.dev.reconnectOut, code);
        u.dev.reconnectOut.clear();
        devsig::reach("collab-reconnect-code");
      }
    }
  if (!u.dev.reconnectIn.empty() && s && h) {
    std::string code, err;
    if (readCodeFile(u.dev.reconnectIn, kReconnectPrefix, code)) {
      if (!h->acceptReconnectCode(code, &err)) std::printf("collab: reconnect code refused: %s\n", err.c_str());
      u.dev.reconnectIn.clear();
    }
  }
  if (const Controller::Exited* ex = c ? c->exited() : nullptr)  // J-111e
    devsig::reach(ex->kind == ExitKind::Removed ? "collab-removed" : "collab-ended");
  if (!s) return;
  for (const auto& [site, sc] : u.shownCursors) {  // J-111d: settled cursors (waitfor)
    const char* state = nullptr;
    if (sc.want && sc.vis >= 1.0f && std::fabs(sc.pos.x - sc.target.x) < 0.5f && std::fabs(sc.pos.y - sc.target.y) < 0.5f) state = "shown";
    if (!sc.want && sc.vis <= 0.0f) state = "hidden";
    if (!state) continue;
    devsig::reach(std::string("collab-cursor:") + canchor::kindName(sc.last.kind) + ":" + state);
    devsig::reach("collab-cursor-window:" + sc.last.window + ":" + state);
  }
  if (!u.dev.pixelFile.empty()) {  // after the frame is drawn: the colour at a point of the cursor's fill
    Json r = Json::makeObject();
    for (const auto& [site, sc] : u.shownCursors) {
      if ((sc.drawnFrame != ImGui::GetFrameCount() && !sc.occluded) || sc.vis * sc.idle < 0.99f) continue;  // (J-111g: also where a plugin editor covers it)
      const ImGuiIO& io = ImGui::GetIO();
      const int fx = static_cast<int>(sc.fillProbe.x * io.DisplayFramebufferScale.x);
      const int fy = static_cast<int>((io.DisplaySize.y - sc.fillProbe.y) * io.DisplayFramebufferScale.y);
      unsigned char px[4] = {0, 0, 0, 0};
      glReadPixels(fx, fy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      const PeerView* pv = s->peer(site);
      const Member* m = s->member(site);
      const uint32_t rgb = pv && pv->profile.color ? pv->profile.color : (m ? defaultColor(m->signPublic) : 0x8FA0B5);
      const ImU32 want = colorOf(rgb);
      r.object["read"] = Json::makeString(std::to_string(px[0]) + " " + std::to_string(px[1]) + " " + std::to_string(px[2]));
      r.object["want"] = Json::makeString(std::to_string(want & 0xFF) + " " + std::to_string((want >> 8) & 0xFF) + " " + std::to_string((want >> 16) & 0xFF));
      const int d = std::max({std::abs(px[0] - static_cast<int>(want & 0xFF)), std::abs(px[1] - static_cast<int>((want >> 8) & 0xFF)),
                              std::abs(px[2] - static_cast<int>((want >> 16) & 0xFF))});
      r.object["diff"] = Json::makeNumber(d);
      ImGuiWindow* top = nullptr;
      ImGuiWindow* under = nullptr;
      ImGui::FindHoveredWindowEx(sc.fillProbe, false, &top, &under);
      r.object["topWindow"] = Json::makeString(top ? ui::stableWindowName(canchor::rootOf(top)->Name) : "");
      r.object["cursorWindow"] = Json::makeString(sc.window);
      r.object["occluded"] = Json::makeNumber(sc.occluded ? 1 : 0);
      break;
    }
    writeFileAtomic(u.dev.pixelFile, jsonStringify(r, 2));
    u.dev.pixelFile.clear();
  }
  if (!u.dev.picturesFile.empty()) {  // J-111g: each member's picture: announced, received, kept, drawn (a pixel of it)
    Json r = Json::makeObject();
    Json list = Json::makeArray();
    const ImGuiIO& io = ImGui::GetIO();
    for (const Member& m : s->members()) {
      Json j = Json::makeObject();
      const bool me = m.site == s->site();
      const PeerView* pv = me ? nullptr : s->peer(m.site);
      j.object["name"] = Json::makeString(me ? s->config().profile.name : (pv ? pv->profile.name : m.name));
      j.object["me"] = Json::makeNumber(me ? 1 : 0);
      const std::string announced = me ? s->config().profile.avatarHash() : (pv ? pv->profile.avatarHash() : std::string());
      j.object["announced"] = Json::makeString(announced);
      const std::string bytes = pictureOf(a, *s, m);
      Profile tmp;
      tmp.avatar = bytes;
      j.object["received"] = Json::makeString(tmp.avatarHash());
      j.object["bytes"] = Json::makeNumber(static_cast<double>(bytes.size()));
      const Profile* known = me ? nullptr : ctlOf(a).profiles().known(m.fingerprint());
      j.object["kept"] = Json::makeString(known ? known->avatarHash() : std::string());
      const auto it = bytes.empty() ? u.drawnAvatars.end() : u.drawnAvatars.find(tmp.avatarHash());
      const bool drawn = it != u.drawnAvatars.end() && it->second.frame == ImGui::GetFrameCount();
      j.object["drawn"] = Json::makeNumber(drawn ? 1 : 0);
      int w = 0, h = 0;
      std::vector<uint8_t> rgba;
      if (drawn && decodeAvatar(bytes, w, h, rgba)) {  // the picture's middle, as drawn on screen and as decoded
        const ImVec2 c = it->second.c;
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(static_cast<int>(c.x * io.DisplayFramebufferScale.x), static_cast<int>((io.DisplaySize.y - c.y) * io.DisplayFramebufferScale.y), 1, 1,
                     GL_RGBA, GL_UNSIGNED_BYTE, px);
        const size_t o = (static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(w / 2)) * 4;
        j.object["pixelRead"] = Json::makeString(std::to_string(px[0]) + " " + std::to_string(px[1]) + " " + std::to_string(px[2]));
        j.object["pixelWant"] = Json::makeString(std::to_string(rgba[o]) + " " + std::to_string(rgba[o + 1]) + " " + std::to_string(rgba[o + 2]));
        j.object["pixelDiff"] = Json::makeNumber(std::max({std::abs(px[0] - rgba[o]), std::abs(px[1] - rgba[o + 1]), std::abs(px[2] - rgba[o + 2])}));
        j.object["radius"] = Json::makeNumber(it->second.r);
      }
      list.array.push_back(std::move(j));
    }
    r.object["members"] = std::move(list);
    writeFileAtomic(u.dev.picturesFile, jsonStringify(r, 2));
    u.dev.picturesFile.clear();
  }
  for (const Member& m : s->members()) {  // J-111g: collab-picture:<name> once a collaborator's picture is drawn here
    if (m.site == s->site()) continue;
    const std::string bytes = pictureOf(a, *s, m);
    if (bytes.empty()) continue;
    Profile tmp;
    tmp.avatar = bytes;
    const auto it = u.drawnAvatars.find(tmp.avatarHash());
    const PeerView* pv = s->peer(m.site);
    if (it != u.drawnAvatars.end() && it->second.frame == ImGui::GetFrameCount() && pv) {
      devsig::reach("collab-picture:" + pv->profile.name);
      const Profile* known = ctlOf(a).profiles().known(m.fingerprint());
      if (known && known->avatarHash() == tmp.avatarHash()) devsig::reach("collab-picture-kept:" + pv->profile.name);
    }
  }
  devsig::reach("collab-active");
  if (s->ready()) devsig::reach("collab-ready");
  if (h && h->joinView().status == Hub::JoinStatus::Failed) devsig::reach("collab-join-failed");
  bool any = false, all = true, synced = true;
  const size_t people = s->activeMembers().size();  // J-111e: not who was removed or left
  for (const Member& m : s->activeMembers()) {
    if (m.site == s->site()) continue;
    const auto it = s->peers().find(m.site);
    const bool on = it != s->peers().end() && it->second.connected;
    any = any || on;
    all = all && on;
    if (!on || it->second.unseen > 0 || it->second.syncTotal > it->second.syncDone) synced = false;
  }
  if (any && s->ready()) devsig::reach("collab-connected");
  if (all && people > 1 && s->ready()) devsig::reach("collab-all-connected");
  if (synced && people > 1 && s->ready() && s->pendingBatches() == 0) devsig::reach("collab-synced");
  devsig::reach("collab-members:" + std::to_string(people));
  for (const auto& ch : a.doc.song().channels) devsig::reach("collab-channel:" + ch.name);
  if (s->ready()) devsig::reach("collab-name:" + s->projectName());  // J-111f
}

void startDemo(App& a, const std::string& folder, bool quietReview) {
  Ui& u = U();
  std::error_code ec;
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  auto d = std::make_unique<Ui::Demo>();
  d->net = MemFabric::makeNet();
  d->folder = folder;
  Controller& c = ctlOf(a);
  auto net = d->net;
  c.fabricFactory = [net] { return std::make_unique<MemFabric>(net, "Alex"); };
  Profile me = c.profiles().profile();
  if (me.name.empty()) me.name = "Alex";
  me.color = 0x4FB8B0;
  c.setProfile(me);
  std::string err;
  if (!c.create(a.doc, (fs::path(folder) / "Alex").string(), "Demo session", &err)) {
    a.notify("collab demo: " + err, true);
    return;
  }
  a.projectPath = c.session()->projectFile();
  d->profiles = std::make_unique<ProfileStore>((fs::path(folder) / "sam-config").string());
  d->profiles->load();
  // screenshots of long names (dev only): GRIDLINE_DEV_DEMO_NAME names the simulated collaborator
  const char* devName = std::getenv("GRIDLINE_DEV_DEMO_NAME");
  d->profiles->profile().name = devName && *devName ? devName : "Sam";
  d->profiles->profile().color = 0xD8775A;
  d->profiles->save();
  d->invite = c.hub()->createInvite(60);
  u.inviteId = d->invite;  // the panel shows Sam joining with this invite (and the safety code)
  d->quietReview = quietReview;
  u.demo = std::move(d);
}

void demoFrame(App& a, double now) {
  Ui& u = U();
  Ui::Demo& d = *u.demo;
  Controller& c = ctlOf(a);
  Session* host = c.session();
  if (!host) return;
  if (d.hub) d.hub->poll(now);
  if (d.session) d.session->frame(now);
  switch (d.step) {
    case 0: {  // the invite code is ready: Sam joins
      const Hub::InviteView* iv = c.hub()->invite(d.invite);
      if (!iv || iv->status != Hub::InviteStatus::Ready) break;
      InviteInfo info;
      if (!parseInviteCode(iv->code, info)) break;
      Session::Config cfg;
      cfg.folder = (fs::path(d.folder) / "Sam").string();
      cfg.identity = d.profiles->identity();
      cfg.profile = d.profiles->profile();
      cfg.appVersion = GRIDLINE_VERSION;
      cfg.durable = false;
      // Sam's clock runs 90 s ahead: edits made "at the same time" count as made apart (review demo)
      cfg.wallMs = [] { return wallMs() + 90000; };
      d.session = Session::join(d.doc, cfg, info.ticket());
      d.hub = std::make_unique<Hub>(std::make_unique<MemFabric>(d.net, "Sam"), d.profiles->identity(), d.profiles->profile(), currentVersion(),
                                    (fs::path(d.folder) / "sam-config" / "projects").string());
      d.hub->setSession(d.session.get());
      d.hub->join(info);
      d.step = 1;
      break;
    }
    case 1:
      if (d.session && d.session->ready()) {
        d.step = 2;
        d.at = now;
      }
      break;
    case 2: {  // Sam's edits, an audio file (sent slowly), and a conflicting edit
      if (now - d.at < 0.5) break;
      const Song& s = d.doc.song();
      if (!s.channels.empty()) d.doc.renameChannel(s.channels[0].id, s.channels[0].name + " (Sam)");
      if (s.mixer.size() > 2) d.doc.setParam(mixerParam(s.mixer[2].id, MixerParam::Volume), 0.42, false);
      const std::string wav = (fs::path(a.assetsDir) / "sounds" / "export.wav").string();
      const ObjectId ch = d.doc.addChannel("Sam's loop", "gl.sampler");
      d.doc.editChannel(ch, "sample", [&](Channel& x) { x.plugin.data = wav; });
      d.doc.editSong("audio clip", [&](Song& x) {
        Clip cl;
        cl.id = x.allocateId();
        cl.kind = ClipKind::Audio;
        cl.source = ch;
        cl.track = 2;
        cl.start = x.barTicks();
        cl.length = x.barTicks() * 4;
        x.playlist.push_back(cl);
      });
      d.session->fileBudget = 300;  // the file trickles in: the placeholder shows its progress
      // the same knob, changed on both sides before either saw the other
      if (s.channels.size() > 1) {
        const ObjectId lead = s.channels[1].id;
        d.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.3, false);
        a.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.9, false);
        d.doc.renameChannel(lead, "Lead (Sam's take)");
        a.doc.renameChannel(lead, "Lead (Alex's take)");
      }
      d.step = 3;
      break;
    }
    case 3: {  // Sam's cursor over the playlist: on track 2, gliding over bars 2 to 3
      Presence p;
      if (d.cursorSet) {
        p = d.cursor;
      } else {
        p.window = "Playlist";
        p.x = 0.42f + 0.05f * static_cast<float>(std::sin(now * 0.8));
        p.y = 0.38f;
        p.kind = canchor::PlaylistTrack;
        p.a = 1;
        p.cx = static_cast<float>(d.doc.song().barTicks()) * (2.5f + 0.5f * static_cast<float>(std::sin(now * 0.8)));
        p.cy = 0.5f;
      }
      d.session->setPresence(p);
      // screenshots of an offline collaborator (dev only): Sam closes the project after a moment
      if (std::getenv("GRIDLINE_DEV_DEMO_OFFLINE") && now - d.at > 1.5) {
        d.hub.reset();
        d.session.reset();
        d.step = 4;
      }
      break;
    }
    case 10:  // J-111e: Sam is admin now: he removes this user
      if (d.session && d.session->isAdmin()) {
        std::string err;
        if (!d.session->removeMember(host->site(), d.removeReason, &err)) std::printf("collab demo: remove failed: %s\n", err.c_str());
        d.step = 11;
      }
      break;
    default: break;
  }
}

// J-111h (dev, "collab section-probe"): an auto-sizing window (520 px when it opens, at least 300)
// holding a full-width section, as the old name window was: its size must not change from frame to frame.
void sectionProbe() {
  if (!U().dev.sectionProbe) return;
  ui::PlaceNextDialog(ImVec2(520, 0), ImGuiCond_Appearing, ImVec2(300, 0));
  if (ImGui::Begin(ui::windowTitle("Section probe").c_str(), &U().dev.sectionProbe, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ui::BeginSection(tr("SECTION"));
    note("A caption that wraps at the section's edge, so the window's height follows its width.");
    ui::EndSection();
  }
  ImGui::End();
}

void draw(App& a) {
  if (a.embedded) return;
  refusalPopup(a);
  panelWindow(a);
  sectionProbe();
  joinWindow(a);
  reviewWindow(a);
  banner(a);
  // J-111e (modals: one at a time, the end of collaboration first)
  exitDialog(a);
  removeDialog(a);
  leaveDialog(a);
  cleanupDialog(a);
  duplicateDialog(a);
}

// ---- collaborators' cursors (J-111d, J-111g) ----
namespace {

constexpr float kCursorFadeSec = 0.13f;   // fade out once the spot stays hidden here, fade in where it shows
constexpr float kCursorCrossSec = 0.08f;  // J-111g: the brief crossfade of a far jump
constexpr float kCursorHoldSec = 0.28f;   // J-111g: hidden for less than this, no fade (gaps, title bars, edges)
constexpr float kCursorJump = 48.0f;      // to another window farther than this: it snaps there (no glide)
constexpr float kCursorFar = 320.0f;      // J-111g: ... and farther than this, a brief crossfade
constexpr float kDefaultArrowH = 20.0f;   // logical units, until the system's pointer size is known

// The pointer arrow, tip at (0, 0), in arrow units (scaled to the system pointer's
// size). Clockwise on screen, as ImGui's filled shapes want (their anti-aliased
// edge goes outwards). Each corner is rounded by its radius (J-111g: softer, like
// the rest of Gridline's interface; the inner corners of the tail a little less).
constexpr ImVec2 kArrow[] = {{0.0f, 0.0f},   {11.75f, 11.75f}, {6.6f, 11.75f}, {9.25f, 17.5f},
                             {6.75f, 18.75f}, {4.0f, 12.75f},  {0.0f, 16.5f}};
constexpr float kArrowRound[] = {1.1f, 1.5f, 0.8f, 1.1f, 1.1f, 0.8f, 1.3f};
constexpr int kArrowN = static_cast<int>(sizeof kArrow / sizeof kArrow[0]);
constexpr float kArrowRim = 1.0f;                          // the light outline, arrow units
constexpr float kArrowUnitsH = 18.75f + 2.0f * kArrowRim;  // the arrow's height with its outline

// `p` grown outwards by `d` (miter joins, limited at the sharp tip); `convex[i]`: the
// corner turns the polygon's way.
void growPoly(const ImVec2* p, int n, float d, ImVec2* out, bool* convex = nullptr) {
  float area2 = 0;
  for (int i = 0; i < n; ++i) {
    const ImVec2& u = p[i];
    const ImVec2& v = p[(i + 1) % n];
    area2 += u.x * v.y - v.x * u.y;
  }
  const float sgn = area2 < 0 ? 1.0f : -1.0f;
  auto outward = [&](ImVec2 from, ImVec2 to) {
    ImVec2 e(to.x - from.x, to.y - from.y);
    const float l = std::sqrt(e.x * e.x + e.y * e.y);
    if (l > 1e-6f) e = ImVec2(e.x / l, e.y / l);
    return ImVec2(-e.y * sgn, e.x * sgn);
  };
  for (int i = 0; i < n; ++i) {
    const ImVec2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
    if (convex) convex[i] = ((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x)) * area2 > 0;
    const ImVec2 n0 = outward(a, b), n1 = outward(b, c);
    ImVec2 m(n0.x + n1.x, n0.y + n1.y);
    const float ml = std::sqrt(m.x * m.x + m.y * m.y);
    if (ml < 1e-6f) {
      out[i] = ImVec2(b.x + n0.x * d, b.y + n0.y * d);
      continue;
    }
    m = ImVec2(m.x / ml, m.y / ml);
    const float cs = std::max(1e-3f, m.x * n0.x + m.y * n0.y);
    const float len = std::min(d / cs, d * 2.2f);
    out[i] = ImVec2(b.x + m.x * len, b.y + m.y * len);
  }
}

// The polygon with each corner replaced by a curve tangent to both edges (a
// quadratic Bezier through the corner's control point, `seg` segments), the
// radius `r[i]` (+ `grow` on convex corners, - `grow` on the inner ones); the
// result scaled by `s` and moved by `t`.
void roundPoly(const ImVec2* p, int n, const float* r, const bool* convex, float grow, float s, ImVec2 t, ImVector<ImVec2>& out) {
  constexpr int seg = 4;
  out.resize(0);
  for (int i = 0; i < n; ++i) {
    const ImVec2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
    ImVec2 din(b.x - a.x, b.y - a.y), dout(c.x - b.x, c.y - b.y);
    const float lin = std::sqrt(din.x * din.x + din.y * din.y), lout = std::sqrt(dout.x * dout.x + dout.y * dout.y);
    if (lin < 1e-6f || lout < 1e-6f) continue;
    din = ImVec2(din.x / lin, din.y / lin);
    dout = ImVec2(dout.x / lout, dout.y / lout);
    const float rr = std::max(0.0f, r[i] + (convex[i] ? grow : -grow));
    const float d = std::min({rr, lin * 0.45f, lout * 0.45f});
    const ImVec2 q0(b.x - din.x * d, b.y - din.y * d), q1(b.x + dout.x * d, b.y + dout.y * d);
    for (int k = 0; k <= seg; ++k) {
      if (d < 1e-4f && k > 0) break;
      const float u = static_cast<float>(k) / seg, v = 1.0f - u;
      const ImVec2 q(v * v * q0.x + 2 * u * v * b.x + u * u * q1.x, v * v * q0.y + 2 * u * v * b.y + u * u * q1.y);
      out.push_back(ImVec2(q.x * s + t.x, q.y * s + t.y));
    }
  }
}

ImU32 withAlphaMul(ImU32 c, float k) {
  const float a0 = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
  return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(a0 * k, 0.0f, 1.0f) * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
}

// The arrow's height here (logical units): the system pointer's.
float arrowHeight() { return U().pointerH > 4.0f ? std::min(U().pointerH, 160.0f) : kDefaultArrowH; }

// Where the arrow is drawn around its hotspot (shadow included), logical units.
ImRect arrowBounds(ImVec2 at, float h) {
  const float s = h / kArrowUnitsH;
  return ImRect(at.x - 6.0f * s, at.y - 6.0f * s, at.x + 22.0f * s, at.y + 28.0f * s);
}

// A pointer arrow in the collaborator's colour, the size of this computer's own
// pointer (`h`, logical units): rounded corners, a thin light outline, a soft
// shadow; the outline's tip is the hotspot `at`. Drawn only inside `pieces`
// (the parts not under a plugin window, J-111g). Returns a point well inside the
// coloured fill (dev pixel checks).
ImVec2 drawArrow(ImDrawList* dl, ImVec2 at, ImU32 col, float alpha, float fbScale, float h, const std::vector<ImRect>& pieces, ImU32 dim = 0) {
  const auto& P = pal();
  const float s = h / kArrowUnitsH;
  const float px = 1.0f / std::max(0.5f, fbScale);
  // the outline: thin, but at least about a device pixel wide (it would blur away at 1x)
  const float rimU = std::max(kArrowRim, 1.1f * px / s);
  ImVec2 grown[kArrowN];
  bool convex[kArrowN];
  growPoly(kArrow, kArrowN, rimU, grown, convex);
  // the outline's tip on the hotspot (its rounded tip is about the rim's width off
  // the shape's corner), the shape on the device pixel grid: crisp at 1x, 1.5x, 2x
  const ImVec2 tipOff(-0.55f * rimU * s, -1.05f * rimU * s);
  const ImVec2 t(std::round((at.x - tipOff.x) / px) * px, std::round((at.y - tipOff.y) / px) * px);
  ImVector<ImVec2> fill, rim, hair, sh;
  roundPoly(kArrow, kArrowN, kArrowRound, convex, 0.0f, s, t, fill);
  roundPoly(grown, kArrowN, kArrowRound, convex, rimU, s, t, rim);
  ImVec2 hairP[kArrowN];
  const float hairU = rimU + std::max(0.45f, 0.6f * px / s);
  growPoly(kArrow, kArrowN, hairU, hairP);
  roundPoly(hairP, kArrowN, kArrowRound, convex, hairU, s, t, hair);
  // light outline: Gridline's paper white (white on a light theme), the shadow carries it on light panels
  const ImU32 rimCol = P.dark ? (P.paper | IM_COL32_A_MASK) : IM_COL32(255, 255, 255, 255);
  const float shadowK = P.dark ? 1.0f : 0.8f;
  const float oldFringe = dl->_FringeScale;
  dl->_FringeScale = px;  // a one-device-pixel anti-aliased edge (sharp on Retina / HiDPI)
  for (const ImRect& c : pieces) {
    dl->PushClipRect(c.Min, c.Max, true);
    // soft shadow: two widening layers, a little below
    const float layers[][2] = {{rimU + 2.0f, 0.07f}, {rimU + 1.0f, 0.10f}};
    for (const auto& L : layers) {
      ImVec2 g[kArrowN];
      growPoly(kArrow, kArrowN, L[0], g);
      roundPoly(g, kArrowN, kArrowRound, convex, L[0], s, ImVec2(t.x, t.y + 1.0f * s), sh);
      dl->AddConcavePolyFilled(sh.Data, sh.Size, withAlphaMul(IM_COL32(0, 0, 0, 255), L[1] * alpha * shadowK));
    }
    dl->AddConcavePolyFilled(hair.Data, hair.Size, withAlphaMul(IM_COL32(0, 0, 0, 255), 0.22f * alpha * shadowK));  // defines the rim on white
    dl->AddConcavePolyFilled(rim.Data, rim.Size, withAlphaMul(rimCol, 0.97f * alpha));
    dl->AddConcavePolyFilled(fill.Data, fill.Size, withAlphaMul(col | IM_COL32_A_MASK, alpha));
    if (dim & IM_COL32_A_MASK) dl->AddConcavePolyFilled(hair.Data, hair.Size, withAlphaMul(dim, alpha));  // J-111h: under a modal's dim
    dl->PopClipRect();
  }
  dl->_FringeScale = oldFringe;
  return ImVec2(t.x + 3.0f * s, t.y + 9.5f * s);
}

bool sameSpot(const Presence& a, const Presence& b) {
  return a.window == b.window && a.x == b.x && a.y == b.y && a.kind == b.kind && a.a == b.a && a.b == b.b && a.cx == b.cx && a.cy == b.cy &&
         a.plugin == b.plugin && a.pluginSlot == b.pluginSlot;
}

// ---- J-111g: plugin editors ----
// Gridline's own plugin editor windows (one per kind; what they show differs per
// computer), and an installed plugin's own window (outside the main window).
constexpr const char* kInstrumentEditor = "channelsettings";
constexpr const char* kEffectEditor = "fxeditor";
constexpr const char* kPluginWindow = "plugin-window";

bool isPluginEditor(const std::string& key) { return key == kInstrumentEditor || key == kEffectEditor; }

// The plugin a Gridline editor window shows here (owner 0: none).
void editorPlugin(App& a, const std::string& key, uint32_t& owner, int& slot) {
  owner = 0, slot = -1;
  if (key == kInstrumentEditor && a.settingsChannel != kNoId) owner = a.settingsChannel;
  if (key == kEffectEditor && a.fxEditor.first != kNoId) owner = a.fxEditor.first, slot = a.fxEditor.second;
}

// J-111h: a window open here that covers collaborators' cursors pointing at something behind it (J-111g
// did this for plugin editors only): every floating window, dialog, modal, popup and menu, in front of
// the window the cursor points into. Docked panels are part of their dock host's window.
struct Occluder {
  ImRect rect;
  int order = 0;  // display order (front: higher)
  ImGuiViewport* viewport = nullptr;
  bool modal = false;  // a modal (dims what is behind it)
};

int displayOrder(ImGuiWindow* w) {
  if (!w) return -1;
  ImGuiWindow* top = w->RootWindowDockTree ? w->RootWindowDockTree : w;
  return ImGui::FindWindowDisplayIndex(top);
}

std::vector<Occluder> windowOccluders() {
  std::vector<Occluder> out;
  ImGuiContext& g = *ImGui::GetCurrentContext();
  for (ImGuiWindow* w : g.Windows) {
    if (!w->WasActive || w->Hidden || w->Size.x < 1 || w->Size.y < 1) continue;
    if (w->Flags & ImGuiWindowFlags_ChildWindow) continue;  // inside its root (docked windows: their dock host)
    // overlays that draw nothing of their own or take no input (tooltips, drag previews): no cover
    if (w->Flags & (ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground)) continue;
    Occluder o;
    o.rect = w->Rect();
    o.order = displayOrder(w);
    o.viewport = w->Viewport;
    o.modal = (w->Flags & ImGuiWindowFlags_Modal) != 0;
    out.push_back(o);
  }
  return out;
}

// `pieces` minus `o` (each piece split into the parts around it; they never overlap).
void subtractRect(std::vector<ImRect>& pieces, const ImRect& o) {
  std::vector<ImRect> out;
  for (const ImRect& r : pieces) {
    if (!r.Overlaps(o)) {
      out.push_back(r);
      continue;
    }
    if (o.Min.y > r.Min.y) out.push_back(ImRect(r.Min.x, r.Min.y, r.Max.x, o.Min.y));
    if (o.Max.y < r.Max.y) out.push_back(ImRect(r.Min.x, o.Max.y, r.Max.x, r.Max.y));
    const float y0 = std::max(r.Min.y, o.Min.y), y1 = std::min(r.Max.y, o.Max.y);
    if (o.Min.x > r.Min.x) out.push_back(ImRect(r.Min.x, y0, o.Min.x, y1));
    if (o.Max.x < r.Max.x) out.push_back(ImRect(o.Max.x, y0, r.Max.x, y1));
  }
  pieces.swap(out);
}

}  // namespace

void setPointerSize(float logicalH, const std::string& source) {
  Ui& u = U();
  if (const char* dev = std::getenv("GRIDLINE_DEV_POINTER_H"); dev && *dev) {  // screenshots: a given size (logical units)
    u.pointerH = std::strtof(dev, nullptr);
    u.pointerSource = "GRIDLINE_DEV_POINTER_H";
    return;
  }
  u.pointerH = std::isfinite(logicalH) ? logicalH : 0.0f;
  u.pointerSource = source;
}

void beginFrame(App& a) {
  Ui& u = U();
  Session* s = a.embedded ? nullptr : session(a);
  std::vector<canchor::Probe> probes;
  std::vector<uint32_t> wanted;
  if (s) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    probes.push_back({ImGui::GetIO().MousePos, canchor::rootOf(g.HoveredWindow)});
    for (const auto& [site, pv] : s->peers())
      if (pv.presence.kind == canchor::Item) wanted.push_back(pv.presence.a);
    for (auto& [site, sc] : u.shownCursors) {  // what each drawn cursor is over here (dev reports)
      sc.probe = probes.size();
      probes.push_back({sc.pos, sc.vis > 0 ? sc.root : nullptr});
    }
  }
  canchor::beginFrame(s != nullptr, probes, wanted);
}

void drawCursors(App& a) {
  Ui& u = U();
  if (a.embedded) return;
  ImGuiContext& g = *ImGui::GetCurrentContext();
  // this user's cursor: what it is over (an anchor), the window it is in, and the
  // plugin when that window is a plugin editor (J-111g)
  {
    Presence p;
    ImGuiWindow* w = canchor::rootOf(g.HoveredWindow);
    PluginTarget hosted;
    float hx = 0, hy = 0;
    if (w && !(w->Flags & ImGuiWindowFlags_Tooltip) && w->Size.x > 1 && w->Size.y > 1) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      p.window = ui::stableWindowName(w->Name);
      if (p.window.rfind("##", 0) == 0 && p.window != "##topbar") p.window.clear();  // popups, menus: not on the other screen
      p.x = std::clamp((m.x - w->Pos.x) / w->Size.x, 0.0f, 1.0f);
      p.y = std::clamp((m.y - w->Pos.y) / w->Size.y, 0.0f, 1.0f);
      canchor::Anchor an;
      if (!p.window.empty() && canchor::anchorAt(0, an)) {
        p.kind = an.kind;
        p.a = an.a;
        p.b = an.b;
        p.cx = an.cx;
        p.cy = an.cy;
      }
      if (isPluginEditor(p.window)) editorPlugin(a, p.window, p.plugin, p.pluginSlot);
    } else if (hostedEditorUnderMouse(hosted, hx, hy) && hosted.owner != kNoId) {  // an installed plugin's own window
      p.window = kPluginWindow;
      p.x = hx;
      p.y = hy;
      p.plugin = hosted.owner;
      p.pluginSlot = hosted.slot;
    }
    p.playing = a.engine.playing();
    p.songTick = static_cast<double>(a.engine.position());
    u.presence = p;
  }
  Session* s = session(a);
  if (!s || !a.settings.getBool("collab.showCursors")) {
    u.shownCursors.clear();
    return;
  }
  const double dt = u.cursorsAt < 0 ? 0.0 : std::clamp(u.now - u.cursorsAt, 0.0, 0.1);
  u.cursorsAt = u.now;
  // Presence arrives up to 20 times a second: the drawn cursor eases towards it so it
  // glides. J-111g: it fades out only once its spot has stayed hidden here for a
  // moment (a gap between windows, a title bar, a window edge do not blink it), and
  // a move to another window snaps or, when far, crossfades briefly.
  const float ease = 1.0f - static_cast<float>(std::exp(-dt * 16.0));
  const float snap = 1.0f - static_cast<float>(std::exp(-dt * 45.0));
  const float fade = static_cast<float>(dt) / kCursorFadeSec;
  const float cross = static_cast<float>(dt) / kCursorCrossSec;
  const float arrowH = arrowHeight();
  const std::vector<Occluder> occluders = windowOccluders();
  // a modal open here dims everything behind it, collaborators' cursors there too
  ImGuiWindow* modal = ImGui::GetTopMostAndVisiblePopupModal();
  const int modalOrder = modal && modal->WasActive ? displayOrder(modal) : -1;
  const ImU32 modalDim = ImGui::GetColorU32(ImGuiCol_ModalWindowDimBg, g.DimBgRatio);
  for (const auto& [site, pv] : s->peers()) {
    const Presence& pr = pv.presence;
    Ui::Shown& sc = u.shownCursors[site];
    const bool live = pv.connected && pv.presenceAt >= 0 && u.now - pv.presenceAt <= 10;
    bool want = false;
    ImVec2 target = sc.target;
    ImGuiWindow* root = nullptr;
    const char* why = "shown";
    ImGuiWindow* w = live && !pr.window.empty() && pr.window != kPluginWindow ? windowByKey(pr.window) : nullptr;
    uint32_t hereOwner = 0;
    int hereSlot = -1;
    if (isPluginEditor(pr.window)) editorPlugin(a, pr.window, hereOwner, hereSlot);
    if (!live) {
      why = "offline";
    } else if (pr.window.empty()) {
      why = "outside";  // over nothing of Gridline (or a menu, a popup)
    } else if (pr.window == kPluginWindow) {
      // an installed plugin's own window: nothing of Gridline can draw over its interface
      PluginTarget t;
      t.owner = pr.plugin;
      t.slot = pr.pluginSlot;
      why = pr.plugin && hostedEditorOpen(t) ? "plugin-window" : "other-plugin";
    } else if (!w) {
      why = "window-hidden";  // closed, collapsed or another tab here
    } else if (isPluginEditor(pr.window) && (hereOwner != pr.plugin || hereSlot != pr.pluginSlot)) {
      why = "other-plugin";  // the same editor window shows another plugin here
    } else if (pr.kind <= canchor::Window || pr.kind >= canchor::KindCount) {
      target = ImVec2(w->Pos.x + pr.x * w->Size.x, w->Pos.y + pr.y * w->Size.y);
      want = true, root = w, why = "window";
    } else {
      canchor::Anchor an;
      an.kind = pr.kind, an.a = pr.a, an.b = pr.b, an.cx = pr.cx, an.cy = pr.cy;
      bool inView = false;
      if (canchor::resolve(an, target, inView, &root)) {
        want = inView;
        if (!inView) why = "off-view";  // scrolled or zoomed elsewhere here
      } else if (pr.kind == canchor::Item) {  // that item is not drawn here: near it, by the window
        target = ImVec2(w->Pos.x + pr.x * w->Size.x, w->Pos.y + pr.y * w->Size.y);
        want = true, root = w, why = "window";
      } else {
        why = "not-drawn";  // another pattern / channel, a track scrolled away
      }
    }
    if (!sameSpot(pr, sc.last)) {
      sc.last = pr;
      sc.movedAt = u.now;
    }
    if (want) {
      const float dx = target.x - sc.pos.x, dy = target.y - sc.pos.y;
      const float d2 = dx * dx + dy * dy;
      const bool moved = sc.window != pr.window || sc.hiddenSince >= 0;  // another window, or back after a moment away
      if (!sc.placed || sc.vis < 0.05f) {  // appears: fades in where it is
        sc.pos = target;
        sc.placed = true;
        sc.snapping = false;
      } else if (moved && d2 > kCursorFar * kCursorFar) {  // far: a brief crossfade, no trip across the screen
        sc.ghostPos = sc.pos, sc.ghostVis = sc.vis, sc.ghostRoot = sc.root;
        sc.pos = target;
        sc.vis = 0;
        sc.crossing = true;
        sc.snapping = false;
      } else if (moved && d2 > kCursorJump * kCursorJump) {  // farther than a glide: it snaps there, no fade
        sc.snapping = true;
      }
      sc.hiddenSince = -1;
      sc.window = pr.window;
      sc.target = target;
      sc.root = root;
      const float k = sc.snapping ? snap : ease;
      sc.pos.x += (target.x - sc.pos.x) * k;
      sc.pos.y += (target.y - sc.pos.y) * k;
      if (sc.snapping && std::fabs(target.x - sc.pos.x) < 0.5f && std::fabs(target.y - sc.pos.y) < 0.5f) sc.snapping = false;
      sc.vis = std::min(1.0f, sc.vis + (sc.crossing ? cross : fade));
      if (sc.vis >= 1.0f) sc.crossing = false;
    } else {
      if (sc.hiddenSince < 0) sc.hiddenSince = u.now;
      const bool hold = live && u.now - sc.hiddenSince < kCursorHoldSec;  // a moment away: it stays where it was
      if (!hold) sc.vis = std::max(0.0f, sc.vis - fade);
      sc.holding = hold && sc.vis > 0;
    }
    if (want) sc.holding = false;
    sc.minVis = std::min(sc.minVis, sc.vis);  // (dev trace: did it fade at all)
    sc.ghostVis = std::max(0.0f, sc.ghostVis - cross);
    sc.want = want;
    sc.why = why;
    // idle: full for 4 s after the last move, then down to 60% over a second
    const float idle = static_cast<float>(u.now - sc.movedAt);
    sc.idle = idle < 4.0f ? 1.0f : std::max(0.6f, 1.0f - (idle - 4.0f) * 0.4f);
    if (sc.vis <= 0 && sc.ghostVis <= 0) {
      sc.occluded = false;
      continue;
    }
    const Member* m = s->member(site);
    const uint32_t rgb = pv.profile.color ? pv.profile.color : (m ? defaultColor(m->signPublic) : 0);
    const ImU32 col = colorOf(rgb);
    // J-111g / J-111h: windows open here in front of the window the cursor points into cover it
    // (clipped around them every frame, never faded): it reads as behind them. In its own window
    // (also a plugin's editor showing the same plugin) and over what is behind that, it is on top.
    auto piecesFor = [&](ImVec2 at, ImGuiWindow* over, bool* hotspotCovered) {
      std::vector<ImRect> pieces{arrowBounds(at, arrowH)};
      const int order = displayOrder(over);
      if (hotspotCovered) *hotspotCovered = false;
      for (const Occluder& o : occluders) {
        if (o.order <= order) continue;  // its own window, or behind it
        if (over && o.viewport != over->Viewport) continue;  // another viewport (detached window)
        subtractRect(pieces, o.rect);
        if (hotspotCovered && o.rect.Contains(at)) *hotspotCovered = true;
      }
      return pieces;
    };
    // on top of every window: the foreground layer of each viewport showing the spot
    // (one in a single-window build; a detached window's own with multi-viewports)
    auto drawAt = [&](ImVec2 at, float alpha, ImGuiWindow* over, ImVec2* fillProbe, bool* covered) {
      at = ImVec2(std::round(at.x * 4) / 4, std::round(at.y * 4) / 4);
      const std::vector<ImRect> pieces = piecesFor(at, over, covered);
      if (fillProbe) {  // (dev reports: the share of the arrow's box that is covered)
        const ImRect box = arrowBounds(at, arrowH);
        float left = 0;
        for (const ImRect& c : pieces) left += c.GetWidth() * c.GetHeight();
        sc.covered = 1.0f - left / std::max(1.0f, box.GetWidth() * box.GetHeight());
      }
      bool drawn = false;
      for (ImGuiViewportP* vp : g.Viewports) {
        if (!vp->GetMainRect().Contains(at) && !(vp == g.Viewports[0] && g.Viewports.Size == 1)) continue;
        const float fs = vp->FramebufferScale.x > 0 ? vp->FramebufferScale.x : ImGui::GetIO().DisplayFramebufferScale.x;
        const bool dimmed = modalOrder > displayOrder(over);  // behind a modal: under its dim too
        const ImVec2 fp = drawArrow(ImGui::GetForegroundDrawList(vp), at, col, alpha, fs, arrowH, pieces, dimmed ? modalDim : 0);
        if (!drawn && fillProbe) *fillProbe = fp;
        drawn = !pieces.empty();
      }
      return drawn;
    };
    if (sc.ghostVis > 0) drawAt(sc.ghostPos, sc.ghostVis * sc.idle, sc.ghostRoot, nullptr, nullptr);
    sc.occluded = false;
    if (sc.vis > 0) sc.drawnFrame = drawAt(sc.pos, sc.vis * sc.idle, sc.root, &sc.fillProbe, &sc.occluded) ? ImGui::GetFrameCount() : -1;
  }
  for (auto it = u.shownCursors.begin(); it != u.shownCursors.end();) {  // collaborators who left
    const PeerView* pv = s->peer(it->first);
    const bool gone = !pv || (!pv->connected && it->second.vis <= 0 && it->second.ghostVis <= 0);
    it = gone ? u.shownCursors.erase(it) : std::next(it);
  }
}

float topBar(App& a, ImDrawList* dl, float rx, float top, float height) {
  Session* s = session(a);
  if (!s) return rx;
  const auto& P = pal();
  const float r = std::min(11.0f, height * 0.5f - 2);
  const auto members = s->activeMembers();  // J-111e: not who was removed or left
  float x = rx;
  int shown = 0;
  for (auto it = members.rbegin(); it != members.rend(); ++it) {
    const Member& m = *it;
    if (++shown > 6) break;
    x -= 2 * r + 4;
    const bool me = m.site == s->site();
    const PeerView* pv = me ? nullptr : s->peer(m.site);
    Profile prof = me ? s->config().profile : (pv ? pv->profile : Profile());
    if (prof.name.empty()) prof.name = m.name;
    prof.avatar = pictureOf(a, *s, m);  // J-111g
    const ImVec2 c(x + r, top + height * 0.5f);
    drawAvatar(a, dl, c, r, prof.name, prof.color ? prof.color : defaultColor(m.signPublic), prof.avatar, me ? -1 : (pv && pv->connected ? 1 : 0));
    ImGui::SetCursorScreenPos(ImVec2(x, top));
    ImGui::PushID(static_cast<int>(m.site & 0x7fffffff));
    if (ImGui::InvisibleButton("##chip", ImVec2(2 * r + 2, height))) a.showCollab = !a.showCollab;
    if (ImGui::IsItemHovered()) {
      std::string tip = prof.name + (me ? std::string(" (") + tr("you") + ")" : std::string());
      if (!me) {
        const Hub::LinkView lv = a.collab->hub() ? a.collab->hub()->link(m.site) : Hub::LinkView();
        if (lv.state == Hub::LinkView::State::Direct && lv.rttMs >= 0) {
          char b[64];
          std::snprintf(b, sizeof b, "\n%s  \xC2\xB7  %.0f ms", tr("online"), lv.rttMs);
          tip += b;
        } else if (lv.state == Hub::LinkView::State::Relayed) {
          tip += "\n" + fmt1(tr("online through %s"), lv.via);
        } else {
          tip += std::string("\n") + (pv && pv->connected ? tr("online") : tr("offline"));
        }
      }
      ui::Tooltip(tip.c_str());
      ui::Hint(std::string(tr("Collaborators")) + "   (" + tr("click: the Collaboration panel") + ")");
    }
    ImGui::PopID();
  }
  // review badge
  const size_t open = s->conflicts().openCount();
  if (open > 0) {
    char b[16];
    std::snprintf(b, sizeof b, "%d", static_cast<int>(open));
    const ImVec2 ts = ImGui::CalcTextSize(b);
    x -= ts.x + 12;
    const ImVec2 b0(x, top + height * 0.5f - 8), b1(x + ts.x + 8, top + height * 0.5f + 8);
    dl->AddRectFilled(b0, b1, P.hot, 8);
    dl->AddText(ImVec2(b0.x + 4, b0.y + 1), theme::contrastText(P.hot), b);
    ImGui::SetCursorScreenPos(b0);
    if (ImGui::InvisibleButton("##revbadge", ImVec2(b1.x - b0.x, b1.y - b0.y))) a.showSyncReview = true;
    if (ImGui::IsItemHovered()) ui::Tooltip(s->isAdmin() ? tr("Changes made while apart need your review") : tr("Changes awaiting the admin's review"));
  }
  return x - theme::kS2;
}

void fileMenu(App& a) {
  if (a.embedded) return;
  if (ImGui::MenuItem(tr("New collaborative project..."))) {
    a.guardUnsaved([&a] {
      a.newProject();
      a.showCollab = true;
      makeCollaborative(a);
    });
  }
  if (ImGui::MenuItem(tr("Open collaborative project..."))) openCollabFolder(a);
  {
    const auto known = ctlOf(a).knownProjects();
    if (ImGui::BeginMenu(tr("Recent collaborative projects"), !known.empty())) {
      using St = Controller::KnownProject::State;
      for (const auto& k : known) {
        // J-111e / J-111f: same names told apart; removed / stopped / missing ones: the Collaboration panel has their actions
        const char* state = k.state == St::Removed ? tr("Removed") : k.state == St::Ended ? tr("Not shared any more") : k.state == St::Missing ? tr("Missing") : "";
        if (ImGui::MenuItem((knownLabel(known, k) + "##" + k.folder).c_str(), state)) {
          const std::string f = k.folder;
          if (k.state == St::Active) a.guardUnsaved([&a, f] { a.openProject(f); });
          else a.showCollab = true;
        } else if (ImGui::IsItemHovered()) {
          ui::Tooltip(k.folder.c_str());
        }
      }
      ImGui::EndMenu();
    }
  }
  if (ImGui::MenuItem(tr("Join collaborative project..."))) openJoin(a);
  if (ImGui::MenuItem(tr("Collaboration..."), nullptr, a.showCollab)) a.showCollab = !a.showCollab;
}

void projectSection(App& a) {
  if (a.embedded) return;
  const auto& P = pal();
  ui::BeginSection(tr("COLLABORATION"));
  label("Sharing");
  if (Session* s = session(a)) {
    char b[160];
    std::snprintf(b, sizeof b, tr("Shared with %d people"), static_cast<int>(s->activeMembers().size()));
    ImGui::AlignTextToFramePadding();
    ui::StatusDot(P.led);
    ImGui::TextUnformatted(b);
  } else {
    ImGui::AlignTextToFramePadding();
    ui::StatusDot(P.textFaint);
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Only on this computer"));
  }
  if (Session* s = session(a); s && s->ready()) projectNameRow(a, 168.0f);  // J-111f
  label("");
  if (ui::TextButton(active(a) ? "Open the Collaboration panel" : "Collaborate on this project...")) a.showCollab = true;
  ui::EndSection();
}

void projectNameRow(App& a, float labelX) {
  Session* s = session(a);
  if (a.embedded || !s || !s->ready()) return;
  const auto& P = pal();
  label("Shared name", labelX);
  if (U().renameAt == 2) {  // J-111h: renamed right here
    renameEditor(a, labelX);
    return;
  }
  const float bw = ui::TextButtonWidth("Rename...");
  ui::TextFit(s->projectName().c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x - bw - theme::kS2), P.text, true);
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(s->stopped());
  if (ui::TextButton("Rename...##projname", false, 0, "Renames the project for everyone in it")) openRename(a, 2);
  ImGui::EndDisabled();
}

std::string projectTitle(const App& a) {
  if (a.embedded || !a.collab) return {};
  const Session* s = a.collab->session();
  return s && s->ready() && !s->stopped() ? s->projectName() : std::string();
}

void settingsPage(App& a, float fieldW) {
  Ui& u = U();
  const auto& P = pal();
  Controller& c = ctlOf(a);
  ProfileStore& ps = c.profiles();
  Profile prof = ps.profile();
  if (!u.profileInit) {
    u.profileInit = true;
    std::snprintf(u.nameBuf, sizeof u.nameBuf, "%s", prof.name.c_str());
    std::snprintf(u.emailBuf, sizeof u.emailBuf, "%s", prof.email.c_str());
  }
  bool changed = false;
  ui::BeginSection(tr("YOUR PROFILE"));
  note(tr("How collaborators see you. Kept on this computer, not in any project."));
  label("Display name");
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::InputTextWithHint("##cname", tr("Required to host or join"), u.nameBuf, sizeof u.nameBuf)) {
    prof.name = u.nameBuf;
    changed = true;
  }
  ui::FocusRing();
  if (cleanName(u.nameBuf).empty()) {
    label("");
    ImGui::TextColored(theme::vec(P.hot), "%s", tr("A name is needed before you can host or join."));
  }
  label("Contact email");
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::InputTextWithHint("##cemail", tr("Optional"), u.emailBuf, sizeof u.emailBuf)) {
    if (validEmail(u.emailBuf)) {
      prof.email = u.emailBuf;
      changed = true;
    }
  }
  ui::FocusRing();
  if (!validEmail(u.emailBuf)) {
    label("");
    ImGui::TextColored(theme::vec(theme::lit(P.danger)), "%s", tr("That is not an email address."));
  }
  label("");
  bool share = prof.shareEmail;
  if (ImGui::Checkbox(tr("Share my email with collaborators"), &share)) {
    prof.shareEmail = share;
    changed = true;
  }
  ui::Hint("Off: nobody sees it. On: sent only to collaborators, encrypted; never written into project files or logs");
  label("Picture");
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 22;
    ImGui::InvisibleButton("##avatar", ImVec2(2 * r + 4, 2 * r + 4));
    ui::Hint("Drop a PNG or JPEG here to use it as your picture");
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
        std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        std::string bytes, jpg, err;
        if (readFileText(path, bytes, kMaxAvatarInputBytes) && sanitizeAvatar(bytes, jpg, &err)) {
          prof.avatar = jpg;
          changed = true;
          u.avatarErr.clear();
        } else {
          u.avatarErr = err.empty() ? std::string(tr("That file could not be read.")) : err;
        }
      }
      ImGui::EndDragDropTarget();
    }
    drawAvatar(a, dl, ImVec2(p.x + r + 2, p.y + r + 2), r, cleanName(u.nameBuf), ps.color(), prof.avatar, -1);
    ImGui::SameLine(0, theme::kS3);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, std::max(0.0f, r + 2 - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.y * 0.5f - 6)));
    if (ui::TextButton("Choose picture...", false, 0, "A PNG or JPEG; it is cropped to a square and made small (256 px). You can also drop a picture here")) {
      a.fileDialog = {};
      a.fileDialog.open = true;
      a.fileDialog.title = "Choose a profile picture";
      a.fileDialog.dir = platform::homeEnv() ? platform::homeEnv() : ".";
      a.fileDialog.exts = {".png", ".jpg", ".jpeg"};
      a.fileDialog.onOk = [&a](const std::string& path) {
        std::string bytes, jpg, err;
        Ui& uu = U();
        if (readFileText(path, bytes, kMaxAvatarInputBytes) && sanitizeAvatar(bytes, jpg, &err)) {
          Profile pr = ctlOf(a).profiles().profile();
          pr.avatar = jpg;
          ctlOf(a).setProfile(pr);
          uu.avatarErr.clear();
        } else {
          uu.avatarErr = err.empty() ? std::string(tr("That file could not be read.")) : err;
        }
      };
    }
    if (!prof.avatar.empty()) {
      ImGui::SameLine(0, theme::kS2);
      if (ui::TextButton("Remove picture")) {
        prof.avatar.clear();
        changed = true;
      }
    }
    if (!u.avatarErr.empty()) ImGui::TextColored(theme::vec(theme::lit(P.danger)), "%s", u.avatarErr.c_str());
    else ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("PNG or JPEG, or drop one on the circle"));
    ImGui::EndGroup();
  }
  label("Colour");
  {
    uint32_t col = ps.color();
    if (ui::ColorSwatch("##ccol", &col, ImVec2(28, ImGui::GetFrameHeight()))) {
      prof.color = col;
      changed = true;
    }
    ImGui::SameLine();
    if (prof.color && ui::TextButton("Default##col")) {
      prof.color = 0;
      changed = true;
    }
    ui::Hint("The colour of your cursor on your collaborators' screens");
  }
  ui::EndSection();
  if (changed) c.setProfile(prof);

  ui::BeginSection(tr("IDENTITY"));
  label("Fingerprint");
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(ps.identity().fingerprint().c_str());
  ImGui::PopFont();
  ImGui::SameLine();
  if (ui::TextButton("Copy##fp", false, 0, "Copy the fingerprint, to compare with a collaborator by phone or chat")) ImGui::SetClipboardText(ps.identity().fingerprint().c_str());
  label("");
  if (!u.confirmReset) {
    if (ui::DangerButton("Reset identity...", false, 0, "New keys: collaborators have to invite you again to every project")) u.confirmReset = true;
  } else {
    ImGui::BeginGroup();
    statusLine(tr("New keys make you a new person to your collaborators."), P.danger);
    note(tr("They have to invite you again to every collaborative project."));
    if (ui::TextButton("Cancel##reset")) u.confirmReset = false;
    ImGui::SameLine(0, theme::kS2);
    if (ui::DangerButton("Reset identity", true)) {
      if (active(a)) {
        a.notify(tr("Close the collaborative project first."), true);
      } else {
        std::string err;
        if (!ps.resetIdentity(&err)) a.notify(err, true);
      }
      u.confirmReset = false;
    }
    ImGui::EndGroup();
  }
  ui::EndSection();

  ui::BeginSection(tr("CONNECTION"));
  Settings& st = a.settings;
  label("Version");
  {
    const GateState& g = updui::gate(a);
    std::string t;
    ImU32 col = P.textDim;
    if (g.kind == GateState::Kind::UpdateRequired) {
      t = fmt1(tr("Update required (%s)"), g.latest);
      col = P.hot;
    } else if (g.kind == GateState::Kind::UpToDate) {
      t = tr("Up to date for collaboration");
    } else {
      t = updui::gateChecking() ? tr("Checking for updates...") : tr("Couldn't check for updates");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(col), "%s", t.c_str());
    if (g.kind == GateState::Kind::UpdateRequired) {
      ImGui::SameLine();
      if (ui::TextButton("Update##gate")) updui::showRequired(a, fmt1(tr("Update to Gridline %s to collaborate"), g.latest), g.latest);
    }
  }
  label("Automatic port");
  {
    bool pm = st.getBool("collab.portMapping");
    if (ImGui::Checkbox(tr("Let Gridline ask my router to open a port automatically"), &pm)) st.set("collab.portMapping", pm);
    ui::Hint("UPnP-IGD, NAT-PMP or PCP: a temporary mapping while a collaborative project is open, removed when it closes");
  }
  label("STUN server");
  {
    static char buf[256];
    static bool init = false;
    if (!init) std::snprintf(buf, sizeof buf, "%s", st.getString("collab.stunHost").c_str()), init = true;
    ImGui::SetNextItemWidth(fieldW - 90);
    if (ImGui::InputTextWithHint("##stun", tr("none (same network only)"), buf, sizeof buf)) st.set("collab.stunHost", std::string(buf));
    ui::FocusRing();
    ImGui::SameLine();
    int port = static_cast<int>(st.getNumber("collab.stunPort"));
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##stunport", &port, 0, 0)) st.set("collab.stunPort", std::clamp(port, 1, 65535));
    ui::Hint("Tells this computer its public address. The server sees your IP address, nothing else");
  }
  label("TURN relay");
  {
    static char host[256], user[128], pass[128];
    static bool init = false;
    if (!init) {
      std::snprintf(host, sizeof host, "%s", st.getString("collab.turnHost").c_str());
      std::snprintf(user, sizeof user, "%s", st.getString("collab.turnUser").c_str());
      std::string pw;
      secrets::Store().get("collab-turn-password", pw);
      std::snprintf(pass, sizeof pass, "%s", pw.c_str());
      init = true;
    }
    ImGui::SetNextItemWidth(fieldW - 90);
    if (ImGui::InputTextWithHint("##turn", tr("optional: turn.example.org"), host, sizeof host)) st.set("collab.turnHost", std::string(host));
    ui::FocusRing();
    ImGui::SameLine();
    int port = static_cast<int>(st.getNumber("collab.turnPort"));
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##turnport", &port, 0, 0)) st.set("collab.turnPort", std::clamp(port, 1, 65535));
    ui::Hint("Relays the encrypted connection when both networks block direct connections. Only needed for strict networks");
    label("");
    ImGui::SetNextItemWidth((fieldW - theme::kS2) * 0.5f);
    if (ImGui::InputTextWithHint("##turnuser", tr("user"), user, sizeof user)) st.set("collab.turnUser", std::string(user));
    ImGui::SameLine(0, theme::kS2);
    ImGui::SetNextItemWidth((fieldW - theme::kS2) * 0.5f);
    if (ImGui::InputTextWithHint("##turnpass", tr("password"), pass, sizeof pass, ImGuiInputTextFlags_Password)) secrets::Store().set("collab-turn-password", pass);
  }
  label("Test");
  netTestBlock(a, 168.0f);
  ui::EndSection();
}

}  // namespace gl::collabui
