// Gridline - K-106: the Tracker's Collaboration page. Included by CollabUi.cpp only (it draws with
// the panel's own state and actions, which live in that file's unnamed namespace).
//
// In the Tracker, File > Collaboration... (and every other way the Collaboration panel opens: New
// collaborative project..., the palette, a welcome-screen action) swaps the header band to this page,
// as Settings and Files do (TrackerFrame.cpp): the classic screen's look, its bevel boxes and buttons,
// radio and check rows, sunken fields and numbers in the monospaced face. The pages are listed at the
// left with the link state under them and Exit at the foot; each page is a row of titled boxes.
//   No collaborative project open: Share (this project, or join one with a code), Projects (the
//   collaborative projects on this computer), Profile, Connection.
//   A collaborative project open: Session, Members, Invite (and reconnect codes), Profile, Connection.
// What each control does is the panel's (drawInvite, drawMember, shareSection, settingsPage ...): the
// same calls, the same confirmations (the leave, remove and clean-up dialogs are the panel's).

namespace {

namespace tv = ::gl::trackerview;

enum TPage { kTShare, kTProjects, kTSession, kTMembers, kTInvite, kTProfile, kTNetwork, kTCount };
const char* const kTPageNames[kTCount] = {i18n::N_("Share"),  i18n::N_("Projects"), i18n::N_("Session"),   i18n::N_("Members"),
                                          i18n::N_("Invite"), i18n::N_("Profile"),  i18n::N_("Connection")};
const char* const kTPageIds[kTCount] = {"share", "projects", "session", "members", "invite", "profile", "connection"};
const char* const kTPageHints[kTCount] = {
    i18n::N_("Make this project collaborative, or join one with an invite code"),
    i18n::N_("The collaborative projects on this computer: open, locate, keep a copy, forget"),
    i18n::N_("The shared project: its name, admin, folder, files, and leaving it"),
    i18n::N_("Everyone in the project, online or not, and what you can do with each"),
    i18n::N_("Invite codes, reply codes and reconnect codes"),
    i18n::N_("How collaborators see you: your name, colour and picture; your identity"),
    i18n::N_("Address servers, a relay, the router and a connection test")};

// The page being drawn: its draw list and sizes (the band's text size).
struct TLook {
  ImDrawList* dl = nullptr;
  float fs = 13, rowH = 19, line = 1;
} gT;

// A column of rows in a box: x0..x1, the next row's top, its foot, the label column's width.
struct TCol {
  float x0 = 0, x1 = 0, y = 0, foot = 0, lab = 0;
  // Room for `h` more pixels? A row that does not fit is not drawn (dev: the page reports it).
  bool room(float h, const char* what) {
    if (y + h <= foot + 0.5f) {
      return true;
    }
    Ui& u = U();
    if (!u.trk.overflowNow) {
      u.trk.overflowNow = true;
      u.trk.overflowWhatNow = what;
    }
    return false;
  }
  void next(float h) { y += h + gT.line; }
  float fieldX() const { return x0 + 6 + lab; }
};

ImFont* tBold(App& a) { return a.fontBold ? a.fontBold : a.fontRegular; }

float tTextW(ImFont* f, const char* t) { return f->CalcTextSizeA(gT.fs, FLT_MAX, 0, t).x; }

float tBtnW(App& a, const char* label) { return std::ceil(tTextW(a.fontRegular, tr(label)) + 2 * std::max(8.0f, std::round(gT.fs * 0.7f))); }

// A titled box (sunken, as the settings screen's groups) over x0..x1, y0..y1: its column of rows.
TCol tBox(App& a, float x0, float x1, float y0, float y1, const char* title, const std::string& right = std::string()) {
  const auto& P = pal();
  tv::bevelBox(gT.dl, ImVec2(x0, y0), ImVec2(x1, y1), true);
  gT.dl->AddText(tBold(a), gT.fs, ImVec2(x0 + 6, y0 + 3), P.text, tr(title));
  if (!right.empty()) {
    ImFont* mono = tv::monoFont(a);
    const float w = tTextW(mono, right.c_str());
    gT.dl->AddText(mono, gT.fs, ImVec2(std::round(x1 - 6 - w), y0 + 3), P.textDim, right.c_str());
  }
  TCol c;
  c.x0 = x0, c.x1 = x1, c.y = std::round(y0 + gT.fs + 8), c.foot = y1 - 4;
  return c;
}

// Text that may not fit `maxW`: cut with "...", the whole of it on hover.
void tText(ImFont* f, float x, float y, float maxW, ImU32 col, const std::string& t) {
  if (maxW < 8 || t.empty()) {
    return;
  }
  theme::textEllipsis(gT.dl, f, gT.fs, ImVec2(std::round(x), std::round(y)), maxW, col, t.c_str());
  if (tTextW(f, t.c_str()) > maxW && ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + maxW, y + gT.fs + 2))) {
    ui::Tooltip(t.c_str());
  }
}

// A row's label, dim, in the label column.
void tLabel(const TCol& c, const char* label) {
  gT.dl->AddText(ImGui::GetFont(), gT.fs, ImVec2(c.x0 + 6, std::round(c.y + (gT.rowH - gT.fs) * 0.5f)), pal().textDim, tr(label));
}

// A path with its end in view (a long one loses its start), mono, in the values' ink.
void tPath(App& a, float x, float y, float maxW, const std::string& path, ImU32 col = 0) {
  ImFont* mono = tv::monoFont(a);
  std::string t = path;
  while (t.size() > 4 && tTextW(mono, t.c_str()) > maxW) {
    t = "..." + t.substr(std::min(t.size(), static_cast<size_t>(6)));
  }
  gT.dl->AddText(mono, gT.fs, ImVec2(std::round(x), std::round(y + (gT.rowH - gT.fs) * 0.5f)), col ? col : tv::valueInk(), t.c_str());
  if (t != path && ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + maxW, y + gT.rowH))) {
    ui::Tooltip(path.c_str());
  }
}

// Wrapped text from the column's left (or `x`), at most `maxLines` lines (the rest in a tooltip);
// the column moves under it. Returns false when nothing of it fits.
bool tNote(TCol& c, const std::string& text, ImU32 col, int maxLines = 3, float x = -1) {
  if (text.empty()) {
    return true;
  }
  ImFont* f = ImGui::GetFont();
  const float x0 = x >= 0 ? x : c.x0 + 6, w = std::max(20.0f, c.x1 - 6 - x0), lh = std::round(gT.fs + 3);
  std::vector<std::pair<const char*, const char*>> lines;
  const char* s = text.c_str();
  const char* end = s + text.size();
  while (s < end) {
    const char* e = f->CalcWordWrapPosition(gT.fs, s, end, w);
    if (e == s) {
      e = s + 1;
    }
    const char* le = e;
    while (le > s && (le[-1] == ' ' || le[-1] == '\n')) {
      --le;
    }
    lines.emplace_back(s, le);
    s = e;
    while (s < end && (*s == ' ' || *s == '\n')) {
      ++s;
    }
  }
  int n = static_cast<int>(lines.size());
  // as many lines as the box has room for, at most maxLines
  int fit = 0;
  while (fit < std::min(n, maxLines) && c.y + static_cast<float>(fit + 1) * lh - 3 <= c.foot + 0.5f) {
    ++fit;
  }
  if (fit == 0) {
    c.room(lh, text.c_str());
    return false;
  }
  if (fit < n && !U().trk.overflowNow && fit < maxLines) {
    c.room(1e6f, text.c_str());  // (dev: the box was too short for the whole note)
  }
  const float y0 = c.y;
  for (int i = 0; i < fit; ++i) {
    std::string t(lines[static_cast<size_t>(i)].first, lines[static_cast<size_t>(i)].second);
    if (i == fit - 1 && fit < n) {
      t += "...";
    }
    gT.dl->AddText(f, gT.fs, ImVec2(std::round(x0), std::round(c.y + 1)), col, t.c_str());
    c.y += lh;
  }
  if (fit < n && ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x0 + w, c.y))) {
    ui::Tooltip(text.c_str());
  }
  c.y += gT.line + 1;
  return true;
}

// A square lamp, as the classic screens' LEDs: a sunken square, lit in `col`.
void tLamp(ImVec2 centre, ImU32 col, bool pulse = false) {
  const float r = std::max(3.0f, std::round(gT.fs * 0.3f));
  const ImVec2 q0(std::round(centre.x - r), std::round(centre.y - r)), q1(q0.x + 2 * r, q0.y + 2 * r);
  tv::bevelBox(gT.dl, ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), true);
  if (pulse) {
    col = theme::withAlpha(col, 0.55f + 0.45f * static_cast<float>(0.5 + 0.5 * std::sin(ImGui::GetTime() * 4.0)));
  }
  gT.dl->AddRectFilled(q0, q1, col);
}

// A bevel button that the dev script can press by its id ("collab press <id>", the id without "##").
bool tButton(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on = false, const char* hint = nullptr, bool enabled = true) {
  Ui& u = U();
  const std::string key = std::strncmp(id, "##", 2) == 0 ? std::string(id + 2) : std::string(id);
  u.trk.buttonsNow.push_back(enabled ? key : key + "(off)");
  if (!enabled) {
    ImGui::BeginDisabled();
  }
  bool c = tv::bevelButton(a, id, label, p0, p1, on, hint);
  if (!enabled) {
    ImGui::EndDisabled();
  }
  if (!u.trk.press.empty() && u.trk.press == key) {
    std::printf("collab page press %s: %s\n", key.c_str(), enabled ? "done" : "disabled");
    std::fflush(stdout);
    u.trk.press.clear();
    c = true;
  }
  return c && enabled;
}

// A button of its label's width at the column's right edge (or ending at `right`), on row y.
bool tButtonRight(App& a, const TCol& c, const char* id, const char* label, const char* hint = nullptr, bool enabled = true, float right = -1, bool on = false) {
  const float w = tBtnW(a, label), x1 = right >= 0 ? right : c.x1 - 4;
  return tButton(a, id, label, ImVec2(x1 - w, c.y), ImVec2(x1, c.y + gT.rowH), on, hint, enabled);
}

// A text field in the band's look: sunk into a bevel well, square, no fill of its own.
bool tField(App& a, const char* id, const char* hint, char* buf, size_t n, ImVec2 p, float w, ImGuiInputTextFlags flags = 0, bool mono = false,
            bool focus = false) {
  const auto& P = pal();
  w = std::max(24.0f, w);
  tv::bevelBox(gT.dl, p, ImVec2(p.x + w, p.y + gT.rowH), true);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, std::max(0.0f, std::floor((gT.rowH - 2 - gT.fs) * 0.5f))));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::withAlpha(P.surface700, 0.35f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  if (mono) {
    ImGui::PushFont(tv::monoFont(a));
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x + 1, p.y + 1));
  ImGui::SetNextItemWidth(w - 2);
  if (focus) {
    ImGui::SetKeyboardFocusHere();
  }
  const bool r = ImGui::InputTextWithHint(id, hint, buf, n, flags);
  const bool active = ImGui::IsItemActive();
  if (mono) {
    ImGui::PopFont();
  }
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(3);
  if (active) {
    gT.dl->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + gT.rowH), theme::withAlpha(P.hot, 0.7f));
  }
  return r;
}

// A number field (a port): the value in the monospaced face, in the values' ink.
bool tNumber(App& a, const char* id, int* v, ImVec2 p, float w, int lo, int hi) {
  char b[16];
  std::snprintf(b, sizeof b, "%d", *v);
  ImGui::PushStyleColor(ImGuiCol_Text, tv::valueInk());
  const bool done = tField(a, id, "", b, sizeof b, p, w, ImGuiInputTextFlags_CharsDecimal, true);
  ImGui::PopStyleColor();
  if (done) {
    const int n = std::atoi(b);
    if (n != *v) {
      *v = std::clamp(n, lo, hi);
      return true;
    }
  }
  return false;
}

