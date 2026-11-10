// Gridline — "edison" pack: the editor form. J-207: laid out from a full map
// of the reference audio editor (PLAN in the track notes) on Gridline's
// metrics (edison_style.h): three head rows on one left and one right edge
// (transport and record options | title, mute; menus | tool strip; the info
// line), level meters at the right end of the head, and the waveform editor
// filling the rest. Every glyph and label is Gridline's own.
#include <algorithm>
#include "../../app/Keys.h"
#include "../../app/UndoUi.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../../core/PluginBrand.h"
#include "../../core/I18n.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../app/Windows.h"
#include "../../../third_party/imgui/imgui_internal.h"
#include "edison_dock.h"
#include "edison_style.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace fs = std::filesystem;

namespace {

using sty::Btn;

// Choice parameter of the slot as a drop-down field (its popup lists the choices).
void paramDrop(App& a, Session& s, const char* id, int k, ImVec2 p0, ImVec2 p1, ImFont* f, float fs, const char* hint) {
  const PluginInfo* info = findPlugin(kType);
  if (!info) return;
  const ParamDesc& d = info->params[static_cast<size_t>(k)];
  const int v = static_cast<int>(param(a, s, k, d.def) + 0.5);
  const std::string cur = v >= 0 && v < static_cast<int>(d.choices.size()) ? d.choices[static_cast<size_t>(v)] : "?";
  if (sty::dropField(id, p0, p1, cur.c_str(), f, fs, s.slot, hint) && s.slot) ImGui::OpenPopup(id);
  ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + 2));
  if (ImGui::BeginPopup(id)) {
    for (int i = 0; i < static_cast<int>(d.choices.size()); ++i)
      if (ImGui::MenuItem(d.choices[static_cast<size_t>(i)].c_str(), nullptr, i == v)) setParam(a, s, k, i);
    ImGui::EndPopup();
  }
}

// Vertical level meter (segments, green / amber / red from the bottom).
void meterBar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float lv) {
  skin::insetField(dl, p0, p1, C(col::Lcd), 1.0f);
  const float db = lv > 1e-6f ? 20.f * std::log10(lv) : -90.f;
  const float n = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
  const float top = p1.y - (p1.y - p0.y) * n;
  for (float yy = p1.y - 2; yy > top; yy -= 3) {
    const float frac = (p1.y - yy) / (p1.y - p0.y);
    const ImU32 c = frac > 0.92f ? C(col::Rec) : frac > 0.75f ? C(0xD7C94A) : C(col::Wave);
    dl->AddRectFilled(ImVec2(p0.x + 1, yy - 2), ImVec2(p1.x - 1, yy), c);
  }
}

// A clickable info field (hover lifts it); true on click.
bool infoButton(const char* id, ImVec2 p0, ImVec2 p1, const char* value, ImFont* f, float fs, bool enabled, const char* hint) {
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(p1.x - p0.x, p1.y - p0.y));
  const bool hov = ImGui::IsItemHovered() && enabled;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  sty::field(dl, p0, p1, nullptr, value, hov ? C(col::Text) : C(col::LcdText), f, fs, fs, 1);
  if (hov) dl->AddRect(p0, p1, C(col::SelEdge, 0.6f), sty::kRadius);
  if (hint) ui::Hint(hint), ui::Tooltip(hint);
  return enabled && ImGui::IsItemClicked();
}

}  // namespace

// ------------------------------------------------------------------ the form
void drawForm(App& a, Session& s, float width, float maxHeight) { drawFormLayout(a, s, dock::floatingLayout(width, maxHeight)); }

// K-15: pinned into the main layout, laid out for the dock's space (edison_dock.h).
void drawFormDocked(App& a, Session& s, ImVec2 avail) { drawFormLayout(a, s, dock::layoutFor(avail.x, avail.y)); }