// A check row of the column's width.
bool tCheck(App& a, TCol& c, const char* id, const char* label, bool on, const char* hint, float x = -1) {
  const float x0 = x >= 0 ? x : c.x0 + 4;
  return tv::checkRow(a, id, label, ImVec2(x0, c.y), ImVec2(c.x1 - 4, c.y + gT.rowH), on, hint);
}

// The project name's caption (shareSection's nameCaption, in a row).
void tNameCaption(TCol& c, const char* typed, const std::string& clean, const char* ok, float x) {
  const auto& P = pal();
  size_t n = 0;
  for (const char* q = typed; *q; ++q) {
    n += (static_cast<unsigned char>(*q) & 0xC0) != 0x80;
  }
  if (clean.empty()) {
    tNote(c, tr("A project needs a name."), theme::lit(P.danger), 1, x);
  } else if (n > kMaxProjectNameChars) {
    char b[96];
    std::snprintf(b, sizeof b, tr("Names are at most %d characters: the rest is left out."), static_cast<int>(kMaxProjectNameChars));
    tNote(c, b, P.hot, 2, x);
  } else {
    tNote(c, ok, P.textFaint, 2, x);
  }
}

// A list box's rows: a sunken box, `n` rows of the band's row height from `first`, a bevel scroll bar
// at the right when they do not all show. Returns the rows in view (from first) and the rows' width.
struct TList {
  float x0 = 0, x1 = 0, y0 = 0, y1 = 0, rowH = 0;
  int first = 0, shown = 0;
};
TList tListBox(App& a, const char* id, ImVec2 p0, ImVec2 p1, int n, double* first) {
  TList L;
  tv::bevelBox(gT.dl, p0, p1, true);
  L.rowH = gT.rowH;
  const float h = p1.y - p0.y - 4;
  const int fits = std::max(1, static_cast<int>(std::floor(h / L.rowH)));
  float right = p1.x - 2;
  if (n > fits) {
    const float sw = std::round(std::max(12.0f, gT.fs + 1));
    tv::vScrollBar(a, id, ImVec2(p1.x - 2 - sw, p0.y + 2), ImVec2(p1.x - 2, p1.y - 2), first, fits, n, 1);
    right = p1.x - 2 - sw - 1;
    if (ImGui::IsMouseHoveringRect(p0, p1) && ImGui::GetIO().MouseWheel != 0) {
      *first = std::clamp(*first - ImGui::GetIO().MouseWheel, 0.0, static_cast<double>(n - fits));
    }
  } else {
    *first = 0;
  }
  *first = std::clamp(std::round(*first), 0.0, static_cast<double>(std::max(0, n - fits)));
  L.x0 = p0.x + 2, L.x1 = right, L.y0 = p0.y + 2, L.y1 = p1.y - 2;
  L.first = static_cast<int>(*first);
  L.shown = std::min(fits, n - L.first);
  return L;
}

// ---- the pages ----

// Share: this project made collaborative (shareSection), and joining with a code (the Join window).
void tPageShare(App& a, ImVec2 t0, ImVec2 t1, float pad) {
  Ui& u = U();
  const auto& P = pal();
  Controller& c = ctlOf(a);
  shareDefaults(a, false);
  const GateState& g = updui::gate(a);
  const bool gateOk = g.kind != GateState::Kind::UpdateRequired;
  const bool can = gateOk && networkBuiltIn();
  const float W = t1.x - t0.x - 3 * pad;
  const float xa = t0.x + pad, xb = std::round(xa + W * 0.6f), xc = xb + pad, y0 = t0.y + pad, y1 = t1.y - pad;
  // -- this project
  TCol col = tBox(a, xa, xb, y0, y1, "Share this project");
  col.lab = tv::labelColumn(a, {"Name", "Folder"}, tv::space::kGroup);
  const std::string name = cleanProjectName(u.shareName);
  const std::string target = shareTarget(name);
  bool enter = false;
  if (col.room(gT.rowH, "share name")) {
    tLabel(col, "Name");
    if (!can) {
      ImGui::BeginDisabled();
    }
    const bool focus = u.shareFocus && can;
    enter = tField(a, "##tsharename", tr("Project name"), u.shareName, sizeof u.shareName, ImVec2(col.fieldX(), col.y), col.x1 - 4 - col.fieldX(),
                   ImGuiInputTextFlags_EnterReturnsTrue, false, focus);
    if (focus) {
      u.shareFocus = false;
    }
    ui::Hint("Everyone you invite sees this name. The folder is named after it.");
    if (!can) {
      ImGui::EndDisabled();
    }
    col.next(gT.rowH);
  }
  tNameCaption(col, u.shareName, name, tr("Everyone you invite sees this name. The folder is named after it."), col.fieldX());
  if (col.room(gT.rowH, "share folder")) {
    tLabel(col, "Folder");
    const float cw = tBtnW(a, "Choose...");
    tPath(a, col.fieldX(), col.y, col.x1 - 4 - cw - tv::space::kGroup - col.fieldX(), target);
    if (tButtonRight(a, col, "##tsharechoose", "Choose...", "Where the project's folder is made", can)) {
      chooseFolder(a, "Choose where the project folder goes", u.shareParent, [](const std::string& dir) { U().shareParent = dir; });
    }
    col.next(gT.rowH);
  }
  if (u.shareChecked != target || u.now - u.shareCheckedAt > 1.0) {  // (looked at again every second)
    std::error_code ec;
    u.shareChecked = target;
    u.shareCheckedAt = u.now;
    u.shareTaken = fs::exists(fs::path(target) / ".gridline-collab", ec);
    u.shareExists = !u.shareTaken && fs::is_directory(target, ec) && !fs::is_empty(target, ec);
  }
  if (u.shareTaken) {
    tNote(col, tr("That folder already holds a collaborative project: choose another place or name."), theme::lit(P.danger), 2, col.fieldX());
  } else if (u.shareExists) {
    tNote(col, tr("That folder already exists: the project is added to what is in it."), P.hot, 2, col.fieldX());
  } else {
    tNote(col, tr("A new folder: the project and every audio file it uses are copied into it. Your project file stays as it is."), P.textFaint, 2,
          col.fieldX());
  }
  tNote(col, tr("Undo starts fresh from here: each person then undoes only their own edits."), P.textFaint, 2, col.fieldX());  // K-141 (D8)
  // what stands in the way (the version gate, a display name, plugins, an error), over the button row
  const float footY = std::round(col.foot - gT.rowH);
  TCol msg = col;
  msg.foot = footY - gT.line;
  if (!networkBuiltIn()) {
    tNote(msg, tr("This build of Gridline has no collaboration networking."), P.hot, 2);
  } else if (!gateOk) {
    tNote(msg, fmt1(tr("Update to Gridline %s to use collaborative features."), g.latest), P.hot, 2);
  } else if (!c.profileReady()) {
    tNote(msg, tr("Choose a display name first: collaborators see it."), P.hot, 2);
  }
  if (!u.makeErr.empty()) {
    tNote(msg, u.makeErr, theme::lit(P.danger), 2);
  }
  {
    TCol foot = col;
    foot.y = footY;
    float right = col.x1 - 4;
    if (!gateOk) {
      if (tButtonRight(a, foot, "##tshareupdate", "Update", "Update Gridline to collaborate")) {
        updui::showRequired(a, fmt1(tr("Update to Gridline %s to collaborate"), g.latest), g.latest);
      }
      right -= tBtnW(a, "Update") + tv::space::kGap;
    } else if (can && !c.profileReady()) {
      if (tButtonRight(a, foot, "##tshareprofile", "Profile", "Your display name and the rest of your profile (the Profile page)")) {
        u.trk.page = kTProfile;
      }
      right -= tBtnW(a, "Profile") + tv::space::kGap;
    }
    const bool ok = can && !name.empty() && !u.shareTaken && c.profileReady();
    if (tButtonRight(a, foot, "##tsharemake", "Make collaborative", "Copies the project into the folder and shares it from there", ok, right) ||
        (enter && ok)) {
      shareNow(a);
    }
  }
  // -- join
  TCol j = tBox(a, xc, t1.x - pad, y0, y1, "Join");
  if (!gateOk || !networkBuiltIn()) {
    ImGui::BeginDisabled();
  }
  tNote(j, tr("Paste an invite code you were sent. Join then asks where the project goes on this computer."), P.textDim, 3);
  bool joinEnter = false;
  if (j.room(gT.rowH, "join code")) {
    joinEnter = tField(a, "##tjoincode", "gridline-invite:...", u.trk.joinBuf, sizeof u.trk.joinBuf, ImVec2(j.x0 + 4, j.y), j.x1 - j.x0 - 8,
                       ImGuiInputTextFlags_EnterReturnsTrue, true);
    ui::Hint("The invite code (Paste with Cmd+V or Ctrl+V)");
    j.next(gT.rowH);
  }
  if (j.room(gT.rowH, "join button")) {
    const float bw = tBtnW(a, "Join...");
    if (tButton(a, "##tjoin", "Join...", ImVec2(j.x1 - 4 - bw, j.y), ImVec2(j.x1 - 4, j.y + gT.rowH), false,
                "The Join window, with this code: it checks the code, then asks where the project goes") ||
        joinEnter) {
      joinWithCode(a, u.trk.joinBuf);
      u.trk.joinBuf[0] = 0;
    }
    j.next(gT.rowH);
  }
  if (!gateOk || !networkBuiltIn()) {
    ImGui::EndDisabled();
  }
  {
    TCol foot = j;
    foot.y = footY;
    const char* l = "Open collaborative project...";
    const float bw = std::min(tBtnW(a, l), j.x1 - j.x0 - 8);
    if (tButton(a, "##topenfolder", l, ImVec2(j.x1 - 4 - bw, foot.y), ImVec2(j.x1 - 4, foot.y + gT.rowH), false,
                "Choose the project's folder (the one with the .gridline-collab folder)")) {
      openCollabFolder(a);
    }
  }
}