void drawFormLayout(App& a, Session& s, const dock::Layout& Lo) {
  static const skin::Skin kSkin = [] {
    skin::Skin k = skin::Skin::fl(col::Panel, col::WaveBg, col::BtnLo, col::Wave, col::Btn, col::Text);
    k.lcdBg = C(col::Lcd);
    k.lcdText = C(col::LcdText);
    k.select = C(col::SelEdge);
    k.textDim = C(col::TextDim);
    return k;
  }();
  skin::Scope scope(kSkin);
  sty::Metrics M;
  M.s = Lo.scale;
  const bool compact = Lo.compact;  // K-15: a narrow dock: the head rows stacked
  auto P = [&M](float v) { return M.px(v); };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* fm = a.fontMono ? a.fontMono : ImGui::GetFont();
  ImFont* fb = a.fontBold ? a.fontBold : ImGui::GetFont();
  const float fsVal = M.font(sty::kFontValue), fsCap = M.font(sty::kFontCap);
  ImGui::PushID(static_cast<int>(s.key & 0x7FFFFFFF));

  // ---- geometry (whole pixels; one left edge L and one right edge R for every row)
  const float W = Lo.formW;
  const float pad = P(sty::kPad), btn = P(sty::kBtn), fld = P(sty::kField), cg = P(sty::kCellGap), gg = P(sty::kGroupGap),
              sg = P(sty::kSectionGap), ico = P(sty::kIcon);
  const float y1 = Lo.yTransport, y2 = Lo.yMenus, y3 = Lo.yInfo, headH = Lo.headH;
  const float L = pad, R = W - pad;
  const float meterW = P(sty::kMeterW), meterX = R - 2 * meterW - cg;
  const float Rc = meterX - gg;  // right edge of the head's controls (the meters sit beyond it)
  const float waveH = Lo.waveH;
  const float H = headH + waveH + pad;
  skin::Form F = skin::formAt(W, H, W, 1.0f);
  const ImVec2 o(std::round(F.o.x), std::round(F.o.y));
  auto pt = [&o](float x, float y) { return ImVec2(o.x + x, o.y + y); };
  const float cy1 = y1 + btn * 0.5f;
  const float cyR = compact ? Lo.yRec + fld * 0.5f : cy1;  // the record options' centre line
  const float meterBottom = compact ? Lo.yRec + fld : y3 + fld;

  // Panel: the head on a soft gradient, the editor below on the recessed tone.
  skin::gradientRect(dl, pt(0, 0), pt(W, headH), C(col::PanelHi), C(col::Panel));
  dl->AddRectFilled(pt(0, headH), pt(W, H), C(col::PanelLo));
  dl->AddLine(pt(0, headH - 0.5f), pt(W, headH - 0.5f), C(col::BtnLo), 1.f);

  // ---- row 1: transport (one group of equal buttons), record options, mute
  const bool playing = isPlaying(a, s);
  const bool loopOn = param(a, s, EdLoop, 0) >= 0.5;
  const bool hasAudio = s.frames() > 0;
  const bool busy = s.job.active();
  {
    float x = L;
    auto cell = [&](const char* id, const char* name, ui::Icon icon, const Btn& b) {
      const ImVec2 p0 = pt(x, y1), p1 = pt(x + btn, y1 + btn);
      const bool c = sty::iconButton(id, p0, p1, icon, ico, b);
      sty::note("row1.transport", name, p0, p1);
      x += btn + cg;
      return c;
    };
    Btn b;
    b.hint = "Follow transport: playback starts and stops with the song";
    b.lit = param(a, s, EdFollowHost, 0) >= 0.5;
    b.enabled = s.slot;
    if (cell("follow", "follow", ui::Icon::Follow, b)) setParam(a, s, EdFollowHost, b.lit ? 0 : 1);
    b = Btn{};
    b.hint = "Loop playback (L)";
    b.lit = loopOn;
    b.enabled = s.slot;
    if (cell("loop", "loop", ui::Icon::EdisonLoop, b)) setParam(a, s, EdLoop, loopOn ? 0 : 1);
    b = Btn{};
    b.hint = "Play / pause the selection or from the cursor (Space)";
    b.lit = playing;
    b.enabled = hasAudio || playing;
    if (cell("play", "play", playing ? ui::Icon::Pause : ui::Icon::Play, b)) playing ? stopPlay(a, s) : play(a, s);
    b = Btn{};
    b.hint = "Scrub: drag in the waveform to hear it at the speed you move (or hold Alt, or drag along the ruler). "
             "The play position stays where you let go";
    b.lit = s.scrubMode;
    if (cell("scrub", "scrub", ui::Icon::Scrub, b)) s.scrubMode = !s.scrubMode;
    b = Btn{};
    b.hint = "Stop (cursor returns)";
    if (cell("stop", "stop", ui::Icon::Stop, b)) stopPlay(a, s);
    b = Btn{};
    b.hint = s.slot ? "Record from the slot input (R)" : "Recording needs the editor in a mixer slot";
    b.lit = s.rec.armed() || s.rec.recording();
    b.enabled = s.slot;
    b.litRole = 0xE5484D;
    if (cell("rec", "rec", ui::Icon::Record, b)) toggleRecord(a, s);
    x = compact ? L : x + sg - cg;
    // Record options: when, maximum length, append; one centre line with the transport.
    const float ft = cyR - fld * 0.5f;
    paramDrop(a, s, "##recmode", EdRecMode, pt(x, ft), pt(x + P(116), ft + fld), fm, fsVal, "When to start recording");
    sty::note("row1.record", "mode", pt(x, ft), pt(x + P(116), ft + fld));
    x += P(116) + gg;
    sty::caption(dl, o.x + x, o.y + cyR, i18n::tr("MAX"), s.slot ? C(col::TextDim) : C(col::TextDim, 0.6f), fm, fsCap);
    x += std::round(sty::textW(fm, fsCap, i18n::tr("MAX"))) + P(6);
    paramDrop(a, s, "##recmax", EdRecMax, pt(x, ft), pt(x + P(84), ft + fld), fm, fsVal, "Maximum recording length");
    sty::note("row1.record", "max", pt(x, ft), pt(x + P(84), ft + fld));
    x += P(84) + gg + P(4);
    {
      const bool on = param(a, s, EdAppend, 0) >= 0.5;
      const float box = P(14);
      const float w = box + std::round(box * 0.5f) + std::round(sty::textW(fm, fsCap, i18n::tr("APPEND")));
      const ImVec2 tl = pt(x, std::round(cyR - box * 0.5f));
      if (sty::check("append", tl, box, w, i18n::tr("APPEND"), on, s.slot, fm, fsCap,
                     "Append recordings after the existing audio (else a take replaces it)") &&
          s.slot)
        setParam(a, s, EdAppend, on ? 0 : 1);
      sty::note("row1.record", "append", tl, ImVec2(tl.x + box, tl.y + box));
    }

    // Title (the wordmark, right-aligned before the mute button) and mute input.
    const float muteX = Rc - btn;
    Btn m;
    const bool muted = param(a, s, EdMuteInput, 0) >= 0.5;
    m.lit = muted;
    m.enabled = s.slot;
    m.litRole = 0xE5484D;
    m.hint = "Mute input (the slot outputs only the editor's playback)";
    if (sty::iconButton("mute", pt(muteX, y1), pt(muteX + btn, y1 + btn), muted ? ui::Icon::SpeakerSmallOff : ui::Icon::SpeakerSmall, ico, m))
      setParam(a, s, EdMuteInput, muted ? 0 : 1);
    sty::note("row1.right", "mute", pt(muteX, y1), pt(muteX + btn, y1 + btn));
    // No title here: the plugin window's header already carries Loupe's wordmark.
  }
  // Level meters: input and output, the full height of the head rows.
  meterBar(dl, pt(meterX, y1), pt(meterX + meterW, meterBottom), s.inMeter);
  meterBar(dl, pt(meterX + meterW + cg, y1), pt(R, meterBottom), s.outMeter);
  if (ImGui::IsMouseHoveringRect(pt(meterX, y1), pt(R, meterBottom))) a.setHint(i18n::tr("Input and output level"));

  // ---- row 2: menus (equal cells, flat) | tool strip (groups of equal buttons, right edge)
  {
    const float stripW = 19 * btn + 13 * cg + 5 * gg;
    const float toolX = Rc - stripW;
    float cellW = P(52);
    if (L + 9 * cellW > toolX - sg) cellW = std::floor((toolX - sg - L) / 9);
    const int perRow = Lo.menuRows == 2 ? 5 : 9;
    if (compact) cellW = std::floor((R - L) / static_cast<float>(perRow));  // K-15: the menus across the dock (two rows when narrow)
    drawMenus(a, s, pt(L, y2), cellW, btn, M, perRow);

    bool r = false;
    float x = compact ? L : toolX;
    float ty = compact ? Lo.yTools : y2;  // K-15: compact: the strip wraps over rows of its own
    auto tb = [&](const char* id, ui::Icon icon, const char* hint, bool lit = false, bool en = true) {
      if (compact && x > L && x + btn > R + 0.5f) {
        x = L;
        ty += btn + cg;
      }
      Btn b;
      b.hint = hint;
      b.lit = lit;
      b.enabled = en && !busy;
      b.right = &r;
      const ImVec2 p0 = pt(x, ty), p1 = pt(x + btn, ty + btn);
      const bool c = sty::iconButton(id, p0, p1, icon, ico, b);
      sty::note("row2.tools", id, p0, p1);
      x += btn + cg;
      return c;
    };
    auto gap = [&] {
      if (!compact || x > L) x += gg - cg;
    };
    if (tb("t_undo", ui::Icon::Undo, "Undo / right-click: redo (Ctrl+Z / Ctrl+Y)", false, s.hist.canUndo() || s.hist.canRedo())) undo(a, s);
    if (r) redo(a, s);
    if (tb("t_redo", ui::Icon::Redo, "Redo", false, s.hist.canRedo())) redo(a, s);
    if (tb("t_copy", ui::Icon::Clone, "Copy selection / right-click: paste", false, hasAudio)) clipboard() = extract(s.cur().a(), target(s));
    if (r && clipboard().frames() > 0) {
      Audio clip = clipboard();
      Range sel = s.sel;
      int64_t at = s.cursor;
      runEdit(a, s, "Paste", [clip, sel, at](const Content& c) { return sel.empty() ? insertAudio(c, at, clip) : replaceRange(c, sel, clip); });
    }
    gap();
    if (tb("t_loop", ui::Icon::EdisonLoopSet, "Set loop points from the selection / right-click: crossfade the loop seam", s.cur().hasLoop(), hasAudio))
      applyNow(a, s, s.sel.empty() ? "Clear loop" : "Set loop", setLoop(s.cur(), s.sel));
    if (r && s.cur().hasLoop()) {
      Range lp{s.cur().loopStart, s.cur().loopEnd};
      runEdit(a, s, "Crossfade loop", [lp](const Content& c) { return crossfadeLoop(c, lp, static_cast<int64_t>(c.rate() * 0.03)); });
    }
    if (tb("t_marker", ui::Icon::Flag, "Add marker at the cursor, or a region from the selection (M) / right-click: remove the nearest marker", false, hasAudio))
      addMarkerAtCursor(a, s);
    if (r && !s.cur().markers.empty()) {
      size_t best = 0;
      for (size_t i = 1; i < s.cur().markers.size(); ++i)
        if (std::llabs(s.cur().markers[i].pos - s.cursor) < std::llabs(s.cur().markers[best].pos - s.cursor)) best = i;
      applyNow(a, s, "Remove marker", removeMarker(s.cur(), best));
    }
    if (tb("t_trim", ui::Icon::Trim, "Trim side silence / right-click: noise gate...", false, hasAudio))
      runEdit(a, s, "Trim side silence", [](const Content& c) { return trimSilence(c, -60); });
    if (r) openTool(s, Tool::Gate);
    if (tb("t_slice", ui::Icon::SliceWave, "Detect beats and place slice markers / right-click: slicing options...", false, hasAudio)) detectSlices(a, s, 0.5);
    if (r) openTool(s, Tool::Slice);
    gap();
    if (tb("t_save", ui::Icon::Save, "Save as WAV... / right-click: save as FLAC...", false, hasAudio)) saveAs(a, s, FileFormat::Wav);
    if (r) saveAs(a, s, FileFormat::Flac);
    gap();
    const Range tr = target(s);
    const Curve fc = static_cast<Curve>(s.fadeCurve);
    if (tb("t_fadein", ui::Icon::FadeIn, "Fade in the selection / right-click: declick in (5 ms)", false, hasAudio))
      runEdit(a, s, "Fade in", [tr, fc](const Content& c) { return fadeIn(c, tr, fc); });
    if (r) runEdit(a, s, "Declick in", [tr](const Content& c) { return fadeIn(c, {tr.a, std::min(tr.b, tr.a + static_cast<int64_t>(c.rate() * 0.005))}, Curve::SCurve); });
    if (tb("t_fadeout", ui::Icon::FadeOut, "Fade out the selection / right-click: declick out (5 ms)", false, hasAudio))
      runEdit(a, s, "Fade out", [tr, fc](const Content& c) { return fadeOut(c, tr, fc); });
    if (r) runEdit(a, s, "Declick out", [tr](const Content& c) { return fadeOut(c, {std::max(tr.a, tr.b - static_cast<int64_t>(c.rate() * 0.005)), tr.b}, Curve::SCurve); });
    if (tb("t_norm", ui::Icon::Normalize, "Normalize to 0 dB / right-click: normalize each region", false, hasAudio))
      runEdit(a, s, "Normalize", [tr](const Content& c) { return normalize(c, tr, 0); });
    if (r)
      runEdit(a, s, "Normalize regions", [](const Content& c) {
        Content n = c;
        for (const auto& g : c.regions) n = normalize(n, {g.start, g.end}, 0);
        return n;
      });
    gap();
    if (tb("t_reverb", ui::Icon::ReverbTail, "Convolution reverb...", false, hasAudio)) openTool(s, Tool::Reverb);
    if (tb("t_blur", ui::Icon::Blur, "Blur...", false, hasAudio)) openTool(s, Tool::Blur);
    if (tb("t_eq", ui::Icon::EqCurve, "Equalize (drawn curve)...", false, hasAudio)) openTool(s, Tool::Equalize);
    if (tb("t_env", ui::Icon::EnvelopePoints, "Amplitude envelope (drawn)...", false, hasAudio)) openTool(s, Tool::Envelope);
    if (tb("t_stretch", ui::Icon::Stretch, "Time stretch / pitch shift... / right-click: tune to note...", false, hasAudio)) openTool(s, Tool::Stretch);
    if (r) openTool(s, Tool::Tune);
    if (tb("t_denoise", ui::Icon::Denoise, "Clean up (denoise)... / right-click: take the noise profile from the selection", s.noise.valid(), hasAudio))
      openTool(s, Tool::Denoise);
    if (r) {
      s.noise = acquireNoise(s.cur().a(), target(s));
      say(s, s.noise.valid() ? "Noise profile taken from the selection" : "Select at least 50 ms of noise first");
    }
    gap();
    if (tb("t_send", ui::Icon::SendOut, "Send to the playlist as an audio clip / right-click: send to a new sampler channel", false, hasAudio)) sendToPlaylist(a, s);
    if (r) sendToSampler(a, s, false);
    // K-17: the drag handle: the selection (or the whole edit) dragged onto the playlist, the rack or a sampler.
    if (!ImGui::GetDragDropPayload()) s.dragOut.clear();
    tb("t_drag", ui::Icon::GripDots,
       "Drag the audio out: onto a playlist track (an audio clip where you drop it), the channel rack or a sampler. "
       "With a selection, only the selection goes",
       !s.dragOut.empty(), hasAudio);
    if (ImGui::IsItemActivated()) s.dragOutUsed = false;
    if (ImGui::IsItemDeactivated() && !s.dragOutUsed && hasAudio)  // a click without a drag
      say(s, "Drag this handle onto the playlist, the channel rack or a sampler");
    devNoteRect("loupedrag");  // dev: an "sdlat" target
    if (hasAudio && !busy) dragOutSource(a, s);
  }

  // ---- row 3: the info line (recessed fields of one height; captions inside)
  {
    char b[96];
    float x = L;
    const float y = y3;
    auto next = [&](float w) {
      const float x0 = x;
      x += w + P(sty::kFieldGap);
      return x0;
    };
    const float wRate = P(84), wBits = P(64), wCh = P(72), wBpm = P(80);
    const bool mono = s.cur().a().channels == 1;
    if (!compact) {  // K-15: compact: rate, bits, channels and tempo are left out (Format menu)
    std::snprintf(b, sizeof b, "%.0f Hz", s.rate());
    float fx = next(wRate);
    sty::field(dl, pt(fx, y), pt(fx + wRate, y + fld), nullptr, b, C(col::LcdText), fm, fsVal, fsCap, 1);
    sty::note("row3.info", "rate", pt(fx, y), pt(fx + wRate, y + fld));
    fx = next(wBits);
    sty::field(dl, pt(fx, y), pt(fx + wBits, y + fld), nullptr, "32-bit", C(col::LcdText), fm, fsVal, fsCap, 1);
    if (ImGui::IsMouseHoveringRect(pt(fx, y), pt(fx + wBits, y + fld))) a.setHint(i18n::tr("Edits are kept as 32-bit float; exports are 24-bit"));
    sty::note("row3.info", "bits", pt(fx, y), pt(fx + wBits, y + fld));
    fx = next(wCh);
    if (infoButton("chans", pt(fx, y), pt(fx + wCh, y + fld), mono ? i18n::tr("Mono") : i18n::tr("Stereo"), fm, fsVal, hasAudio && !busy,
                   mono ? "Mono: click to make stereo" : "Stereo: click to mix to mono"))
      runEdit(a, s, mono ? "To stereo" : "To mono", [mono](const Content& c) { return mono ? toStereo(c) : toMono(c); });
    sty::note("row3.info", "chans", pt(fx, y), pt(fx + wCh, y + fld));
    std::snprintf(b, sizeof b, "%.0f BPM", a.doc.song().tempo);
    fx = next(wBpm);
    sty::field(dl, pt(fx, y), pt(fx + wBpm, y + fld), nullptr, b, C(col::LcdText), fm, fsVal, fsCap, 1);
    sty::note("row3.info", "bpm", pt(fx, y), pt(fx + wBpm, y + fld));
    }
    // Position / length on the right, flush with the right edge; the name takes the rest.
    const float wLen = P(152), wPos = P(232);
    // K-15: compact: the name across the dock, cursor and length on the row under it.
    const float posX = compact ? L : Rc - wLen - P(sty::kFieldGap) - wPos;
    const float nameR = compact ? R : posX - P(sty::kFieldGap);
    const float y2i = compact ? Lo.yInfo2 : y;
    const float lenX = compact ? std::round(L + (R - L) * 0.58f) : Rc - wLen, lenR = compact ? R : Rc;
    const float posR = compact ? lenX - P(sty::kFieldGap) : posX + wPos;
    std::string name;
    if (s.slot) {
      name = s.committed.empty() || s.committed[0] == '\x01' ? std::string(i18n::tr("(empty)")) : fs::path(s.committed).stem().string();
      const size_t dash = name.rfind('-');
      if (dash != std::string::npos && name.size() - dash == 13) name.resize(dash);  // drop the version hash
    } else if (!s.file.empty()) {
      name = fs::path(s.file).filename().string();
    } else {
      name = a.doc.song().channel(s.channel) ? a.doc.song().channel(s.channel)->name : std::string("?");
    }
    // While recording (or armed) the name field says so, in the record colour.
    const char* nameCap = i18n::tr("NAME");
    ImU32 nameCol = hasAudio ? C(col::Text) : C(col::TextDim);
    if (s.slot && s.rec.recording()) {
      nameCap = i18n::tr("RECORDING");
      name = timeText(a, s, s.rec.recordedFrames());
      nameCol = C(col::Rec);
    } else if (s.slot && s.rec.armed()) {
      static const char* kWait[] = {"starting", "waiting for signal", "waiting for signal", "waiting for playback"};
      nameCap = i18n::tr("ARMED");
      name = i18n::tr(kWait[std::clamp(s.rec.config().mode, 0, 3)]);
      nameCol = C(col::Rec);
    }
    sty::field(dl, pt(x, y), pt(nameR, y + fld), nameCap, name.c_str(), nameCol, fm, fsVal, fsCap, 0);
    sty::note("row3.info", "name", pt(x, y), pt(nameR, y + fld));
    const Range sr = s.sel;
    const std::string selTxt = sr.empty() ? timeText(a, s, s.cursor) : timeText(a, s, sr.a) + " - " + timeText(a, s, sr.b);
    const std::string lenTxt = timeText(a, s, sr.empty() ? s.frames() : sr.len());
    sty::field(dl, pt(posX, y2i), pt(posR, y2i + fld), sr.empty() ? i18n::tr("CURSOR") : i18n::tr("SELECTION"), hasAudio ? selTxt.c_str() : "-",
               C(col::LcdText), fm, fsVal, fsCap, 2);
    sty::note("row3.info", "pos", pt(posX, y2i), pt(posR, y2i + fld));
    sty::field(dl, pt(lenX, y2i), pt(lenR, y2i + fld), sr.empty() ? i18n::tr("LENGTH") : i18n::tr("SEL LENGTH"), hasAudio ? lenTxt.c_str() : "-",
               C(col::LcdText), fm, fsVal, fsCap, 2);
    sty::note("row3.info", "len", pt(lenX, y2i), pt(lenR, y2i + fld));
  }

  // ---- waveform editor (the rest of the form, on the same left and right edges)
  drawWaveArea(a, s, pt(L, headH), pt(R, headH + waveH), M);

  // Busy: a plate over the lanes with the edit's name and its running time.
  if (busy) {
    char b[96];
    std::snprintf(b, sizeof b, "%s...  %.1f s", s.job.label.c_str(), ImGui::GetTime() - s.job.started);
    const float fs = M.font(13);
    const ImVec2 ts = fb->CalcTextSizeA(fs, FLT_MAX, 0, b);
    const ImVec2 c = pt(std::round((L + R) * 0.5f), std::round(headH + waveH * 0.45f));
    const float pw = std::round(ts.x * 0.5f + P(20)), ph = std::round(P(18));
    const ImVec2 q0(c.x - pw, c.y - ph), q1(c.x + pw, c.y + ph);
    dl->AddRectFilled(ImVec2(q0.x + 2, q0.y + 3), ImVec2(q1.x + 2, q1.y + 3), IM_COL32(0, 0, 0, 70), sty::kRadius + 1);
    skin::raisedFace(dl, q0, q1, C(col::Panel), sty::kRadius + 1);
    // indeterminate bar under the text
    const float bw = q1.x - q0.x - P(16), t = static_cast<float>(std::fmod(ImGui::GetTime() * 0.8, 1.0));
    const float bx0 = q0.x + P(8), by = q1.y - P(6);
    dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx0 + bw, by + 2), C(col::BtnLo), 1);
    dl->AddRectFilled(ImVec2(bx0 + bw * std::max(0.f, t - 0.25f), by), ImVec2(bx0 + bw * std::min(1.f, t + 0.05f), by + 2), C(col::Lit), 1);
    sty::textAt(dl, fb, fs, c.x, c.y - P(2), C(col::Text), b, 1);
  }

  drawToolDialogs(a, s);
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) handleKeys(a, s);
  skin::reserve(F);
  sty::frameEnd();
  // While the audio is dragged out of a floating Loupe, the window turns see-through and lets
  // the pointer through: the playlist, the channel rack or a sampler under it takes the drop
  // (Loupe's window covers most of them otherwise, and a drop on its own waveform did nothing).
  if (!s.dragOut.empty() && ImGui::GetDragDropPayload() && !inPanel(s)) {
    ImGuiWindow* w = ImGui::GetCurrentWindow()->RootWindow;
    w->Flags |= ImGuiWindowFlags_NoMouseInputs;  // the next frame's hovering skips it
    for (ImDrawVert& v : w->DrawList->VtxBuffer) {
      const ImU32 al = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
      v.col = (v.col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(al * 0.14f) << IM_COL32_A_SHIFT);
    }
  }
  ImGui::PopID();
}