// Projects: the collaborative projects on this computer (recentProjects), a list and its commands.
void tPageProjects(App& a, ImVec2 t0, ImVec2 t1, float pad) {
  using St = Controller::KnownProject::State;
  Ui& u = U();
  const auto& P = pal();
  const auto known = ctlOf(a).knownProjects();
  const float cmdW = std::round(std::clamp(std::max({tBtnW(a, "Keep a private copy..."), tBtnW(a, "Open collaborative project...")}) + 8, gT.fs * 10, gT.fs * 17));
  const float xa = t0.x + pad, xb = t1.x - pad - cmdW, y0 = t0.y + pad, y1 = t1.y - pad;
  char right[32];
  std::snprintf(right, sizeof right, "%d", static_cast<int>(known.size()));
  TCol col = tBox(a, xa, xb - pad, y0, y1, "Collaborative projects on this computer", right);
  const int sel = [&] {
    for (size_t i = 0; i < known.size(); ++i) {
      if (known[i].folder == u.trk.project) {
        return static_cast<int>(i);
      }
    }
    return known.empty() ? -1 : 0;
  }();
  if (sel >= 0) {
    u.trk.project = known[static_cast<size_t>(sel)].folder;
  }
  const TList L = tListBox(a, "##tprojsb", ImVec2(col.x0 + 4, col.y), ImVec2(col.x1 - 4, col.foot), static_cast<int>(known.size()), &u.trk.projectFirst);
  if (known.empty()) {
    gT.dl->AddText(ImGui::GetFont(), gT.fs, ImVec2(L.x0 + 4, L.y0 + 2), P.textFaint, tr("None on this computer yet. Projects you share or join appear here."));
  }
  const float wName = std::round((L.x1 - L.x0) * 0.34f), wState = std::round(gT.fs * 9);
  for (int r = 0; r < L.shown; ++r) {
    const int i = L.first + r;
    const auto& k = known[static_cast<size_t>(i)];
    const float y = L.y0 + static_cast<float>(r) * L.rowH;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(ImVec2(L.x0, y));
    const bool click = ImGui::InvisibleButton("##trow", ImVec2(std::max(1.0f, L.x1 - L.x0), L.rowH));
    const bool hov = ImGui::IsItemHovered(), dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImGui::PopID();
    if (i == sel) {
      gT.dl->AddRectFilled(ImVec2(L.x0, y), ImVec2(L.x1, y + L.rowH), theme::withAlpha(P.surface600, 0.95f));
    } else if (hov) {
      gT.dl->AddRectFilled(ImVec2(L.x0, y), ImVec2(L.x1, y + L.rowH), theme::withAlpha(P.surface700, 0.6f));
    }
    if (click) {
      u.trk.project = k.folder;
    }
    if (dbl && k.state == St::Active) {
      const std::string f = k.folder;
      a.guardUnsaved([&a, f] { a.openProject(f); });
    }
    const float ty = std::round(y + (L.rowH - gT.fs) * 0.5f);
    tText(k.state == St::Active ? tBold(a) : a.fontRegular, L.x0 + 4, ty, wName - 8, k.state == St::Active ? P.text : P.textDim, knownLabel(known, k));
    const char* stateT = k.state == St::Removed ? tr("Removed") : k.state == St::Ended ? tr("Not shared any more") : k.state == St::Missing ? tr("Missing") : tr("Shared");
    tText(a.fontRegular, L.x0 + wName, ty, wState - 6, k.state == St::Missing ? P.hot : k.state == St::Active ? tv::valueInk() : P.textFaint, stateT);
    std::string sub = k.folder;
    if (k.state == St::Removed) {
      sub = fmt2(tr("%s removed you, %s"), k.byName.empty() ? std::string(tr("The admin")) : k.byName, clockTime(k.at, false));
      if (!k.reason.empty()) {
        sub += "  \xE2\x80\x9C" + k.reason + "\xE2\x80\x9D";
      }
    } else if (k.state == St::Ended) {
      sub = fmt2(tr("%s stopped sharing it, %s"), k.byName.empty() ? std::string(tr("The admin")) : k.byName, clockTime(k.at, false));
    }
    if (k.state == St::Active || k.state == St::Missing) {
      tPath(a, L.x0 + wName + wState, y, L.x1 - 4 - (L.x0 + wName + wState), sub, P.textFaint);
    } else {
      tText(a.fontRegular, L.x0 + wName + wState, ty, L.x1 - 4 - (L.x0 + wName + wState), P.textFaint, sub);
    }
  }
  // the commands, for the project picked
  const float cx0 = xb, cx1 = t1.x - pad;
  tv::bevelBox(gT.dl, ImVec2(cx0, y0), ImVec2(cx1, y1));
  float y = y0 + 2;
  auto cmd = [&](const char* id, const char* label, const char* hint, bool enabled) {
    const bool c = tButton(a, id, label, ImVec2(cx0 + 2, y), ImVec2(cx1 - 2, y + gT.rowH), false, hint, enabled);
    y += gT.rowH + gT.line;
    return c;
  };
  const Controller::KnownProject* k = sel >= 0 ? &known[static_cast<size_t>(sel)] : nullptr;
  if (cmd("##tprojopen", "Open", "Opens it and reconnects to the others (they need it open too)", k && k->state == St::Active)) {
    const std::string f = k->folder;
    a.guardUnsaved([&a, f] { a.openProject(f); });
  }
  if (cmd("##tprojlocate", "Locate...", "Moved or renamed it? Choose the folder where it is now", k && k->state == St::Missing)) {
    const std::string id = k->id;
    chooseFolder(a, "Where is the project folder now?", fs::path(k->folder).parent_path().string(), [&a, id](const std::string& dir) {
      std::string err;
      if (!ctlOf(a).locateProject(id, dir, &err)) {
        a.notify(err, true);
      }
    });
  }
  if (cmd("##tprojcopy", "Keep a private copy...", "A normal project in a new folder: the music and its audio, nothing shared",
          k && (k->state == St::Removed || k->state == St::Ended))) {
    keepCopyOfFolder(a, k->folder, k->name);
  }
  if (cmd("##tprojforget", "Forget", "Take it off this list; the folder stays as a normal project", k && k->state != St::Active)) {
    forgetPrompt(a, *k);
  }
  const float by = std::round(y1 - 2 - gT.rowH);
  if (tButton(a, "##tprojfolder", "Open collaborative project...", ImVec2(cx0 + 2, by), ImVec2(cx1 - 2, by + gT.rowH), false,
              "Choose the project's folder (the one with the .gridline-collab folder)")) {
    openCollabFolder(a);
  }
}

// Session: the shared project (the panel's header and This project section).
void tPageSession(App& a, Session& s, Hub* hub, ImVec2 t0, ImVec2 t1, float pad) {
  Ui& u = U();
  const auto& P = pal();
  Controller& c = ctlOf(a);
  const float W = t1.x - t0.x - 3 * pad;
  const float xa = t0.x + pad, xb = std::round(xa + W * 0.5f), xc = xb + pad, y0 = t0.y + pad, y1 = t1.y - pad;
  TCol col = tBox(a, xa, xb, y0, y1, "Project");
  col.lab = tv::labelColumn(a, {"Name", "Admin", "Members", "State", "Review", "Last synced"}, tv::space::kGroup);
  const float footY = std::round(col.foot - gT.rowH);
  const bool renaming = u.renameAt == 3 && s.ready() && !s.stopped();
  if (u.renameAt == 3 && !renaming) {
    u.renameAt = 0;
  }
  if (col.room(gT.rowH, "name")) {
    tLabel(col, "Name");
    if (renaming) {
      const std::string name = cleanProjectName(u.projNameBuf);
      const fs::path folder = fs::path(s.config().folder).lexically_normal();
      const fs::path target = folder.parent_path() / projectFileStem(name.empty() ? std::string("Project") : name);
      const bool differs = target.filename() != folder.filename();
      const bool nothing = name == s.projectName() && !(u.alsoFolder && differs);
      const float okW = tBtnW(a, "Rename"), cw = tBtnW(a, "Cancel");
      const float fx1 = col.x1 - 4 - okW - cw - 2 * tv::space::kGap;
      const bool focus = u.nameFocus;
      u.nameFocus = false;
      const bool enter = tField(a, "##trename", tr("Project name"), u.projNameBuf, sizeof u.projNameBuf, ImVec2(col.fieldX(), col.y), fx1 - tv::space::kGap - col.fieldX(),
                                ImGuiInputTextFlags_EnterReturnsTrue, false, focus);
      const bool escape = ImGui::IsItemDeactivated() && ::gl::keys::escapePressed();
      const bool go = tButtonRight(a, col, "##trenameok", "Rename", "Renames the project for everyone", !name.empty() && !nothing, col.x1 - 4 - cw - tv::space::kGap) ||
                      (enter && !name.empty() && !nothing);
      const bool cancel = tButtonRight(a, col, "##trenamecancel", "Cancel") || escape;
      col.next(gT.rowH);
      tNameCaption(col, u.projNameBuf, name, tr("Everyone sees the new name: now if they are online, or the next time you are online together."), col.fieldX());
      if (col.room(gT.rowH, "rename folder")) {
        if (!differs) {
          ImGui::BeginDisabled();
        }
        if (tCheck(a, col, "##trenamefolder", "Also rename the project folder on this computer", u.alsoFolder && differs,
                   "Only your copy: collaborators' folders stay as they are. Gridline reopens the project and reconnects.", col.fieldX() - 2)) {
          u.alsoFolder = !u.alsoFolder;
        }
        if (!differs) {
          ImGui::EndDisabled();
        }
        col.next(gT.rowH);
      }
      if (!u.nameErr.empty()) {
        tNote(col, u.nameErr, theme::lit(P.danger), 2, col.fieldX());
      }
      if (cancel) {
        u.renameAt = 0;
      } else if (go) {
        commitRename(a, name);
        if (u.renameAt == 0) {
          u.renameAt = 0;
        }
      }
    } else {
      const float rw = tBtnW(a, "Rename...");
      tText(tBold(a), col.fieldX(), std::round(col.y + (gT.rowH - gT.fs) * 0.5f), col.x1 - 8 - rw - col.fieldX(), P.text, s.projectName());
      if (tButtonRight(a, col, "##trenameopen", "Rename...", "Renames the project for everyone in it", s.ready() && !s.stopped())) {
        openRename(a, 3);
      }
      col.next(gT.rowH);
    }
  }
  const auto row = [&](const char* label, const std::string& value, ImU32 colr, ImFont* f = nullptr) {
    if (!col.room(gT.rowH, label)) {
      return false;
    }
    tLabel(col, label);
    tText(f ? f : a.fontRegular, col.fieldX(), std::round(col.y + (gT.rowH - gT.fs) * 0.5f), col.x1 - 4 - col.fieldX(), colr, value);
    return true;
  };
  {
    const Member* adm = s.member(s.admin());
    const std::string an = s.isAdmin() ? std::string(tr("you")) : adm ? adm->name : std::string("-");
    if (row("Admin", an, P.text)) {
      ui::Hint(s.isAdmin() ? "You are the admin: you resolve conflicting changes." : "The admin resolves conflicting changes");
      col.next(gT.rowH);
    }
  }
  {
    int online = 1;
    for (const auto& [site, pv] : s.peers()) {
      online += pv.connected && s.isActive(site) ? 1 : 0;
    }
    char b[64];
    std::snprintf(b, sizeof b, tr("%d of %d online"), online, static_cast<int>(s.activeMembers().size()));
    if (row("Members", b, tv::valueInk(), tv::monoFont(a))) {
      col.next(gT.rowH);
    }
  }
  if (col.room(gT.rowH, "state")) {
    tLabel(col, "State");
    std::string t;
    ImU32 lc = P.led;
    bool pulse = false;
    if (c.leaving()) {
      t = std::string(tr("Telling your collaborators")) + dots(), lc = P.hot, pulse = true;
    } else if (!s.ready()) {
      const Hub::JoinView& jv = hub->joinView();
      t = jv.status == Hub::JoinStatus::NeedReply ? std::string(tr("Waiting for the inviter: send them the reply code (Join window).")) : std::string(tr("Connecting to the inviter..."));
      lc = P.hot, pulse = true;
    } else {
      t = tr("Shared: changes sync while you are online together");
    }
    tLamp(ImVec2(col.fieldX() + 5, col.y + gT.rowH * 0.5f), lc, pulse);
    tText(a.fontRegular, col.fieldX() + 14, std::round(col.y + (gT.rowH - gT.fs) * 0.5f), col.x1 - 4 - col.fieldX() - 14, P.text, t);
    ui::Hint("Peer to peer, no server: changes travel while two computers are online together.");
    col.next(gT.rowH);
  }
  if (col.room(gT.rowH, "review")) {
    tLabel(col, "Review");
    const size_t open = s.conflicts().openCount();
    char b[128];
    if (open == 0) {
      std::snprintf(b, sizeof b, "%s", tr("Nothing to review"));
    } else if (open == 1) {
      std::snprintf(b, sizeof b, "%s", s.isAdmin() ? tr("1 change needs your review") : tr("1 change awaiting the admin's review"));
    } else {
      std::snprintf(b, sizeof b, s.isAdmin() ? tr("%d changes need your review") : tr("%d changes awaiting the admin's review"), static_cast<int>(open));
    }
    const bool canReview = open > 0 && s.isAdmin();
    const float rw = canReview ? tBtnW(a, "Review...") + tv::space::kGap : 0.0f;
    tText(a.fontRegular, col.fieldX(), std::round(col.y + (gT.rowH - gT.fs) * 0.5f), col.x1 - 4 - rw - col.fieldX(), open ? P.hot : P.textDim, b);
    if (canReview && tButtonRight(a, col, "##treview", "Review...", "The Sync review: conflicting changes, side by side")) {
      a.showSyncReview = true;
    }
    col.next(gT.rowH);
  }
  {
    const uint64_t last = s.lastContactMs(), nowMs = wallMs();
    if (s.activeMembers().size() > 1 &&
        row("Last synced", last ? fmt1(tr("with a collaborator %s"), ago(last, nowMs)) : std::string(tr("not with anyone yet")), P.textDim)) {
      col.next(gT.rowH);
    }
  }
  // leaving (the panel's foot)
  {
    TCol foot = col;
    foot.y = footY;
    const bool admin = s.isAdmin(), others = s.activeMembers().size() > 1;
    if (c.leaving()) {
      // (the State row says so)
    } else if (admin) {
      if (tButtonRight(a, foot, "##tstop", "Stop sharing...", "For everyone: nothing syncs any more; each person keeps their copy")) {
        u.leaveDialog = 2;
        u.leaveErr.clear();
      }
      if (others) {
        tButtonRight(a, foot, "##tleave", "Leave project...", "You are the admin. Make someone else admin first (the Members page), then leave.", false,
                     col.x1 - 4 - tBtnW(a, "Stop sharing...") - tv::space::kGap);
      }
    } else if (tButtonRight(a, foot, "##tleave", "Leave project...", "You stop syncing; keep your copy as a normal project or delete it")) {
      u.leaveDialog = 1;
      u.leaveMode = 0;
      u.leaveWait = false;
      u.leaveErr.clear();
    }
  }
  // -- the folder
  TCol f = tBox(a, xc, t1.x - pad, y0, y1, "Folder");
  f.lab = tv::labelColumn(a, {"Folder", "Files"}, tv::space::kGroup);
  const std::string& folder = s.config().folder;
  if (f.room(gT.rowH, "folder")) {
    tLabel(f, "Folder");
    tPath(a, f.fieldX(), f.y, f.x1 - 4 - f.fieldX(), folder);
    f.next(gT.rowH);
  }
  if (f.room(gT.rowH, "files")) {
    tLabel(f, "Files");
    const Ui::Sizes& z = folderSizes(folder, u.now);
    const std::string t = fmt1(tr("%s in all"), formatBytes(z.total)) + "  \xC2\xB7  " + fmt1(tr("audio %s"), formatBytes(z.audio)) + "  \xC2\xB7  " +
                          fmt1(tr("shared history %s"), formatBytes(z.history));
    tText(a.fontRegular, f.fieldX(), std::round(f.y + (gT.rowH - gT.fs) * 0.5f), f.x1 - 4 - f.fieldX(), P.textDim, t);
    f.next(gT.rowH);
  }
  if (f.room(gT.rowH, "folder buttons")) {
    const float w1 = tBtnW(a, revealLabel()), w2 = tBtnW(a, "Clean up unused audio...");
    float x = f.fieldX() - 2;
    if (x + w1 + tv::space::kGap + w2 > f.x1 - 4) {
      x = f.x0 + 4;
    }
    if (tButton(a, "##treveal", revealLabel(), ImVec2(x, f.y), ImVec2(x + w1, f.y + gT.rowH), false, "The project's folder in the file manager")) {
      revealFolder(folder);
    }
    x += w1 + tv::space::kGap;
    if (tButton(a, "##tcleanup", "Clean up unused audio...", ImVec2(x, f.y), ImVec2(std::min(f.x1 - 4, x + w2), f.y + gT.rowH), false,
                "Audio files nothing in the project, its history, undo or Sync review uses")) {
      u.unused = s.unusedAudio();
      u.unusedBytes = 0;
      for (const auto& fl : u.unused) {
        u.unusedBytes += fl.second;
      }
      u.cleanupOpen = true;
    }
    f.next(gT.rowH);
  }
  if (f.room(gT.rowH, "cursors")) {
    const bool cursors = a.settings.getBool("collab.showCursors");
    if (tCheck(a, f, "##tcursors", "Show collaborators' cursors", cursors, "Their pointers and edit cursors over your windows")) {
      a.settings.set("collab.showCursors", !cursors);
    }
    f.next(gT.rowH);
  }
  tNote(f, tr("Peer to peer, no server: changes travel while two computers are online together."), P.textFaint, 2);
  // J-111e: an abandoned project (nobody came back for a month): nothing is deleted by itself
  const uint64_t last = s.lastContactMs(), nowMs = wallMs();
  const int idleDays = last && nowMs > last ? static_cast<int>((nowMs - last) / 86400000ull) : 0;
  if (idleDays >= 30 || std::getenv("GRIDLINE_DEV_ABANDONED")) {
    char b[160];
    std::snprintf(b, sizeof b, tr("Nobody has synced with you for %d days."), std::max(idleDays, 30));
    TCol m = f;
    m.foot = footY - gT.line;
    tNote(m, std::string(b) + " " + tr("Nothing is deleted by itself. Keep it shared, or make it a normal project on this computer."), P.hot, 3);
    TCol foot = f;
    foot.y = footY;
    if (tButtonRight(a, foot, "##tabandon", "Make it a normal project...")) {
      u.leaveDialog = 3;
      u.leaveErr.clear();
    }
  }
}

// Members: everyone (drawMember, drawGoneMember) as list rows; the actions (memberMenu) in a column.
void tPageMembers(App& a, Session& s, Hub* hub, ImVec2 t0, ImVec2 t1, float pad) {
  Ui& u = U();
  const auto& P = pal();
  const std::vector<Member> all = s.members();  // (a copy: the rows point into it)
  std::vector<const Member*> rows;
  int goneN = 0, removedN = 0;
  for (const Member& m : all) {
    if (s.isActive(m.site)) {
      rows.push_back(&m);
    }
  }
  for (const Member& m : all) {
    if (!s.isActive(m.site)) {
      ++goneN;
      removedN += s.memberState(m.site) == MemberState::Removed ? 1 : 0;
      if (u.showGone) {
        rows.push_back(&m);
      }
    }
  }
  const Member* sel = nullptr;
  for (const Member* m : rows) {
    if (m->site == u.trk.member) {
      sel = m;
    }
  }
  if (!sel) {  // the first other member (or this user)
    for (const Member* m : rows) {
      if (!sel || (sel->site == s.site() && m->site != s.site() && s.isActive(m->site))) {
        sel = m;
      }
    }
  }
  u.trk.member = sel ? sel->site : 0;
  const float cmdW = std::round(std::clamp(std::max({tBtnW(a, "Go to what they are on"), tBtnW(a, "Follow their playhead"), tBtnW(a, "Remove from project...")}) + 8,
                                           gT.fs * 10, gT.fs * 16));
  const float xa = t0.x + pad, xb = t1.x - pad - cmdW, y0 = t0.y + pad, y1 = t1.y - pad;
  int online = 1;
  for (const auto& [site, pv] : s.peers()) {
    online += pv.connected && s.isActive(site) ? 1 : 0;
  }
  char right[64];
  std::snprintf(right, sizeof right, tr("%d of %d online"), online, static_cast<int>(s.activeMembers().size()));
  TCol col = tBox(a, xa, xb - pad, y0, y1, "Members", right);
  // a collaborator this computer could not reach directly: what still works (under the list)
  std::string failed;
  for (const Member& m : s.members()) {
    if (m.site != s.site() && hub && s.isActive(m.site) && hub->link(m.site).state == Hub::LinkView::State::Failed) {
      failed = fmt1(tr("Couldn't connect directly to %s"), m.name);
      break;
    }
  }
  const float listFoot = failed.empty() ? col.foot : col.foot - std::round(gT.fs + 6);
  const TList L = tListBox(a, "##tmemsb", ImVec2(col.x0 + 4, col.y), ImVec2(col.x1 - 4, listFoot), static_cast<int>(rows.size()), &u.trk.memberFirst);
  if (!failed.empty()) {
    TCol n = col;
    n.y = listFoot + 3;
    tLamp(ImVec2(n.x0 + 10, n.y + (gT.fs + 3) * 0.5f), P.danger);
    tText(a.fontRegular, n.x0 + 19, n.y + 1, n.x1 - 25 - n.x0, theme::lit(P.danger),
          failed + ". " + tr("A collaborator online with you both relays changes. A TURN server (Connection page) or a reconnect code can also help."));
  }
  const float W = L.x1 - L.x0;
  const float wName = std::round(std::clamp(W * 0.36f, gT.fs * 9, gT.fs * 18)), wRole = std::round(gT.fs * 6.5f);
  for (int r = 0; r < L.shown; ++r) {
    const Member& m = *rows[static_cast<size_t>(L.first + r)];
    const bool me = m.site == s.site(), gone = !s.isActive(m.site);
    const PeerView* pv = me ? nullptr : s.peer(m.site);
    Profile prof = me ? s.config().profile : (pv ? pv->profile : Profile());
    if (prof.name.empty()) {
      prof.name = m.name;
    }
    const Hub::LinkView lv = hub && !me ? hub->link(m.site) : Hub::LinkView();
    const MemberStatus st = memberStatus(s, m, pv, lv);
    const float y = L.y0 + static_cast<float>(r) * L.rowH;
    ImGui::PushID(static_cast<int>(m.site & 0x7fffffff));
    ImGui::SetCursorScreenPos(ImVec2(L.x0, y));
    const bool click = ImGui::InvisibleButton("##tmrow", ImVec2(std::max(1.0f, W), L.rowH));
    const bool hov = ImGui::IsItemHovered();
    if (click || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      u.trk.member = m.site;
    }
    if (hov) {
      std::string tip = std::string(tr("Fingerprint")) + ": " + m.fingerprint();
      if (pv && !pv->profile.email.empty()) {
        tip += "\n" + pv->profile.email;
      }
      tip += "\n" + st.text;
      if (!st.more.empty()) {
        tip += "\n" + st.more;
      }
      if (pv && pv->clockApprox) {
        tip += std::string("\n") + tr("Their clock differs from this computer's: times are approximate.");
      }
      ui::Tooltip(tip.c_str());
    }
    if (!gone && ImGui::BeginPopupContextItem("##tmmenu")) {  // the panel's member menu
      const bool canInvite = !me && !st.online && lv.state != Hub::LinkView::State::Connecting && hub && s.ready();
      memberMenu(s, hub, m, me, canInvite);
      ImGui::EndPopup();
    }
    ImGui::PopID();
    if (m.site == u.trk.member) {
      gT.dl->AddRectFilled(ImVec2(L.x0, y), ImVec2(L.x1, y + L.rowH), theme::withAlpha(P.surface600, 0.95f));
    } else if (hov) {
      gT.dl->AddRectFilled(ImVec2(L.x0, y), ImVec2(L.x1, y + L.rowH), theme::withAlpha(P.surface700, 0.6f));
    }
    const float ty = std::round(y + (L.rowH - gT.fs) * 0.5f), cy = y + L.rowH * 0.5f;
    // the lamp (their link), their colour, the name
    tLamp(ImVec2(L.x0 + 9, cy), gone ? P.surface950 : st.dot, st.pulse);
    const float sw = std::round(gT.fs * 0.75f);
    const ImVec2 c0(std::round(L.x0 + 18), std::round(cy - sw * 0.5f));
    tv::bevelBox(gT.dl, ImVec2(c0.x - 1, c0.y - 1), ImVec2(c0.x + sw + 1, c0.y + sw + 1), true);
    gT.dl->AddRectFilled(c0, ImVec2(c0.x + sw, c0.y + sw), gone ? P.surface700 : colorOf(prof.color ? prof.color : defaultColor(m.signPublic)));
    const std::string name = prof.name.empty() ? std::string(tr("(no name)")) : prof.name;
    const float nx = c0.x + sw + 6;
    tText(gone ? a.fontRegular : tBold(a), nx, ty, L.x0 + wName - nx - 4, gone ? P.textDim : P.text, name);
    // the role
    std::string role = m.site == s.admin() ? std::string(tr("admin")) : gone ? std::string() : std::string(tr("member"));
    if (me) {
      role += role.empty() ? tr("you") : std::string(", ") + tr("you");
    }
    if (gone) {
      role = s.memberState(m.site) == MemberState::Removed ? tr("removed") : tr("left");
    }
    tText(a.fontRegular, L.x0 + wName, ty, wRole - 6, m.site == s.admin() ? tv::valueInk() : P.textDim, role);
    // the link in words (or why they are gone)
    std::string what = st.text;
    if (gone) {
      const Removal* rm = s.openRemoval(m.site);
      what = rm ? fmt2(tr("Removed by %s, %s"), rm->byName.empty() ? std::string(tr("the admin")) : rm->byName, clockTime(rm->at, false)) : std::string(tr("Left the project"));
      if (rm && !rm->reason.empty()) {
        what += "  \xE2\x80\x9C" + rm->reason + "\xE2\x80\x9D";
      }
    }
    tText(a.fontRegular, L.x0 + wName + wRole, ty, L.x1 - 4 - (L.x0 + wName + wRole), gone ? P.textFaint : st.failed ? theme::lit(P.danger) : P.textDim, what);
  }
  // the commands, for the member picked (the panel's row buttons and its member menu)
  const float cx0 = xb, cx1 = t1.x - pad;
  tv::bevelBox(gT.dl, ImVec2(cx0, y0), ImVec2(cx1, y1));
  float y = y0 + 2;
  auto cmd = [&](const char* id, const char* label, const char* hint, bool enabled, bool on = false) {
    const bool c = tButton(a, id, label, ImVec2(cx0 + 2, y), ImVec2(cx1 - 2, y + gT.rowH), on, hint, enabled);
    y += gT.rowH + gT.line;
    return c;
  };
  const bool me = sel && sel->site == s.site();
  const bool gone = sel && !s.isActive(sel->site);
  const bool live = sel && !me && !gone;
  const Hub::LinkView lv = hub && live ? hub->link(sel->site) : Hub::LinkView();
  const bool offline = live && lv.state != Hub::LinkView::State::Direct && lv.state != Hub::LinkView::State::Relayed;
  if (cmd("##tfollow", "Follow their playhead", "Your playhead follows theirs while they play", live, live && u.follow == sel->site)) {
    u.follow = u.follow == sel->site ? 0 : sel->site;
  }
  if (cmd("##tgoto", "Go to what they are on", "Selects the channel, mixer track, pattern or clip under their pointer and shows it in your layout", live)) {
    u.gotoPeer = sel->site;
  }
  if (cmd("##treconnect", "Reconnect...", "Exchange reconnect codes with them (chat, email): the Invite page shows the codes", live)) {
    u.reconnectPeer = sel->site;
    u.trk.page = kTInvite;
  }
  {
    const bool removed = gone && s.memberState(sel->site) == MemberState::Removed;
    const bool canInvite = hub && s.ready() && ((live && offline && lv.state != Hub::LinkView::State::Connecting) || (gone && (!removed || s.isAdmin())));
    if (cmd("##tinviteagain", removed ? "Re-invite..." : "Invite again",
            removed ? "A one-time invite that lets them back in (only an admin's invite does)" : "A new invite code: they join again and stay the same member", canInvite)) {
      if (gone) {
        u.inviteId = hub->createInvite(u.inviteMinutes, removed ? sel->site : 0);
        u.reinviteFor = sel->name.empty() ? std::string(tr("(no name)")) : sel->name;
        u.scrollToInvite = true;
      } else {
        inviteAgain(*hub);
      }
      u.trk.page = kTInvite;
    }
  }
  if (cmd("##tmakeadmin", "Make admin", "They become the admin: they resolve conflicting changes (you can then leave)", live && s.isAdmin())) {
    s.transferAdmin(sel->site);
  }
  if (cmd("##tfingerprint", "Copy fingerprint", "Copy their fingerprint, to compare by phone or chat", sel != nullptr)) {
    ImGui::SetClipboardText(sel->fingerprint().c_str());
  }
  if (cmd("##tremove", "Remove from project...", "They stop receiving the project; their earlier changes stay", live && s.isAdmin() && s.ready())) {
    u.removeTarget = sel->site;
    u.removeReason[0] = 0;
    u.removeErr.clear();
  }
  if (goneN > 0) {
    char b[96];
    if (removedN == goneN) {
      std::snprintf(b, sizeof b, tr("Removed (%d)"), removedN);
    } else if (removedN == 0) {
      std::snprintf(b, sizeof b, tr("Left (%d)"), goneN);
    } else {
      std::snprintf(b, sizeof b, tr("Removed or left (%d)"), goneN);
    }
    const float by = std::round(y1 - 2 - gT.rowH);
    if (tv::checkRow(a, "##tshowgone", b, ImVec2(cx0 + 2, by), ImVec2(cx1 - 2, by + gT.rowH), u.showGone,
                     "People who are no longer in the project. Their earlier changes stay.")) {
      u.showGone = !u.showGone;
    }
  }
}