// ------------------------------------------------------------------ menus (reference: options menu groups)
void drawMenus(App& a, Session& s, ImVec2 tl, float cellW, float h, const sty::Metrics& M, int perRow) {
  static const char* kMenus[] = {"File", "Format", "Edit", "Tools", "Regions", "View", "Snap", "Select", "Zoom"};
  ImFont* fr = a.fontRegular ? a.fontRegular : ImGui::GetFont();
  const bool busy = s.job.active();
  const bool has = s.frames() > 0;
  for (int i = 0; i < 9; ++i) {
    perRow = std::max(1, perRow);
    const ImVec2 p0(tl.x + static_cast<float>(i % perRow) * cellW, tl.y + static_cast<float>(i / perRow) * h), p1(p0.x + cellW, p0.y + h);
    char id[16], pid[24];
    std::snprintf(id, sizeof id, "##menu%d", i);
    std::snprintf(pid, sizeof pid, "edmenu%d", i);
    Btn b;
    b.flat = true;
    b.lit = ImGui::IsPopupOpen(pid);
    if (sty::textButton(id, p0, p1, i18n::tr(kMenus[i]), fr, M.font(sty::kFontMenu), b) || (s.devMenu == i && ImGui::GetFrameCount() > 20)) {
      ImGui::OpenPopup(pid);
      s.devMenu = -1;
    }
    sty::note("row2.menus", kMenus[i], p0, p1);
    ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + 2));
    if (!ImGui::BeginPopup(pid)) continue;
    const Range tr = target(s);
    switch (i) {
      case 0:  // File
        if (ImGui::MenuItem(i18n::tr("Load audio file..."), nullptr, false, !busy)) openFile(a, s);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Save as WAV..."), nullptr, false, has)) saveAs(a, s, FileFormat::Wav);
        if (ImGui::MenuItem(i18n::tr("Save as FLAC..."), nullptr, false, has)) saveAs(a, s, FileFormat::Flac);
        if (ImGui::MenuItem(i18n::tr("Export selection..."), nullptr, false, has)) exportSelection(a, s);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Send to playlist as audio clip"), nullptr, false, has && !busy)) sendToPlaylist(a, s);
        if (ImGui::MenuItem(i18n::tr("Send to new sampler channel"), nullptr, false, has && !busy)) sendToSampler(a, s, false);
        if (ImGui::MenuItem(i18n::tr("Send to new slicer channel (markers = slices)"), nullptr, false, has && !busy)) sendToSampler(a, s, true);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Split stems..."), nullptr, false, has && !busy)) splitStems(a, s);  // J-191
        break;
      case 1:  // Format
        if (ImGui::MenuItem(i18n::tr("Mix to mono"), nullptr, s.cur().a().channels == 1, has && !busy)) runEdit(a, s, "To mono", [](const Content& c) { return toMono(c); });
        if (ImGui::MenuItem(i18n::tr("Make stereo"), nullptr, false, has && !busy && s.cur().a().channels == 1))
          runEdit(a, s, "To stereo", [](const Content& c) { return toStereo(c); });
        if (ImGui::MenuItem(i18n::tr("Swap channels"), nullptr, false, has && !busy)) runEdit(a, s, "Swap channels", [](const Content& c) { return swapChannels(c); });
        if (ImGui::BeginMenu(i18n::tr("Sample rate"), has && !busy)) {
          for (int rate : {22050, 32000, 44100, 48000, 88200, 96000}) {
            char b[32];
            std::snprintf(b, sizeof b, "%d Hz", rate);
            if (ImGui::MenuItem(b, nullptr, std::lround(s.rate()) == rate)) {
              s.rsRate = rate;
              openTool(s, Tool::Resample);
            }
          }
          ImGui::EndMenu();
        }
        break;
      case 2:  // Edit
        if (ImGui::MenuItem(i18n::tr("Undo"), keys::label("edit.undo"), false, s.hist.canUndo() && !busy)) undo(a, s);
        if (ImGui::MenuItem(i18n::tr("Redo"), keys::label("edit.redo"), false, s.hist.canRedo() && !busy)) redo(a, s);
        if (ImGui::BeginMenu(i18n::tr("History"), s.hist.canUndo())) {
          auto l = s.hist.labels();
          for (size_t k = l.size(); k-- > 0;) ImGui::MenuItem(l[k].c_str(), nullptr, false, false);
          ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Cut"), "Ctrl+X", false, has && !s.sel.empty() && !busy)) {
          clipboard() = extract(s.cur().a(), s.sel);
          Range r = s.sel;
          runEdit(a, s, "Cut", [r](const Content& c) { return deleteRange(c, r); });
        }
        if (ImGui::MenuItem(i18n::tr("Copy"), "Ctrl+C", false, has)) clipboard() = extract(s.cur().a(), tr);
        if (ImGui::MenuItem(i18n::tr("Paste"), "Ctrl+V", false, clipboard().frames() > 0 && !busy)) {
          Audio clip = clipboard();
          Range sel = s.sel;
          int64_t at = s.cursor;
          runEdit(a, s, "Paste", [clip, sel, at](const Content& c) { return sel.empty() ? insertAudio(c, at, clip) : replaceRange(c, sel, clip); });
        }
        if (ImGui::MenuItem(i18n::tr("Delete"), "Del", false, has && !s.sel.empty() && !busy)) {
          Range r = s.sel;
          runEdit(a, s, "Delete", [r](const Content& c) { return deleteRange(c, r); });
        }
        if (ImGui::MenuItem(i18n::tr("Silence selection"), nullptr, false, has && !s.sel.empty() && !busy)) {
          Range r = s.sel;
          runEdit(a, s, "Silence", [r](const Content& c) { return silenceRange(c, r); });
        }
        if (ImGui::MenuItem(i18n::tr("Insert silence..."), nullptr, false, !busy)) openTool(s, Tool::Silence);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Trim to selection"), "Ctrl+T", false, has && !s.sel.empty() && !busy)) {
          Range r = s.sel;
          runEdit(a, s, "Trim to selection", [r](const Content& c) { return cropTo(c, r); });
        }
        if (ImGui::MenuItem(i18n::tr("Trim side silence"), nullptr, false, has && !busy)) runEdit(a, s, "Trim side silence", [](const Content& c) { return trimSilence(c, -60); });
        break;
      case 3:  // Tools
        if (ImGui::BeginMenu(i18n::tr("Scripts"), !busy)) {  // J-57: Lua audio scripts (SC-05)
          audioScriptMenuItems(a, s);
          ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(i18n::tr("Amplitude"), has && !busy)) {
          if (ImGui::MenuItem(i18n::tr("Normalize..."))) openTool(s, Tool::Normalize);
          if (ImGui::MenuItem(i18n::tr("Amplify..."))) openTool(s, Tool::Amplify);
          if (ImGui::MenuItem(i18n::tr("Fade in"))) runEdit(a, s, "Fade in", [tr, k = static_cast<Curve>(s.fadeCurve)](const Content& c) { return fadeIn(c, tr, k); });
          if (ImGui::MenuItem(i18n::tr("Fade out"))) runEdit(a, s, "Fade out", [tr, k = static_cast<Curve>(s.fadeCurve)](const Content& c) { return fadeOut(c, tr, k); });
          if (ImGui::BeginMenu(i18n::tr("Fade curve"))) {
            for (int k = 0; k < static_cast<int>(Curve::Count); ++k)
              if (ImGui::MenuItem(curveName(static_cast<Curve>(k)), nullptr, s.fadeCurve == k)) s.fadeCurve = k;
            ImGui::EndMenu();
          }
          if (ImGui::MenuItem(i18n::tr("Amplitude envelope..."))) openTool(s, Tool::Envelope);
          ImGui::Separator();
          if (ImGui::MenuItem(i18n::tr("Reverse"))) runEdit(a, s, "Reverse", [tr](const Content& c) { return reverse(c, tr); });
          if (ImGui::MenuItem(i18n::tr("Invert polarity"))) runEdit(a, s, "Invert", [tr](const Content& c) { return invert(c, tr); });
          if (ImGui::MenuItem(i18n::tr("Remove DC offset"))) runEdit(a, s, "Remove DC", [tr](const Content& c) { return removeDc(c, tr); });
          if (ImGui::MenuItem(i18n::tr("Noise gate..."))) openTool(s, Tool::Gate);
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(i18n::tr("Noise"), has && !busy)) {
          if (ImGui::MenuItem(i18n::tr("Take noise profile from selection"), nullptr, s.noise.valid())) {
            s.noise = acquireNoise(s.cur().a(), tr);
            say(s, s.noise.valid() ? "Noise profile taken" : "Select at least 50 ms of noise first");
          }
          if (ImGui::MenuItem(i18n::tr("Clean up (denoise)..."))) openTool(s, Tool::Denoise);
          if (ImGui::MenuItem(i18n::tr("Declick..."))) openTool(s, Tool::Declick);
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(i18n::tr("Time and pitch"), has && !busy)) {
          if (ImGui::MenuItem(i18n::tr("Time stretch / pitch shift..."))) openTool(s, Tool::Stretch);
          if (ImGui::MenuItem(i18n::tr("Tune to note..."))) openTool(s, Tool::Tune);
          if (ImGui::MenuItem(i18n::tr("Resample..."))) openTool(s, Tool::Resample);
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(i18n::tr("Effects"), has && !busy)) {
          if (ImGui::MenuItem(i18n::tr("Equalize..."))) openTool(s, Tool::Equalize);
          if (ImGui::MenuItem(i18n::tr("Convolution reverb..."))) openTool(s, Tool::Reverb);
          if (ImGui::MenuItem(i18n::tr("Blur..."))) openTool(s, Tool::Blur);
          if (ImGui::MenuItem(i18n::tr("Apply spectral brush..."), nullptr, false, !s.brush.empty())) openTool(s, Tool::Spectral);
          ImGui::EndMenu();
        }
        break;
      case 4:  // Regions
        if (ImGui::MenuItem(i18n::tr("Add marker / region"), "M", false, has)) addMarkerAtCursor(a, s);
        if (ImGui::MenuItem(i18n::tr("Set loop from selection"), nullptr, false, has && !s.sel.empty())) applyNow(a, s, "Set loop", setLoop(s.cur(), s.sel));
        if (ImGui::MenuItem(i18n::tr("Clear loop points"), nullptr, false, s.cur().hasLoop())) applyNow(a, s, "Clear loop", setLoop(s.cur(), {}));
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Detect beats (slice markers)..."), nullptr, false, has && !busy)) openTool(s, Tool::Slice);
        if (ImGui::MenuItem(i18n::tr("Snap markers to zero crossings"), nullptr, false, !s.cur().markers.empty() && !busy))
          applyNow(a, s, "Snap markers", snapMarkersToZero(s.cur(), static_cast<int64_t>(s.rate() * 0.01)));
        if (ImGui::MenuItem(i18n::tr("Remove all markers"), nullptr, false, !s.cur().markers.empty())) applyNow(a, s, "Remove markers", clearMarkers(s.cur()));
        if (!s.cur().markers.empty() || !s.cur().regions.empty()) {
          ImGui::Separator();
          for (size_t k = 0; k < s.cur().markers.size() && k < 40; ++k) {
            const auto& m = s.cur().markers[k];
            std::string l = m.name + "  " + timeText(a, s, m.pos) + "##m" + std::to_string(k);
            if (ImGui::MenuItem(l.c_str())) s.cursor = m.pos, s.sel = {};
          }
          for (size_t k = 0; k < s.cur().regions.size() && k < 40; ++k) {
            const auto& g = s.cur().regions[k];
            std::string l = "[" + g.name + "]  " + timeText(a, s, g.start) + "##r" + std::to_string(k);
            if (ImGui::MenuItem(l.c_str())) s.sel = {g.start, g.end};
          }
        }
        break;
      case 5:  // View
        if (ImGui::MenuItem(i18n::tr("Waveform"), "W", !s.spectral)) s.spectral = false;
        if (ImGui::MenuItem(i18n::tr("Spectrogram"), "W", s.spectral)) s.spectral = true;
        if (ImGui::MenuItem(i18n::tr("Spectral brush"), "B", s.brushMode, s.spectral)) s.brushMode = !s.brushMode;
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Time as min:sec"), nullptr, !s.barsTime)) s.barsTime = false;
        if (ImGui::MenuItem(i18n::tr("Time as bar:beat (song tempo)"), nullptr, s.barsTime)) s.barsTime = true;
        if (ImGui::MenuItem(i18n::tr("Auto-scroll"), nullptr, autoScroll(a))) setAutoScroll(a, !autoScroll(a));
        ImGui::Separator();
        dockMenuItems(a, s);  // K-15: pin into the main layout / back into a window of its own
        break;
      case 6:  // Snap
        if (ImGui::MenuItem(i18n::tr("No snap"), nullptr, s.snap == 0)) s.snap = 0;
        if (ImGui::MenuItem(i18n::tr("Zero crossings"), nullptr, s.snap == 1)) s.snap = 1;
        if (ImGui::MenuItem(i18n::tr("Markers"), nullptr, s.snap == 2)) s.snap = 2;
        if (ImGui::MenuItem(i18n::tr("Beats (song tempo)"), nullptr, s.snap == 3)) s.snap = 3;
        break;
      case 7:  // Select
        if (ImGui::MenuItem(i18n::tr("All"), "Ctrl+A", false, has)) s.sel = {0, s.frames()};
        if (ImGui::MenuItem(i18n::tr("None"), nullptr, false, !s.sel.empty())) s.sel = {};
        if (ImGui::MenuItem(i18n::tr("Between markers around the cursor"), nullptr, false, has)) {
          int64_t lo = 0, hi = s.frames();
          for (const auto& m : s.cur().markers) {
            if (m.pos <= s.cursor) lo = std::max(lo, m.pos);
            if (m.pos > s.cursor) hi = std::min(hi, m.pos);
          }
          s.sel = {lo, hi};
        }
        if (ImGui::MenuItem(i18n::tr("Loop"), nullptr, false, s.cur().hasLoop())) s.sel = {s.cur().loopStart, s.cur().loopEnd};
        break;
      case 8:  // Zoom
        if (ImGui::MenuItem(i18n::tr("Zoom in"), "+")) zoomBy(s, 0.5, s.viewStart + s.viewLen * 0.5);
        if (ImGui::MenuItem(i18n::tr("Zoom out"), "-")) zoomBy(s, 2.0, s.viewStart + s.viewLen * 0.5);
        if (ImGui::MenuItem(i18n::tr("Zoom to selection"), nullptr, false, !s.sel.empty())) zoomTo(s, s.sel);
        if (ImGui::MenuItem(i18n::tr("Show all"), "0")) zoomAll(s);
        break;
      default: break;
    }
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ keys
void handleKeys(App& a, Session& s) {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
  const bool ctrl = io.KeyCtrl || io.KeySuper;
  auto key = [](ImGuiKey k) {
    if (!ImGui::IsKeyPressed(k, false)) return false;
    ImGui::SetKeyOwner(k, ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);  // the editor consumes it
    return true;
  };
  const bool busy = s.job.active();
  if (key(ImGuiKey_Space)) isPlaying(a, s) ? stopPlay(a, s) : play(a, s);
  // K-141: the Undo / Redo shortcuts (as bound) move this editor's own history while it has a
  // step to move; with none, they reach the project's history.
  if (s.hist.canUndo() && undoui::editorPressed(-1)) undo(a, s);
  if (s.hist.canRedo() && undoui::editorPressed(1)) redo(a, s);
  if (ctrl && key(ImGuiKey_A)) s.sel = {0, s.frames()};
  if (ctrl && key(ImGuiKey_C)) clipboard() = extract(s.cur().a(), target(s));
  if (!busy && ctrl && key(ImGuiKey_X) && !s.sel.empty()) {
    clipboard() = extract(s.cur().a(), s.sel);
    Range r = s.sel;
    runEdit(a, s, "Cut", [r](const Content& c) { return deleteRange(c, r); });
  }
  if (!busy && ctrl && key(ImGuiKey_V) && clipboard().frames() > 0) {
    Audio clip = clipboard();
    Range sel = s.sel;
    int64_t at = s.cursor;
    runEdit(a, s, "Paste", [clip, sel, at](const Content& c) { return sel.empty() ? insertAudio(c, at, clip) : replaceRange(c, sel, clip); });
  }
  if (!busy && ctrl && key(ImGuiKey_T) && !s.sel.empty()) {
    Range r = s.sel;
    runEdit(a, s, "Trim to selection", [r](const Content& c) { return cropTo(c, r); });
  }
  if (!busy && (key(ImGuiKey_Delete) || key(ImGuiKey_Backspace)) && !s.sel.empty()) {
    Range r = s.sel;
    runEdit(a, s, "Delete", [r](const Content& c) { return deleteRange(c, r); });
  }
  if (!ctrl && key(ImGuiKey_M) && s.frames() > 0) addMarkerAtCursor(a, s);
  if (!ctrl && key(ImGuiKey_L) && s.slot) setParam(a, s, EdLoop, param(a, s, EdLoop, 0) >= 0.5 ? 0 : 1);
  if (!ctrl && key(ImGuiKey_R)) toggleRecord(a, s);
  if (!ctrl && key(ImGuiKey_W)) s.spectral = !s.spectral;
  if (!ctrl && key(ImGuiKey_B) && s.spectral) s.brushMode = !s.brushMode;
  if (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) zoomBy(s, 0.5, s.viewStart + s.viewLen * 0.5);
  if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) zoomBy(s, 2.0, s.viewStart + s.viewLen * 0.5);
  if (key(ImGuiKey_0) || key(ImGuiKey_Keypad0)) zoomAll(s);
  if (key(ImGuiKey_Home)) s.cursor = 0, s.viewStart = 0;
  if (key(ImGuiKey_End)) s.cursor = s.frames();
  if ((!s.sel.empty() || !s.brush.empty()) && key(ImGuiKey_Escape)) {  // K-124: with nothing to clear, Esc goes on to the playlist
    s.sel = {}, s.brush.clear();
    ::gl::keys::claimEscape();
  }
}

}  // namespace gl::packs::edison