// Invite: a code for one person (drawInvite), and reconnect codes (drawReconnect).
void tPageInvite(App& a, Session& s, Hub* hub, ImVec2 t0, ImVec2 t1, float pad) {
  Ui& u = U();
  const auto& P = pal();
  const float W = t1.x - t0.x - 3 * pad;
  const float xa = t0.x + pad, xb = std::round(xa + W * 0.6f), xc = xb + pad, y0 = t0.y + pad, y1 = t1.y - pad;
  TCol col = tBox(a, xa, xb, y0, y1, "Invite someone");
  col.lab = tv::labelColumn(a, {"Valid for", "Reply code"}, tv::space::kGroup);
  const float footY = std::round(col.foot - gT.rowH);
  const Hub::InviteView* iv = hub && !u.inviteId.empty() ? hub->invite(u.inviteId) : nullptr;
  if (!hub || !s.ready()) {
    tNote(col, tr("Invites can be made once the project is ready here."), P.textDim, 2);
  } else if (!iv || iv->status == Hub::InviteStatus::Expired || iv->status == Hub::InviteStatus::Failed) {
    if (iv && iv->status == Hub::InviteStatus::Expired) {
      tNote(col, tr("The last invite expired. Make a new one."), P.textDim, 1);
    }
    if (iv && iv->status == Hub::InviteStatus::Failed) {
      tNote(col, iv->error, theme::lit(P.danger), 2);
    }
    static const int kMinutes[] = {15, 60, 24 * 60};
    static const char* const kLabels[] = {i18n::N_("15 minutes"), i18n::N_("1 hour"), i18n::N_("24 hours")};
    if (col.room(gT.rowH, "valid for")) {
      tLabel(col, "Valid for");
      const float rw = std::floor((col.x1 - 4 - col.fieldX()) / 3);
      for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        const float rx = col.fieldX() - 4 + static_cast<float>(i) * rw;
        if (tv::radioRow(a, "##tvalid", kLabels[i], ImVec2(rx, col.y), ImVec2(rx + rw, col.y + gT.rowH), u.inviteMinutes == kMinutes[i],
                         "How long the code works")) {
          u.inviteMinutes = kMinutes[i];
          a.settings.set("collab.inviteMinutes", u.inviteMinutes);
        }
        ImGui::PopID();
      }
      col.next(gT.rowH);
    }
    tNote(col, tr("A code works once, for one person."), P.textFaint, 1, col.fieldX());
    TCol foot = col;
    foot.y = footY;
    if (tButtonRight(a, foot, "##tinvitecreate", "Create invite", "Makes a one-time code for one person")) {
      u.inviteId = hub->createInvite(u.inviteMinutes);
      u.reinviteFor.clear();
      a.settings.set("collab.inviteMinutes", u.inviteMinutes);
    }
  } else if (iv->status == Hub::InviteStatus::Preparing) {
    if (col.room(gT.rowH, "preparing")) {
      tLamp(ImVec2(col.x0 + 10, col.y + gT.rowH * 0.5f), P.hot, true);
      tText(a.fontRegular, col.x0 + 20, std::round(col.y + (gT.rowH - gT.fs) * 0.5f), col.x1 - 24 - col.x0, P.text, std::string(tr("Preparing the invite")) + dots());
      col.next(gT.rowH);
    }
    tNote(col, tr("Finding this computer's network addresses."), P.textFaint, 1);
  } else if (iv->status == Hub::InviteStatus::Connected) {
    if (col.room(gT.rowH, "joined")) {
      tLamp(ImVec2(col.x0 + 10, col.y + gT.rowH * 0.5f), P.led);
      tText(tBold(a), col.x0 + 20, std::round(col.y + (gT.rowH - gT.fs) * 0.5f), col.x1 - 24 - col.x0, P.text,
            fmt1(tr("%s joined"), iv->joinerName.empty() ? std::string(tr("Someone")) : iv->joinerName));
      col.next(gT.rowH);
    }
    if (!iv->safety.empty()) {
      // the safety code: four groups of digits in sunken boxes, large, to read out and compare
      ImFont* big = a.fontMonoBold ? a.fontMonoBold : tv::monoFont(a);
      const float bfs = std::round(gT.fs * 1.4f), bw = std::round(big->CalcTextSizeA(bfs, FLT_MAX, 0, "0000").x + 12), bh = std::round(bfs + 8);
      if (col.room(gT.rowH + bh, "safety code")) {
        tLabel(col, "Safety code");
        col.next(gT.rowH);
        float x = col.x0 + 6;
        size_t i = 0;
        const std::string& code = iv->safety;
        while (i < code.size()) {
          size_t j = code.find(' ', i);
          const std::string grp = code.substr(i, j == std::string::npos ? std::string::npos : j - i);
          if (!grp.empty() && x + bw <= col.x1 - 4) {
            tv::bevelBox(gT.dl, ImVec2(x, col.y), ImVec2(x + bw, col.y + bh), true);
            const float gw = big->CalcTextSizeA(bfs, FLT_MAX, 0, grp.c_str()).x;
            gT.dl->AddText(big, bfs, ImVec2(std::round(x + (bw - gw) * 0.5f), std::round(col.y + (bh - bfs) * 0.5f)), tv::valueInk(), grp.c_str());
            x += bw + tv::space::kGroup;
          }
          if (j == std::string::npos) {
            break;
          }
          i = j + 1;
        }
        col.next(bh);
      }
      tNote(col, tr("Compare it with them by phone or chat. If it matches, nobody is in between."), P.textFaint, 2);
    }
    TCol foot = col;
    foot.y = footY;
    if (tButtonRight(a, foot, "##tinviteother", "Invite someone else")) {
      u.inviteId.clear();
      u.reinviteFor.clear();
    }
  } else {
    // a code is ready (or they are connecting with it)
    const bool connecting = iv->status == Hub::InviteStatus::Connecting;
    const int64_t left = iv->expires - static_cast<int64_t>(std::time(nullptr));
    if (col.room(gT.rowH, "invite state")) {
      const std::string what = connecting      ? std::string(tr("Someone is connecting..."))
                               : iv->replyUsed ? std::string(tr("Reply code used: reaching them")) + dots()
                                               : std::string(tr("Waiting for them to join"));
      const std::string exp = fmt1(tr("expires in %s"), countdown(left));
      const float ew = tTextW(a.fontRegular, exp.c_str());
      tLamp(ImVec2(col.x0 + 10, col.y + gT.rowH * 0.5f), connecting ? P.led : P.accent, true);
      tText(a.fontRegular, col.x0 + 20, std::round(col.y + (gT.rowH - gT.fs) * 0.5f), col.x1 - 30 - ew - col.x0, P.text, what);
      gT.dl->AddText(a.fontRegular, gT.fs, ImVec2(std::round(col.x1 - 6 - ew), std::round(col.y + (gT.rowH - gT.fs) * 0.5f)), left < 120 ? theme::lit(P.hot) : P.textFaint,
                     exp.c_str());
      col.next(gT.rowH);
    }
    // the code: mono, wrapped, in a sunken box (Copy and Save as file... under it)
    {
      ImFont* mono = tv::monoFont(a);
      const float lh = std::round(gT.fs + 2), bx0 = col.x0 + 4, bx1 = col.x1 - 4;
      const int lines = 3;
      const float bh = lh * lines + 6;
      if (col.room(bh, "invite code")) {
        tv::bevelBox(gT.dl, ImVec2(bx0, col.y), ImVec2(bx1, col.y + bh), true);
        const std::string& code = iv->code;
        const float cw = std::max(1.0f, tTextW(mono, "0"));
        const size_t per = static_cast<size_t>(std::max(8.0f, std::floor((bx1 - bx0 - 10) / cw)));
        for (int i = 0; i < lines && static_cast<size_t>(i) * per < code.size(); ++i) {
          std::string t = code.substr(static_cast<size_t>(i) * per, per);
          if (i == lines - 1 && static_cast<size_t>(i + 1) * per < code.size()) {
            t = t.substr(0, t.size() > 3 ? t.size() - 3 : 0) + "...";
          }
          gT.dl->AddText(mono, gT.fs, ImVec2(bx0 + 5, std::round(col.y + 3 + static_cast<float>(i) * lh)), tv::valueInk(), t.c_str());
        }
        if (ImGui::IsMouseHoveringRect(ImVec2(bx0, col.y), ImVec2(bx1, col.y + bh))) {
          ui::Tooltip(tr("Send this code to the person you are inviting. It works once."));
        }
        col.next(bh);
      }
    }
    if (col.room(gT.rowH, "invite buttons")) {
      float x = col.x0 + 4;
      const float w1 = tBtnW(a, "Copy"), w2 = tBtnW(a, "Save as file...");
      if (tButton(a, "##tinvitecopy", "Copy", ImVec2(x, col.y), ImVec2(x + w1, col.y + gT.rowH), false, "Copy the invite code")) {
        ImGui::SetClipboardText(iv->code.c_str());
        a.notify(tr("Invite code copied"));
      }
      x += w1 + tv::space::kGap;
      if (tButton(a, "##tinvitesave", "Save as file...", ImVec2(x, col.y), ImVec2(x + w2, col.y + gT.rowH), false, "The code in a text file, to send")) {
        saveCodeFile(a, iv->code, "Gridline invite.txt");
      }
      if (tButtonRight(a, col, "##tinvitecancel", "Cancel invite", "The code stops working")) {
        hub->cancelInvite(u.inviteId);
        u.inviteId.clear();
        u.reinviteFor.clear();
      }
      col.next(gT.rowH);
    }
    // why it may not reach them (K-174, K-312), whom it is for
    std::string notes;
    ImU32 nc = P.textFaint;
    if (!u.reinviteFor.empty()) {
      notes = fmt1(tr("For %s only: they come back as the same member."), u.reinviteFor);
      nc = P.textDim;
    }
    auto add = [&](const std::string& t, ImU32 c) {
      notes += (notes.empty() ? "" : " ") + t;
      nc = c;
    };
    if (iv->replyUsed && !connecting) {
      add(fmt2(tr("Trying for %s, attempt %s. Their Join window keeps trying too, so leave both open."), countdown(static_cast<int64_t>(iv->replyWaited)),
               std::to_string(std::max(1, iv->attempts))),
          P.textDim);
      if (!iv->peerPublicAddress) {
        add(tr("Their reply code has no public internet address: their computer couldn't reach the address servers, so it can only connect on the same network. On their computer, check the connection, a VPN or firewall, and run Test my connection in Settings > Collaboration."),
            theme::lit(P.danger));
      } else if (iv->replyWaited > 90 && !iv->natBlocked) {
        add(tr("Still not connected: a router or firewall on one side probably blocks direct connections. Turn off VPNs, allow Gridline through the firewall, and run Test my connection in Settings > Collaboration on both computers."),
            theme::lit(P.hot));
      }
    }
    if (!iv->natNote.empty()) {
      add(iv->natNote, theme::lit(iv->natBlocked ? P.danger : P.hot));
    }
    if (iv->codeChanged) {
      add(tr("This computer's network address changed, so the code was renewed. If they have not joined yet, send them this new code."), theme::lit(P.hot));
    }
    if (!iv->publicAddress) {
      add(tr("This computer couldn't find its public internet address, so this code only works on the same network. Check the connection, a VPN or firewall, or the STUN server in Settings > Collaboration."),
          theme::lit(P.danger));
    }
    if (notes.empty()) {
      notes = tr("Send this code to the person you are inviting. It works once.");
    }
    TCol nt = col;
    nt.foot = footY - gT.line;
    tNote(nt, notes, nc, 3);
    // the reply code (only when they can't connect directly)
    TCol foot = col;
    foot.y = footY;
    tLabel(foot, "Reply code");
    const float uw = tBtnW(a, "Use reply");
    const bool enter = tField(a, "##treply", "gridline-reply:...", u.replyBuf, sizeof u.replyBuf, ImVec2(col.fieldX(), foot.y),
                              col.x1 - 4 - uw - tv::space::kGap - col.fieldX(), ImGuiInputTextFlags_EnterReturnsTrue, true);
    ui::Hint("Only if they can't connect directly: they then get a reply code to send you. It still works if it takes a few minutes to arrive.");
    if (tButtonRight(a, foot, "##tusereply", "Use reply", "Connects to them with their reply code", u.replyBuf[0] != 0) || (enter && u.replyBuf[0])) {
      std::string err;
      if (hub->acceptReply(u.replyBuf, &err)) {
        u.replyBuf[0] = 0;
        a.notify(tr("Reply code used: connecting to them now."));
      } else {
        a.notify(err, true);
      }
    }
  }
  // -- reconnect
  const Member* m = u.reconnectPeer ? s.member(u.reconnectPeer) : nullptr;
  if (u.reconnectPeer && !m) {
    u.reconnectPeer = 0;
  }
  TCol r = tBox(a, xc, t1.x - pad, y0, y1, "Reconnect", m ? m->name : std::string());
  if (!m || !hub) {
    tNote(r, tr("To reconnect with someone offline, pick them on the Members page and press Reconnect. You then swap codes by chat or email."), P.textFaint, 4);
    return;
  }
  tNote(r, fmt1(tr("Send %s your code and paste theirs. Codes only carry network addresses; your keys check the connection."), m->name), P.textDim, 3);
  const std::string code = hub->reconnectCode(u.reconnectPeer);
  if (r.room(gT.rowH, "your code")) {
    tLabel(r, "Your code");
    const float lw = tv::labelColumn(a, {"Your code", "Their code"}, tv::space::kGroup);
    const float cw = tBtnW(a, "Copy");
    if (code.empty()) {
      tText(a.fontRegular, r.x0 + 6 + lw, std::round(r.y + (gT.rowH - gT.fs) * 0.5f), r.x1 - 10 - lw - r.x0, P.textFaint, std::string(tr("Preparing")) + dots());
    } else {
      tPath(a, r.x0 + 6 + lw, r.y, r.x1 - 4 - cw - tv::space::kGap - (r.x0 + 6 + lw), code);
    }
    if (tButtonRight(a, r, "##trccopy", "Copy", "Copy your reconnect code", !code.empty())) {
      ImGui::SetClipboardText(code.c_str());
    }
    r.next(gT.rowH);
  }
  if (r.room(gT.rowH, "their code")) {
    tLabel(r, "Their code");
    const float lw = tv::labelColumn(a, {"Your code", "Their code"}, tv::space::kGroup);
    const float cw = tBtnW(a, "Connect");
    const bool enter = tField(a, "##trcin", "gridline-reconnect:...", u.reconnectBuf, sizeof u.reconnectBuf, ImVec2(r.x0 + 6 + lw, r.y),
                              r.x1 - 4 - cw - tv::space::kGap - (r.x0 + 6 + lw), ImGuiInputTextFlags_EnterReturnsTrue, true);
    if (tButtonRight(a, r, "##trcconnect", "Connect", "Connects with their code", u.reconnectBuf[0] != 0) || (enter && u.reconnectBuf[0])) {
      std::string err;
      if (hub->acceptReconnectCode(u.reconnectBuf, &err)) {
        u.reconnectBuf[0] = 0;
      } else {
        a.notify(err, true);
      }
    }
    r.next(gT.rowH);
  }
  TCol foot = r;
  foot.y = footY;
  if (tButtonRight(a, foot, "##trcclose", "Close", "Done with reconnecting")) {
    u.reconnectPeer = 0;
  }
}

// Profile: settingsPage's profile, identity and presence.
void tPageProfile(App& a, ImVec2 t0, ImVec2 t1, float pad) {
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
  const float W = t1.x - t0.x - 3 * pad;
  const float xa = t0.x + pad, xb = std::round(xa + W * 0.58f), xc = xb + pad, y0 = t0.y + pad, y1 = t1.y - pad;
  TCol col = tBox(a, xa, xb, y0, y1, "Your profile");
  col.lab = tv::labelColumn(a, {"Display name", "Contact email", "Colour", "Picture"}, tv::space::kGroup);
  if (col.room(gT.rowH, "display name")) {
    tLabel(col, "Display name");
    if (tField(a, "##tcname", tr("Required to host or join"), u.nameBuf, sizeof u.nameBuf, ImVec2(col.fieldX(), col.y), col.x1 - 4 - col.fieldX())) {
      prof.name = u.nameBuf;
      changed = true;
    }
    ui::Hint("How collaborators see you. Kept on this computer, not in any project.");
    col.next(gT.rowH);
  }
  if (cleanName(u.nameBuf).empty()) {
    tNote(col, tr("A name is needed before you can host or join."), P.hot, 1, col.fieldX());
  }
  if (col.room(gT.rowH, "email")) {
    tLabel(col, "Contact email");
    if (tField(a, "##tcemail", tr("Optional"), u.emailBuf, sizeof u.emailBuf, ImVec2(col.fieldX(), col.y), col.x1 - 4 - col.fieldX())) {
      if (validEmail(u.emailBuf)) {
        prof.email = u.emailBuf;
        changed = true;
      }
    }
    col.next(gT.rowH);
  }
  if (!validEmail(u.emailBuf)) {
    tNote(col, tr("That is not an email address."), theme::lit(P.danger), 1, col.fieldX());
  }
  if (col.room(gT.rowH, "share email")) {
    if (tCheck(a, col, "##tshareemail", "Share my email with collaborators", prof.shareEmail,
               "Off: nobody sees it. On: sent only to collaborators, encrypted; never written into project files or logs", col.fieldX() - 2)) {
      prof.shareEmail = !prof.shareEmail;
      changed = true;
    }
    col.next(gT.rowH);
  }
  if (col.room(gT.rowH, "colour")) {
    tLabel(col, "Colour");
    // the colours offered (the content palette) as small bevel chips; the current one sunk and outlined
    int n = 0;
    const uint32_t* cols = ui::contentPalette(&n);
    const uint32_t cur = ps.color();
    const float ch = gT.rowH - 2;
    const float dw = prof.color ? tBtnW(a, "Default") + tv::space::kGap : 0.0f, ow = tBtnW(a, "Other...") + tv::space::kGap;
    float x = col.fieldX();
    const int fit = std::max(0, std::min(n, static_cast<int>(std::floor((col.x1 - 4 - dw - ow - x + 1) / (ch + 1)))));
    for (int i = 0; i < fit; ++i) {
      const ImVec2 q0(x, col.y + 1), q1(x + ch, col.y + 1 + ch);
      ImGui::PushID(i);
      ImGui::SetCursorScreenPos(q0);
      if (ImGui::InvisibleButton("##tchip", ImVec2(ch, ch))) {
        prof.color = cols[i];
        changed = true;
      }
      const bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      const bool on = cols[i] == cur;
      tv::bevelBox(gT.dl, q0, q1, on);
      gT.dl->AddRectFilled(ImVec2(q0.x + 2, q0.y + 2), ImVec2(q1.x - 2, q1.y - 2), theme::rgb(cols[i]));
      if (on || hov) {
        gT.dl->AddRect(ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), on ? P.text : theme::withAlpha(P.hot, 0.7f));
      }
      x += ch + 1;
    }
    x += tv::space::kGroup - 1;
    if (tButton(a, "##tcolother", "Other...", ImVec2(x, col.y), ImVec2(x + ow - tv::space::kGap, col.y + gT.rowH), false, "Any colour (a colour picker)")) {
      ImGui::OpenPopup("##tcolpick");
    }
    if (ImGui::BeginPopup("##tcolpick")) {
      float f[3] = {((cur >> 16) & 0xFF) / 255.f, ((cur >> 8) & 0xFF) / 255.f, (cur & 0xFF) / 255.f};
      if (ImGui::ColorPicker3("##tcolpicker", f, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs)) {
        prof.color = (static_cast<uint32_t>(f[0] * 255 + 0.5f) << 16) | (static_cast<uint32_t>(f[1] * 255 + 0.5f) << 8) | static_cast<uint32_t>(f[2] * 255 + 0.5f);
        changed = true;
      }
      ImGui::EndPopup();
    }
    if (prof.color && tButtonRight(a, col, "##tcoldefault", "Default", "The colour your identity gives you")) {
      prof.color = 0;
      changed = true;
    }
    ui::Hint("The colour of your cursor on your collaborators' screens");
    col.next(gT.rowH);
  }
  {
    const float ph = std::round(gT.rowH * 2 + gT.line);
    if (col.room(ph, "picture")) {
      tLabel(col, "Picture");
      const float x = col.fieldX();
      tv::bevelBox(gT.dl, ImVec2(x, col.y), ImVec2(x + ph, col.y + ph), true);
      ImGui::SetCursorScreenPos(ImVec2(x, col.y));
      ImGui::InvisibleButton("##tavatar", ImVec2(ph, ph));
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
      drawAvatar(a, gT.dl, ImVec2(x + ph * 0.5f, col.y + ph * 0.5f), ph * 0.5f - 3, cleanName(u.nameBuf), ps.color(), prof.avatar, -1);
      float bx = x + ph + tv::space::kGroup;
      const float w1 = tBtnW(a, "Choose picture...");
      if (tButton(a, "##tpicture", "Choose picture...", ImVec2(bx, col.y), ImVec2(bx + w1, col.y + gT.rowH), false,
                  "A PNG or JPEG; it is cropped to a square and made small (256 px). You can also drop a picture here")) {
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
        const float bx2 = bx + w1 + tv::space::kGap, w2 = tBtnW(a, "Remove picture");
        if (tButton(a, "##tpictureremove", "Remove picture", ImVec2(bx2, col.y), ImVec2(std::min(col.x1 - 4, bx2 + w2), col.y + gT.rowH))) {
          prof.avatar.clear();
          changed = true;
        }
      }
      const std::string t = !u.avatarErr.empty() ? u.avatarErr : std::string(tr("PNG or JPEG, or drop one on the picture"));
      tText(a.fontRegular, bx, std::round(col.y + gT.rowH + gT.line + (gT.rowH - gT.fs) * 0.5f), col.x1 - 4 - bx, u.avatarErr.empty() ? P.textFaint : theme::lit(P.danger), t);
      col.next(ph);
    }
  }
  if (changed) {
    c.setProfile(prof);
  }
  // -- identity and presence
  TCol id = tBox(a, xc, t1.x - pad, y0, y1, "Identity");
  const std::string fp = ps.identity().fingerprint();
  tNote(id, tr("Your fingerprint: compare it with a collaborator by phone or chat."), P.textDim, 2);
  if (id.room(gT.rowH, "fingerprint")) {
    const float cw = tBtnW(a, "Copy");
    tPath(a, id.x0 + 6, id.y, id.x1 - 4 - cw - tv::space::kGap - (id.x0 + 6), fp);
    if (tButtonRight(a, id, "##tfpcopy", "Copy", "Copy the fingerprint, to compare with a collaborator by phone or chat")) {
      ImGui::SetClipboardText(fp.c_str());
    }
    id.next(gT.rowH);
  }
  if (!u.confirmReset) {
    if (id.room(gT.rowH, "reset")) {
      if (tButtonRight(a, id, "##tresetid", "Reset identity...", "New keys: collaborators have to invite you again to every project")) {
        u.confirmReset = true;
      }
      id.next(gT.rowH);
    }
  } else {
    tNote(id, std::string(tr("New keys make you a new person to your collaborators.")) + " " + tr("They have to invite you again to every collaborative project."),
          theme::lit(P.danger), 3);
    if (id.room(gT.rowH, "reset confirm")) {
      if (tButtonRight(a, id, "##tresetyes", "Reset identity")) {
        if (active(a)) {
          a.notify(tr("Close the collaborative project first."), true);
        } else {
          std::string err;
          if (!ps.resetIdentity(&err)) {
            a.notify(err, true);
          }
        }
        u.confirmReset = false;
      }
      if (tButtonRight(a, id, "##tresetno", "Cancel", nullptr, true, id.x1 - 4 - tBtnW(a, "Reset identity") - tv::space::kGap)) {
        u.confirmReset = false;
      }
      id.next(gT.rowH);
    }
  }
  // J-146: presence
  TCol pr = id;
  pr.y = std::round(id.foot - gT.rowH);
  const bool on = a.settings.getBool("collab.showOnline");
  if (tCheck(a, pr, "##tshowonline", "Show that I am online to collaborators", on,
             "On: the members of a project see on their welcome screen when you have it open (asked over the encrypted member link). Off: nobody is told, and your welcome screen does not ask about others either")) {
    a.settings.set("collab.showOnline", !on);
    c.setShowOnline(!on);
  }
}

// Connection: settingsPage's connection section (servers, the router, the test).
void tPageNetwork(App& a, ImVec2 t0, ImVec2 t1, float pad) {
  Ui& u = U();
  const auto& P = pal();
  Settings& st = a.settings;
  netFieldsInit(st);
  const float W = t1.x - t0.x - 3 * pad;
  const float xa = t0.x + pad, xb = std::round(xa + W * 0.55f), xc = xb + pad, y0 = t0.y + pad, y1 = t1.y - pad;
  ImFont* mono = tv::monoFont(a);
  const float portW = std::round(tTextW(mono, "00000") + 12);
  TCol col = tBox(a, xa, xb, y0, y1, "Servers");
  col.lab = tv::labelColumn(a, {"STUN server", "Backup servers", "TURN relay", "Login"}, tv::space::kGroup);
  if (col.room(gT.rowH, "stun")) {
    tLabel(col, "STUN server");
    const float fx1 = col.x1 - 4 - portW - tv::space::kGap;
    if (tField(a, "##tstun", tr("none (same network only)"), u.stunBuf, sizeof u.stunBuf, ImVec2(col.fieldX(), col.y), fx1 - col.fieldX(), 0, true)) {
      st.set("collab.stunHost", std::string(u.stunBuf));
    }
    ui::Hint("Tells this computer its public address. The server sees your IP address, nothing else");
    int port = static_cast<int>(st.getNumber("collab.stunPort"));
    if (tNumber(a, "##tstunport", &port, ImVec2(col.x1 - 4 - portW, col.y), portW, 1, 65535)) {
      st.set("collab.stunPort", port);
    }
    col.next(gT.rowH);
  }
  if (col.room(gT.rowH, "backups")) {
    tLabel(col, "Backup servers");
    const float dw = tBtnW(a, "Default");
    if (tField(a, "##tstunbk", tr("host:port, host:port"), u.stunBk, sizeof u.stunBk, ImVec2(col.fieldX(), col.y), col.x1 - 4 - dw - tv::space::kGap - col.fieldX(), 0,
               true)) {
      st.set("collab.stunBackups", std::string(u.stunBk));
    }
    ui::Hint("Public address servers asked when the one above gives no answer, in this order. Each sees your IP address, nothing else");
    if (tButtonRight(a, col, "##tstunbkdefault", "Default", "The built-in backup servers")) {
      std::snprintf(u.stunBk, sizeof u.stunBk, "%s", kDefaultStunBackups);
      st.set("collab.stunBackups", std::string(u.stunBk));
    }
    col.next(gT.rowH);
  }
  if (col.room(gT.rowH, "turn")) {
    tLabel(col, "TURN relay");
    const float fx1 = col.x1 - 4 - portW - tv::space::kGap;
    if (tField(a, "##tturn", tr("optional: turn.example.org"), u.turnHost, sizeof u.turnHost, ImVec2(col.fieldX(), col.y), fx1 - col.fieldX(), 0, true)) {
      st.set("collab.turnHost", std::string(u.turnHost));
    }
    ui::Hint("Relays the encrypted connection when both networks block direct connections. Only needed for strict networks");
    int port = static_cast<int>(st.getNumber("collab.turnPort"));
    if (tNumber(a, "##tturnport", &port, ImVec2(col.x1 - 4 - portW, col.y), portW, 1, 65535)) {
      st.set("collab.turnPort", port);
    }
    col.next(gT.rowH);
  }
  if (col.room(gT.rowH, "turn login")) {
    tLabel(col, "Login");
    const float half = std::floor((col.x1 - 4 - col.fieldX() - tv::space::kGap) * 0.5f);
    if (tField(a, "##tturnuser", tr("user"), u.turnUser, sizeof u.turnUser, ImVec2(col.fieldX(), col.y), half)) {
      st.set("collab.turnUser", std::string(u.turnUser));
    }
    const bool anyTurn = u.turnHost[0] || u.turnUser[0];
    if (tField(a, "##tturnpass", anyTurn ? tr("password: type to replace") : tr("password"), u.turnPass, sizeof u.turnPass,
               ImVec2(col.fieldX() + half + tv::space::kGap, col.y), col.x1 - 4 - (col.fieldX() + half + tv::space::kGap), ImGuiInputTextFlags_Password)) {
      if (secrets::Store().set("collab-turn-password", u.turnPass)) {
        rememberTurnPassword(u.turnPass);
      }
    }
    ui::Hint("The TURN relay's user and password (the password is kept in the system's keychain)");
    col.next(gT.rowH);
  }
  {
    NetSettings filled;
    filled.turnHost = u.turnHost;
    filled.turnUser = u.turnUser;
    filled.turnPass = u.turnPass;
    const std::string problem = turnConfigProblem(filled);
    if (!problem.empty()) {
      tNote(col, problem, theme::lit(P.hot), 3, col.fieldX());
    } else {
      tNote(col, tr("A relay is only needed when both networks block direct connections."), P.textFaint, 2, col.fieldX());
    }
  }
  // -- this computer: the version, the router, the test
  TCol r = tBox(a, xc, t1.x - pad, y0, y1, "This computer");
  r.lab = tv::labelColumn(a, {"Version", "UDP port", "Public address", "Router", "Port opening", "TURN relay", "IPv6"}, tv::space::kGroup);
  if (r.room(gT.rowH, "version")) {
    tLabel(r, "Version");
    const GateState& g = updui::gate(a);
    std::string t;
    ImU32 cl = P.textDim;
    if (g.kind == GateState::Kind::UpdateRequired) {
      t = fmt1(tr("Update required (%s)"), g.latest);
      cl = P.hot;
    } else if (g.kind == GateState::Kind::UpToDate) {
      t = tr("Up to date for collaboration");
    } else {
      t = updui::gateChecking() ? tr("Checking for updates...") : tr("Couldn't check for updates");
    }
    const float uw = g.kind == GateState::Kind::UpdateRequired ? tBtnW(a, "Update") + tv::space::kGap : 0.0f;
    tText(a.fontRegular, r.fieldX(), std::round(r.y + (gT.rowH - gT.fs) * 0.5f), r.x1 - 4 - uw - r.fieldX(), cl, t);
    if (g.kind == GateState::Kind::UpdateRequired && tButtonRight(a, r, "##tgateupdate", "Update", "Update Gridline to collaborate")) {
      updui::showRequired(a, fmt1(tr("Update to Gridline %s to collaborate"), g.latest), g.latest);
    }
    r.next(gT.rowH);
  }
  if (r.room(gT.rowH, "port mapping")) {
    const bool pm = st.getBool("collab.portMapping");
    if (tCheck(a, r, "##tportmap", "Let Gridline ask my router to open a port automatically", pm,
               "UPnP-IGD, NAT-PMP or PCP: a temporary mapping while a collaborative project is open, removed when it closes")) {
      st.set("collab.portMapping", !pm);
    }
    r.next(gT.rowH);
  }
  if (r.room(gT.rowH, "udp port")) {
    tLabel(r, "UDP port");
    int port = static_cast<int>(st.getNumber("collab.udpPort"));
    if (tNumber(a, "##tudpport", &port, ImVec2(r.fieldX(), r.y), portW, 0, 65535)) {
      st.set("collab.udpPort", port);
    }
    ui::Hint("0: any port. Set one (for example 50000) to forward it on your router: then a collaborator can always reach this computer");
    tText(a.fontRegular, r.fieldX() + portW + tv::space::kGroup, std::round(r.y + (gT.rowH - gT.fs) * 0.5f), r.x1 - 4 - (r.fieldX() + portW + tv::space::kGroup),
          P.textFaint, port <= 0 ? std::string(tr("any port")) : std::string(tr("this port only")));
    r.next(gT.rowH);
    if (r.room(gT.rowH, "port forwarded")) {
      const bool fw = st.getBool("collab.portForwarded");
      if (port <= 0) {
        ImGui::BeginDisabled();
      }
      if (tCheck(a, r, "##tportfw", "My router forwards this UDP port to this computer", fw, "Set a port first (0 is any port)", r.fieldX() - 2)) {
        st.set("collab.portForwarded", !fw);
      }
      if (port <= 0) {
        ImGui::EndDisabled();
      }
      r.next(gT.rowH);
    }
  }
  // the test (netTestBlock) and what it found
  if (u.netTest) {
    std::lock_guard<std::mutex> lk(u.netTest->mu);
    if (u.netTest->done) {
      u.lastReport = u.netTest->report;
      u.haveReport = true;
      u.netTest.reset();
    }
  }
  if (r.room(gT.rowH, "test")) {
    const char* l = u.netTest ? "Testing..." : "Test my connection";
    const float w = tBtnW(a, "Test my connection");
    if (tButton(a, "##tnettest", l, ImVec2(r.x0 + 4, r.y), ImVec2(r.x0 + 4 + w, r.y + gT.rowH), u.netTest != nullptr,
                "Asks two STUN servers for your public address, tries the router's automatic port opening and looks for IPv6", !u.netTest)) {
      startNetTest(a);
    }
    r.next(gT.rowH);
  }
  if (u.haveReport) {
    const NetReport& rp = u.lastReport;
    static const char* const nat[] = {i18n::N_("unknown"), i18n::N_("no NAT"), i18n::N_("cone NAT (direct works unless the other side is symmetric)"),
                                      i18n::N_("symmetric NAT"), i18n::N_("UDP blocked")};
    auto row = [&](const char* l, const std::string& v, bool isMono) {
      if (!r.room(gT.rowH, l)) {
        return;
      }
      tLabel(r, l);
      tText(isMono ? mono : a.fontRegular, r.fieldX(), std::round(r.y + (gT.rowH - gT.fs) * 0.5f), r.x1 - 4 - r.fieldX(), isMono ? tv::valueInk() : P.text, v);
      r.next(gT.rowH);
    };
    row("Public address", rp.publicAddress.empty() ? std::string(tr("unknown")) : rp.publicAddress, !rp.publicAddress.empty());
    row("Router", tr(nat[static_cast<int>(rp.nat)]), false);
    row("Port opening", rp.portMapping ? rp.portMappingProtocol + " (" + rp.mappedAddress + ")" : std::string(tr("no")), false);
    if (rp.turnTried) {
      row("TURN relay", rp.turnOk ? std::string(tr("works")) : std::string(tr("failed")), false);
    }
    row("IPv6", rp.ipv6 ? rp.ipv6Address : std::string(tr("no")), rp.ipv6);
    if (!rp.summary.empty()) {
      tNote(r, rp.summary, P.textDim, 3, r.fieldX());
    }
  }
}

}  // namespace

void trackerPage(App& a, ImVec2 p0, ImVec2 p1) {
  Ui& u = U();
  auto& T = u.trk;
  const auto& P = pal();
  T.shownFrame = ImGui::GetFrameCount();
  T.buttonsNow.clear();
  T.overflowNow = false;
  T.overflowWhatNow.clear();
  gT.dl = ImGui::GetWindowDrawList();
  gT.fs = ImGui::GetFontSize();
  gT.rowH = std::round(gT.fs + 6);
  gT.line = tv::space::kLine;
  Controller& c = ctlOf(a);
  Session* s = c.session();
  Hub* hub = c.hub();
  // the pages for the state: a collaborative project open, or not
  std::vector<int> pages = s ? std::vector<int>{kTSession, kTMembers, kTInvite, kTProfile, kTNetwork} : std::vector<int>{kTShare, kTProjects, kTProfile, kTNetwork};
  // what the panel's actions ask for: the name field (New collaborative project...), the invite
  // (Invite again), the reconnect codes
  static SiteId reconnectWas = 0;
  if (u.shareFocus && !s) {
    T.page = kTShare;
  }
  if (u.scrollToInvite && s) {
    T.page = kTInvite;
    u.scrollToInvite = false;
  }
  if (u.reconnectPeer && u.reconnectPeer != reconnectWas && s) {
    T.page = kTInvite;
  }
  reconnectWas = u.reconnectPeer;
  if (std::find(pages.begin(), pages.end(), T.page) == pages.end()) {
    T.page = T.page == kTShare && s ? kTSession : T.page == kTSession && !s ? kTShare : pages.front();
  }
  // ---- the left panel: the title, the pages, the link state, Exit
  const float fs = gT.fs;
  const float leftW = std::round(std::max(150.0f, fs * 11));
  tv::bevelBox(gT.dl, p0, ImVec2(p0.x + leftW, p1.y));
  tv::boxTitle(a, ImVec2(p0.x + 6, p0.y + 4), "Collaboration");
  const float rr = std::round(fs + 5), bh = std::round(fs + 9);
  float y = std::round(p0.y + 6 + fs);
  for (size_t i = 0; i < pages.size(); ++i) {
    const int pg = pages[i];
    ImGui::PushID(pg);
    if (tv::radioRow(a, "##tpage", kTPageNames[pg], ImVec2(p0.x + 4, y), ImVec2(p0.x + leftW - 4, y + rr), T.page == pg, kTPageHints[pg])) {
      T.page = pg;
    }
    ImGui::PopID();
    y += rr;
  }
  {
    // the link state, over Exit
    std::string t1, t2;
    ImU32 lc = P.textFaint;
    bool pulse = false;
    if (!s) {
      t1 = tr("Not shared");
      t2 = tr("Only on this computer");
    } else {
      int online = 1;
      for (const auto& [site, pv] : s->peers()) {
        online += pv.connected && s->isActive(site) ? 1 : 0;
      }
      char b[64];
      std::snprintf(b, sizeof b, tr("%d of %d online"), online, static_cast<int>(s->activeMembers().size()));
      t2 = b;
      if (!s->ready()) {
        t1 = tr("Connecting"), lc = P.hot, pulse = true;
      } else {
        t1 = tr("Shared"), lc = online > 1 ? P.led : P.accent;
      }
      if (const size_t open = s->conflicts().openCount(); open > 0) {
        char r[64];
        std::snprintf(r, sizeof r, tr("%d to review"), static_cast<int>(open));
        t2 += ", ";
        t2 += r;
      }
    }
    const float sy = std::round(p1.y - 4 - bh - tv::space::kGroup - 2 * (fs + 3));
    if (sy > y + 2) {
      tLamp(ImVec2(p0.x + 12, sy + (fs + 3) * 0.5f), lc, pulse);
      tText(tBold(a), p0.x + 22, sy + 1, leftW - 28, P.text, t1);
      tText(a.fontRegular, p0.x + 22, sy + fs + 4, leftW - 28, P.textDim, t2);
    }
  }
  if (tButton(a, "##tcollabexit", "Exit", ImVec2(p0.x + 4, p1.y - 4 - bh), ImVec2(p0.x + leftW - 4, p1.y - 4), false, "Back to the header")) {
    a.showCollab = false;
  }
  // ---- the page
  const ImVec2 t0(p0.x + leftW + 2, p0.y), t1 = p1;
  tv::bevelBox(gT.dl, t0, t1);
  const float pad = tv::space::kGroup;
  gT.dl->PushClipRect(t0, t1, true);
  switch (T.page) {
    case kTShare: tPageShare(a, t0, t1, pad); break;
    case kTProjects: tPageProjects(a, t0, t1, pad); break;
    case kTSession: tPageSession(a, *s, hub, t0, t1, pad); break;
    case kTMembers: tPageMembers(a, *s, hub, t0, t1, pad); break;
    case kTInvite: tPageInvite(a, *s, hub, t0, t1, pad); break;
    case kTProfile: tPageProfile(a, t0, t1, pad); break;
    default: tPageNetwork(a, t0, t1, pad); break;
  }
  gT.dl->PopClipRect();
  T.buttons.swap(T.buttonsNow);
  T.overflow = T.overflowNow;
  T.overflowWhat = T.overflowWhatNow;
}

bool trackerPageShown() { return ImGui::GetFrameCount() - U().trk.shownFrame <= 1; }

namespace {

// Dev: "collab page [<page>]" picks a page (share, projects, session, members, invite, profile,
// connection) and prints what the page shows: "collab page frame F: shown 1 page members pages
// session,members,... buttons a,b(off),... overflow 0 [what]"; "collab press <id>" presses a
// button by its id on the next frame the page draws ("collab page press <id>: done|disabled").
void trackerPageDev(App& a, const std::vector<std::string>& args, int frame) {
  Ui& u = U();
  auto& T = u.trk;
  if (args[0] == "press") {
    T.press = args.size() > 1 ? args[1] : std::string();
    return;
  }
  if (args.size() > 1) {
    for (int i = 0; i < kTCount; ++i) {
      if (args[1] == kTPageIds[i]) {
        T.page = i;
      }
    }
  }
  const bool shown = ImGui::GetFrameCount() - T.shownFrame <= 2;
  std::string b;
  for (const std::string& n : T.buttons) {
    b += (b.empty() ? "" : ",") + n;
  }
  Controller& c = ctlOf(a);
  const bool live = c.session() != nullptr;
  // the panel state its actions change: following a playhead, the invite, the reconnect codes
  const char* inv = "none";
  if (Hub* h = c.hub(); h && !u.inviteId.empty()) {
    if (const Hub::InviteView* iv = h->invite(u.inviteId)) {
      using IS = Hub::InviteStatus;
      inv = iv->status == IS::Preparing ? "preparing" : iv->status == IS::Ready ? "ready" : iv->status == IS::Connecting ? "connecting"
          : iv->status == IS::Connected ? "connected" : iv->status == IS::Expired ? "expired" : "failed";
    }
  }
  std::printf("collab page frame %d: shown %d page %s pages %s follow %d invite %s reconnect %d rename %d buttons %s overflow %d%s%s\n", frame, shown ? 1 : 0,
              T.page >= 0 && T.page < kTCount ? kTPageIds[T.page] : "none", live ? "session,members,invite,profile,connection" : "share,projects,profile,connection",
              u.follow ? 1 : 0, inv, u.reconnectPeer ? 1 : 0, u.renameAt == 3 ? 1 : 0, b.empty() ? "-" : b.c_str(), T.overflow ? 1 : 0, T.overflow ? " " : "",
              T.overflow ? T.overflowWhat.c_str() : "");
  std::fflush(stdout);
}

}  // namespace
