// Gridline — "edison" pack: the waveform / spectrogram editor — time ruler with
// markers, regions and loop points, stereo lanes drawn from the peak pyramid,
// selection with snapping, scrubbing, spectral brush, overview scroller and
// the bottom bar with view options and level / pitch readouts.
#include <SDL_opengl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../core/I18n.h"
#include "../../app/Theme.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace {

struct Geo {
  ImVec2 p0, p1;  // lanes
  double start, len;
  float W() const { return p1.x - p0.x; }
  double frameAt(float x) const { return start + (x - p0.x) / std::max(1.f, W()) * len; }
  float xOf(double f) const { return p0.x + static_cast<float>((f - start) / len) * W(); }
};

constexpr double kMinHz = 30.0;

double hzAtY(const Session& s, float y, float y0, float y1) {
  const double ny = s.rate() * 0.5;
  const double t = std::clamp((y1 - y) / std::max(1.f, y1 - y0), 0.f, 1.f);
  return kMinHz * std::pow(ny / kMinHz, t);
}
float yOfHz(const Session& s, double hz, float y0, float y1) {
  const double ny = s.rate() * 0.5;
  const double t = std::log(std::max(kMinHz, hz) / kMinHz) / std::log(ny / kMinHz);
  return y1 - static_cast<float>(t) * (y1 - y0);
}

int64_t snapFrame(App& a, const Session& s, const Geo& g, double f) {
  int64_t p = std::clamp<int64_t>(static_cast<int64_t>(std::llround(f)), 0, s.frames());
  if (ImGui::GetIO().KeyShift && ImGui::GetIO().KeyAlt) return p;
  switch (s.snap) {
    case 1: return nearestZeroCrossing(s.cur().a(), p, static_cast<int64_t>(s.rate() * 0.004));
    case 2: {
      int64_t best = p;
      float bd = 8;
      for (const auto& m : s.cur().markers)
        if (std::fabs(g.xOf(static_cast<double>(m.pos)) - g.xOf(f)) < bd) bd = std::fabs(g.xOf(static_cast<double>(m.pos)) - g.xOf(f)), best = m.pos;
      return best;
    }
    case 3: {
      const double beat = s.rate() * 60.0 / std::max(10.0, a.doc.song().tempo);
      return std::clamp<int64_t>(static_cast<int64_t>(std::llround(std::round(f / beat) * beat)), 0, s.frames());
    }
    default: return p;
  }
}

// Spectrogram colour map: dark slate → green → amber → white.
ImU32 heat(uint8_t v) {
  const float t = std::clamp((v / 255.f - 0.3f) / 0.7f, 0.f, 1.f);  // -84 dB and below = background
  const float stops[5][4] = {{0.f, 0x1C, 0x21, 0x24}, {0.45f, 0x23, 0x4D, 0x3A}, {0.7f, 0x8F, 0xD3, 0xA0}, {0.88f, 0xE8, 0xC4, 0x6A}, {1.f, 0xFF, 0xFF, 0xFF}};
  for (int k = 1; k < 5; ++k)
    if (t <= stops[k][0]) {
      float u = (t - stops[k - 1][0]) / (stops[k][0] - stops[k - 1][0]);
      auto lerp = [&](int c) { return static_cast<int>(stops[k - 1][c] + (stops[k][c] - stops[k - 1][c]) * u); };
      return IM_COL32(lerp(1), lerp(2), lerp(3), 255);
    }
  return IM_COL32(255, 255, 255, 255);
}

void updateSpectrogram(Session& s) {
  const uint64_t serial = s.hist.serial();
  if (s.specJob.valid() && s.specJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    s.spec = s.specJob.get();
    s.texDirty = true;
  }
  if (s.specSerial != serial && !s.specJob.valid()) {
    s.specSerial = serial;
    AudioPtr a = s.cur().audio;
    s.specJob = std::async(std::launch::async, [a]() { return a ? computeSpectrogram(*a, 2048, 4096) : Spectrogram{}; });
  }
  if (!s.texDirty || s.spec.cols == 0) return;
  s.texDirty = false;
  // Rows on a log-frequency axis (bottom = 30 Hz, top = Nyquist).
  const int H = 256, W = s.spec.cols;
  std::vector<uint32_t> px(static_cast<size_t>(W) * H);
  const double ny = s.spec.rate * 0.5;
  for (int r = 0; r < H; ++r) {
    const double hz = kMinHz * std::pow(ny / kMinHz, static_cast<double>(H - 1 - r) / (H - 1));
    const int bin = std::clamp(static_cast<int>(hz / ny * s.spec.bins), 0, s.spec.bins - 1);
    for (int c = 0; c < W; ++c) px[static_cast<size_t>(r) * W + c] = heat(s.spec.at(c, bin));
  }
  GLint last = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
  if (!s.tex) {
    GLuint t = 0;
    glGenTextures(1, &t);
    s.tex = t;
  }
  glBindTexture(GL_TEXTURE_2D, s.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
  s.texW = W;
  s.texH = H;
}

void updateLevels(Session& s) {
  if (s.levelsJob.valid() && s.levelsJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto r = s.levelsJob.get();
    s.levels = r.first;
    s.pitch = r.second;
  }
  const Range r = target(s);
  const uint64_t key = s.hist.serial() * 1000003ull ^ static_cast<uint64_t>(r.a) * 31ull ^ static_cast<uint64_t>(r.b);
  if (key == s.levelsKey || s.dragging || s.levelsJob.valid() || s.frames() == 0) return;
  s.levelsKey = key;
  AudioPtr a = s.cur().audio;
  s.levelsJob = std::async(std::launch::async, [a, r]() {
    Levels L = measure(*a, r);
    Range pr{r.a, std::min(r.b, r.a + static_cast<int64_t>(a->rate * 2))};
    return std::make_pair(L, detectPitch(*a, pr));
  });
}

void drawLanes(App& a, Session& s, ImDrawList* dl, const Geo& g, float scale) {
  const Audio& au = s.cur().a();
  const int lanes = au.channels == 1 ? 1 : 2;
  const float laneH = (g.p1.y - g.p0.y) / lanes;
  const int W = static_cast<int>(g.W());
  const Range sel = s.sel;
  const float selX0 = sel.empty() ? 1e9f : g.xOf(static_cast<double>(sel.a)), selX1 = sel.empty() ? -1e9f : g.xOf(static_cast<double>(sel.b));
  for (int ln = 0; ln < lanes; ++ln) {
    const float y0 = g.p0.y + ln * laneH, y1 = y0 + laneH, mid = (y0 + y1) * 0.5f, half = laneH * 0.46f * s.vZoom;
    dl->AddLine(ImVec2(g.p0.x, mid), ImVec2(g.p1.x, mid), C(col::WaveGridHi), 1.f);
    for (float q : {0.5f, -0.5f}) dl->AddLine(ImVec2(g.p0.x, mid - q * half), ImVec2(g.p1.x, mid - q * half), C(col::WaveGrid), 1.f);
    if (ln > 0) dl->AddLine(ImVec2(g.p0.x, y0), ImVec2(g.p1.x, y0), C(col::BtnLo), 1.f);
    if (au.frames() == 0) continue;
    const double fpp = g.len / std::max(1, W);
    if (fpp >= 1.0) {
      for (int x = 0; x < W; ++x) {
        const double f0 = g.start + x * fpp, f1 = f0 + fpp;
        if (f1 < 0 || f0 >= static_cast<double>(au.frames())) continue;
        float mn, mx;
        s.peaks.query(au, ln, static_cast<int64_t>(f0), static_cast<int64_t>(std::ceil(f1)), mn, mx);
        const float px = g.p0.x + x + 0.5f;
        const ImU32 c = px >= selX0 && px <= selX1 ? C(col::WaveSel) : C(col::Wave);
        float ya = mid - std::clamp(mx, -1.f, 1.f) * half, yb = mid - std::clamp(mn, -1.f, 1.f) * half;
        if (yb - ya < 1.f) ya -= 0.5f, yb += 0.5f;
        dl->AddLine(ImVec2(px, std::max(y0, ya)), ImVec2(px, std::min(y1, yb)), c, 1.f);
      }
    } else {
      const int64_t i0 = std::max<int64_t>(0, static_cast<int64_t>(g.start) - 1);
      const int64_t i1 = std::min<int64_t>(au.frames(), static_cast<int64_t>(g.start + g.len) + 2);
      std::vector<ImVec2> pts;
      pts.reserve(static_cast<size_t>(i1 - i0));
      const float* x = au.ch(ln);
      for (int64_t i = i0; i < i1; ++i) pts.push_back(ImVec2(g.xOf(static_cast<double>(i)), std::clamp(mid - x[i] * half, y0, y1)));
      if (pts.size() > 1) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), C(col::Wave), 0, std::max(1.f, scale * 0.8f));
      if (fpp < 0.2)
        for (auto& p : pts) dl->AddCircleFilled(p, std::max(1.5f, scale * 1.4f), C(col::WaveSel));
    }
  }
  (void)a;
}

void drawSpectral(Session& s, ImDrawList* dl, const Geo& g) {
  updateSpectrogram(s);
  if (s.tex && s.spec.cols > 0) {
    const double total = static_cast<double>(s.spec.cols) * static_cast<double>(s.spec.hop);
    const double u0 = (g.start + s.spec.hop * 0.5) / total, u1 = (g.start + g.len + s.spec.hop * 0.5) / total;
    dl->AddImage(ImTextureRef(static_cast<ImTextureID>(s.tex)), g.p0, g.p1, ImVec2(static_cast<float>(u0), 0), ImVec2(static_cast<float>(u1), 1));
  } else {
    dl->AddText(ImVec2(g.p0.x + 10, g.p0.y + 10), C(col::TextDim), "computing spectrogram...");
  }
  // Frequency labels.
  for (double hz : {100.0, 1000.0, 10000.0}) {
    if (hz >= s.rate() * 0.5) continue;
    float y = yOfHz(s, hz, g.p0.y, g.p1.y);
    dl->AddLine(ImVec2(g.p0.x, y), ImVec2(g.p0.x + 8, y), C(col::Text, 0.6f));
    char b[16];
    std::snprintf(b, sizeof b, hz >= 1000 ? "%.0fk" : "%.0f", hz >= 1000 ? hz / 1000 : hz);
    dl->AddText(ImVec2(g.p0.x + 10, y - 7), C(col::Text, 0.7f), b);
  }
  // Brush strokes.
  for (const auto& q : s.brush) {
    ImVec2 a(g.xOf(static_cast<double>(q.f0)), yOfHz(s, q.hz1, g.p0.y, g.p1.y)), b(g.xOf(static_cast<double>(q.f1)), yOfHz(s, q.hz0, g.p0.y, g.p1.y));
    dl->AddRectFilled(a, b, C(col::Brush, 0.38f));
    dl->AddRect(a, b, C(col::Brush, 0.8f));
  }
}

}  // namespace

void drawWaveArea(App& a, Session& s, ImVec2 p0, ImVec2 p1, float sc) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* fm = a.fontMono ? a.fontMono : ImGui::GetFont();
  const float rulerH = std::round(15 * sc * 0.8f), scrollH = std::round(9 * sc * 0.8f), barH = std::round(19 * sc * 0.85f);
  const float fs = std::clamp(7.5f * sc, 9.f, 13.f);
  const ImVec2 r0 = p0, r1(p1.x, p0.y + rulerH);
  Geo g{ImVec2(p0.x, r1.y), ImVec2(p1.x, p1.y - scrollH - barH), 0, 0};
  const ImVec2 sc0(p0.x, g.p1.y), sc1(p1.x, g.p1.y + scrollH);
  const ImVec2 b0(p0.x, sc1.y), b1(p1.x, p1.y);

  if (s.peaksSerial != s.hist.serial()) {
    s.peaks.build(s.cur().a());
    s.peaksSerial = s.hist.serial();
  }
  if (s.viewLen <= 0) zoomAll(s);
  const double n = static_cast<double>(s.frames());
  // Auto-scroll with the playhead.
  const double pos = playPos(a, s);
  if (s.autoScroll && pos >= 0 && !s.dragging && (pos < s.viewStart || pos > s.viewStart + s.viewLen * 0.98))
    s.viewStart = std::clamp(pos - s.viewLen * 0.05, 0.0, std::max(0.0, n - s.viewLen));
  g.start = s.viewStart;
  g.len = std::max(1.0, s.viewLen);

  // ---- backgrounds
  dl->AddRectFilled(r0, r1, C(col::RulerBg));
  dl->AddRectFilled(g.p0, g.p1, C(col::WaveBg));
  // Selection.
  if (!s.sel.empty()) {
    float x0 = std::max(g.p0.x, g.xOf(static_cast<double>(s.sel.a))), x1 = std::min(g.p1.x, g.xOf(static_cast<double>(s.sel.b)));
    if (x1 > x0) {
      dl->AddRectFilled(ImVec2(x0, g.p0.y), ImVec2(x1, g.p1.y), C(col::SelBg));
      dl->AddRectFilled(ImVec2(x0, r0.y), ImVec2(x1, r1.y), C(col::SelBg, 0.8f));
    }
  }
  // Time grid + labels.
  {
    const double secPerPx = g.len / s.rate() / std::max(1.f, g.W());
    double step;
    if (s.barsTime) {
      const double beat = 60.0 / std::max(10.0, a.doc.song().tempo);
      step = beat;
      while (step / secPerPx < 70) step *= 2;
    } else {
      static const double kSteps[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
      step = 600;
      for (double st : kSteps)
        if (st / secPerPx >= 70) {
          step = st;
          break;
        }
    }
    const double t0 = g.start / s.rate(), t1 = (g.start + g.len) / s.rate();
    dl->PushClipRect(r0, g.p1, true);
    for (double t = std::floor(t0 / step) * step; t <= t1; t += step) {
      float x = g.xOf(t * s.rate());
      dl->AddLine(ImVec2(x, r1.y - rulerH * 0.35f), ImVec2(x, r1.y), C(col::RulerText, 0.7f));
      dl->AddLine(ImVec2(x, g.p0.y), ImVec2(x, g.p1.y), C(col::WaveGrid), 1.f);
      std::string lbl = timeText(a, s, static_cast<int64_t>(std::llround(t * s.rate())), step < 0.1);
      dl->AddText(fm, fs, ImVec2(x + 3, r0.y + 1), C(col::RulerText), lbl.c_str());
    }
    dl->PopClipRect();
  }

  // ---- lanes
  dl->PushClipRect(g.p0, g.p1, true);
  if (s.spectral) {
    drawSpectral(s, dl, g);
    if (!s.sel.empty())
      dl->AddRectFilled(ImVec2(g.xOf(static_cast<double>(s.sel.a)), g.p0.y), ImVec2(g.xOf(static_cast<double>(s.sel.b)), g.p1.y), C(col::SelEdge, 0.18f));
  } else {
    drawLanes(a, s, dl, g, sc);
  }
  if (s.frames() == 0 && !s.job.active()) {
    const char* msg = s.slot ? "Empty. Record (R) from the mixer input, load a file (File menu) or paste audio."
                             : "No audio in this channel's sample.";
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2((g.p0.x + g.p1.x - ts.x) * 0.5f, (g.p0.y + g.p1.y - ts.y) * 0.5f), C(col::TextDim), msg);
  }
  // Regions (band in the ruler + tint), loop, markers, cursor, playhead.
  const Content& c = s.cur();
  dl->PopClipRect();
  dl->PushClipRect(r0, g.p1, true);
  for (const auto& rg : c.regions) {
    float x0 = g.xOf(static_cast<double>(rg.start)), x1 = g.xOf(static_cast<double>(rg.end));
    dl->AddRectFilled(ImVec2(x0, r1.y - 4), ImVec2(x1, r1.y), C(col::Region, 0.9f));
    dl->AddRectFilled(ImVec2(x0, g.p0.y), ImVec2(x1, g.p1.y), C(col::Region, 0.07f));
    dl->AddText(fm, fs, ImVec2(x0 + 3, g.p1.y - fs - 3), C(col::Region), rg.name.c_str());
  }
  if (c.hasLoop()) {
    float x0 = g.xOf(static_cast<double>(c.loopStart)), x1 = g.xOf(static_cast<double>(c.loopEnd));
    dl->AddRectFilled(ImVec2(x0, r0.y), ImVec2(x1, r0.y + 3), C(col::Loop));
    for (float x : {x0, x1}) dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Loop, 0.8f), 1.f);
  }
  int hoverMarker = -1;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  for (size_t k = 0; k < c.markers.size(); ++k) {
    const auto& m = c.markers[k];
    float x = g.xOf(static_cast<double>(m.pos));
    if (x < g.p0.x - 50 || x > g.p1.x) continue;
    const bool hot = std::fabs(mouse.x - x) < 5 && mouse.y >= r0.y && mouse.y < r1.y;
    if (hot) hoverMarker = static_cast<int>(k);
    dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Marker, hot || s.dragMarker == static_cast<int>(k) ? 1.f : 0.75f), 1.f);
    dl->AddTriangleFilled(ImVec2(x, r0.y), ImVec2(x + rulerH * 0.55f, r0.y + rulerH * 0.3f), ImVec2(x, r0.y + rulerH * 0.6f), C(col::Marker));
    dl->AddText(fm, fs, ImVec2(x + 3, g.p0.y + 2), C(col::Marker), m.name.c_str());
  }
  if (s.cursor >= g.start && s.cursor <= g.start + g.len) {
    float x = g.xOf(static_cast<double>(s.cursor));
    dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Cursor, 0.75f), 1.f);
  }
  if (pos >= 0) {
    float x = g.xOf(pos);
    dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Playhead), std::max(1.5f, sc));
  }
  dl->PopClipRect();

  // ---- interaction: ruler
  ImGui::SetCursorScreenPos(r0);
  ImGui::InvisibleButton("ruler", ImVec2(std::max(1.f, r1.x - r0.x), std::max(1.f, r1.y - r0.y)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool busy = s.job.active();
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (hoverMarker >= 0) {
      s.dragMarker = hoverMarker;
    } else {
      s.cursor = snapFrame(a, s, g, g.frameAt(mouse.x));
      s.sel = {};
      if (isPlaying(a, s)) play(a, s);
    }
  }
  if (s.dragMarker >= 0 && ImGui::IsItemActive() && static_cast<size_t>(s.dragMarker) < c.markers.size()) {
    float x = std::clamp(mouse.x, g.p0.x, g.p1.x);
    dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Marker), 2.f);
  }
  if (s.dragMarker >= 0 && ImGui::IsItemDeactivated()) {
    if (static_cast<size_t>(s.dragMarker) < c.markers.size()) {
      int64_t to = snapFrame(a, s, g, g.frameAt(std::clamp(mouse.x, g.p0.x, g.p1.x)));
      if (to != c.markers[static_cast<size_t>(s.dragMarker)].pos) applyNow(a, s, "Move marker", moveMarker(c, static_cast<size_t>(s.dragMarker), to));
    }
    s.dragMarker = -1;
  }
  if (hoverMarker >= 0 && ImGui::IsItemHovered()) {
    a.setHint("Marker: drag to move, double-click to rename, right-click for options");
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      s.renameMarker = hoverMarker;
      s.renameRegion = -1;
      std::snprintf(s.renameBuf, sizeof s.renameBuf, "%s", c.markers[static_cast<size_t>(hoverMarker)].name.c_str());
      openTool(s, Tool::Rename);
    }
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hoverMarker >= 0) {
    s.renameMarker = hoverMarker;
    ImGui::OpenPopup("markermenu");
  }
  if (ImGui::BeginPopup("markermenu")) {
    if (s.renameMarker >= 0 && static_cast<size_t>(s.renameMarker) < c.markers.size()) {
      ImGui::TextDisabled("%s", c.markers[static_cast<size_t>(s.renameMarker)].name.c_str());
      if (ImGui::MenuItem(i18n::tr("Rename..."))) {
        std::snprintf(s.renameBuf, sizeof s.renameBuf, "%s", c.markers[static_cast<size_t>(s.renameMarker)].name.c_str());
        s.renameRegion = -1;
        openTool(s, Tool::Rename);
      }
      if (ImGui::MenuItem(i18n::tr("Snap to zero crossing"))) {
        const auto& m = c.markers[static_cast<size_t>(s.renameMarker)];
        applyNow(a, s, "Snap marker", moveMarker(c, static_cast<size_t>(s.renameMarker), nearestZeroCrossing(c.a(), m.pos, static_cast<int64_t>(s.rate() * 0.01))));
      }
      if (ImGui::MenuItem(i18n::tr("Select to next marker"))) {
        const int64_t from = c.markers[static_cast<size_t>(s.renameMarker)].pos;
        const int64_t to = static_cast<size_t>(s.renameMarker) + 1 < c.markers.size() ? c.markers[static_cast<size_t>(s.renameMarker) + 1].pos : s.frames();
        s.sel = {from, to};
      }
      if (ImGui::MenuItem(i18n::tr("Remove"))) applyNow(a, s, "Remove marker", removeMarker(c, static_cast<size_t>(s.renameMarker)));
    }
    ImGui::EndPopup();
  }

  // ---- interaction: lanes
  ImGui::SetCursorScreenPos(g.p0);
  ImGui::InvisibleButton("lanes", ImVec2(std::max(1.f, g.W()), std::max(1.f, g.p1.y - g.p0.y)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  const double fm0 = std::clamp(g.frameAt(mouse.x), 0.0, n);
  EdisonFx* fx = slotFx(a, s);
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && s.frames() > 0) {
    if (s.spectral && s.brushMode) {
      s.dragging = true;
    } else if ((s.scrubMode || io.KeyAlt) && fx) {
      s.scrubbing = true;
      fx->scrub(true, fm0);
    } else {
      const int64_t f = snapFrame(a, s, g, fm0);
      if (io.KeyShift && !s.sel.empty()) {
        s.anchor = std::llabs(f - s.sel.a) > std::llabs(f - s.sel.b) ? s.sel.a : s.sel.b;
      } else {
        s.anchor = f;
        s.cursor = f;
        s.sel = {};
      }
      s.dragging = true;
    }
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (s.scrubbing && fx) {
      fx->scrub(true, fm0);
    } else if (s.dragging && s.spectral && s.brushMode) {
      // Paint: one cell per ~1/3 octave × 20 ms around the mouse.
      const double hz = hzAtY(s, mouse.y, g.p0.y, g.p1.y);
      const int64_t w = static_cast<int64_t>(std::max(s.rate() * 0.02, g.len / g.W() * 6));
      SpectralRect q{static_cast<int64_t>(fm0) - w / 2, static_cast<int64_t>(fm0) + w / 2, hz / 1.12, hz * 1.12};
      if (s.brush.empty() || std::llabs(s.brush.back().f0 - q.f0) > w / 3 || std::fabs(std::log(s.brush.back().hz0 / q.hz0)) > 0.04) s.brush.push_back(q);
    } else if (s.dragging) {
      const int64_t f = snapFrame(a, s, g, fm0);
      s.sel = {std::min(s.anchor, f), std::max(s.anchor, f)};
      // Drag past the edges scrolls.
      if (mouse.x > g.p1.x) s.viewStart = std::min(std::max(0.0, n - s.viewLen), s.viewStart + s.viewLen * 0.02);
      if (mouse.x < g.p0.x) s.viewStart = std::max(0.0, s.viewStart - s.viewLen * 0.02);
    }
  }
  if (ImGui::IsItemDeactivated()) {
    if (s.scrubbing && fx) fx->scrub(false, fm0);
    if (s.dragging && !(s.spectral && s.brushMode) && s.sel.len() < 2) s.sel = {};
    s.scrubbing = false;
    s.dragging = false;
  }
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !(s.spectral && s.brushMode)) {
    // Double-click: the region under the mouse, else the span between markers.
    const int64_t f = static_cast<int64_t>(fm0);
    Range r{0, s.frames()};
    bool found = false;
    for (const auto& rg : c.regions)
      if (f >= rg.start && f < rg.end) r = {rg.start, rg.end}, found = true;
    if (!found)
      for (const auto& m : c.markers) {
        if (m.pos <= f) r.a = std::max(r.a, m.pos);
        if (m.pos > f) r.b = std::min(r.b, m.pos);
      }
    s.sel = r;
  }
  if (hovered) {
    if (io.MouseWheel != 0) {
      if (io.KeyCtrl || io.KeySuper)
        s.vZoom = std::clamp(s.vZoom * (io.MouseWheel > 0 ? 1.25f : 0.8f), 0.25f, 16.f);
      else if (io.KeyShift)
        s.viewStart = std::clamp(s.viewStart - io.MouseWheel * s.viewLen * 0.1, 0.0, std::max(0.0, n - s.viewLen));
      else
        zoomBy(s, io.MouseWheel > 0 ? 0.8 : 1.25, fm0);
    }
    if (io.MouseWheelH != 0) s.viewStart = std::clamp(s.viewStart + io.MouseWheelH * s.viewLen * 0.1, 0.0, std::max(0.0, n - s.viewLen));
    char b[96];
    if (s.spectral)
      std::snprintf(b, sizeof b, "%s  %.0f Hz%s", timeText(a, s, static_cast<int64_t>(fm0)).c_str(), hzAtY(s, mouse.y, g.p0.y, g.p1.y),
                    s.brushMode ? "  - brush: paint the area to edit, Tools > Effects > Apply spectral brush" : "");
    else
      std::snprintf(b, sizeof b, "%s  - drag to select, Shift extends, Alt scrubs, wheel zooms, right-click for edits", timeText(a, s, static_cast<int64_t>(fm0)).c_str());
    a.setHint(b);
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("lanemenu");
  if (ImGui::BeginPopup("lanemenu")) {
    const bool has = s.frames() > 0, sel = !s.sel.empty();
    const Range tr = target(s);
    if (ImGui::MenuItem(i18n::tr("Cut"), "Ctrl+X", false, sel && !busy)) {
      clipboard() = extract(c.a(), s.sel);
      Range r = s.sel;
      runEdit(a, s, "Cut", [r](const Content& cc) { return deleteRange(cc, r); });
    }
    if (ImGui::MenuItem(i18n::tr("Copy"), "Ctrl+C", false, has)) clipboard() = extract(c.a(), tr);
    if (ImGui::MenuItem(i18n::tr("Paste"), "Ctrl+V", false, clipboard().frames() > 0 && !busy)) {
      Audio clip = clipboard();
      Range sl = s.sel;
      int64_t at = s.cursor;
      runEdit(a, s, "Paste", [clip, sl, at](const Content& cc) { return sl.empty() ? insertAudio(cc, at, clip) : replaceRange(cc, sl, clip); });
    }
    if (ImGui::MenuItem(i18n::tr("Delete"), "Del", false, sel && !busy)) {
      Range r = s.sel;
      runEdit(a, s, "Delete", [r](const Content& cc) { return deleteRange(cc, r); });
    }
    if (ImGui::MenuItem(i18n::tr("Trim to selection"), "Ctrl+T", false, sel && !busy)) {
      Range r = s.sel;
      runEdit(a, s, "Trim to selection", [r](const Content& cc) { return cropTo(cc, r); });
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Fade in"), nullptr, false, has && !busy)) runEdit(a, s, "Fade in", [tr, k = static_cast<Curve>(s.fadeCurve)](const Content& cc) { return fadeIn(cc, tr, k); });
    if (ImGui::MenuItem(i18n::tr("Fade out"), nullptr, false, has && !busy)) runEdit(a, s, "Fade out", [tr, k = static_cast<Curve>(s.fadeCurve)](const Content& cc) { return fadeOut(cc, tr, k); });
    if (ImGui::MenuItem(i18n::tr("Normalize"), nullptr, false, has && !busy)) runEdit(a, s, "Normalize", [tr](const Content& cc) { return normalize(cc, tr, 0); });
    if (ImGui::MenuItem(i18n::tr("Reverse"), nullptr, false, has && !busy)) runEdit(a, s, "Reverse", [tr](const Content& cc) { return reverse(cc, tr); });
    ImGui::Separator();
    if (ImGui::MenuItem(sel ? i18n::tr("Add region") : i18n::tr("Add marker"), "M", false, has)) addMarkerAtCursor(a, s);
    if (ImGui::MenuItem(i18n::tr("Set loop from selection"), nullptr, false, sel)) applyNow(a, s, "Set loop", setLoop(c, s.sel));
    if (ImGui::MenuItem(i18n::tr("Export selection..."), nullptr, false, has)) exportSelection(a, s);
    if (s.spectral && !s.brush.empty()) {
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Apply spectral brush..."))) openTool(s, Tool::Spectral);
      if (ImGui::MenuItem(i18n::tr("Clear brush"))) s.brush.clear();
    }
    ImGui::EndPopup();
  }

  // ---- overview scroller
  dl->AddRectFilled(sc0, sc1, C(col::RulerBg));
  if (n > 0) {
    float x0 = sc0.x + static_cast<float>(s.viewStart / n) * (sc1.x - sc0.x);
    float x1 = sc0.x + static_cast<float>(std::min(1.0, (s.viewStart + s.viewLen) / n)) * (sc1.x - sc0.x);
    dl->AddRectFilled(ImVec2(x0, sc0.y + 1), ImVec2(std::max(x0 + 4, x1), sc1.y - 1), C(col::BtnHi), 2);
    if (pos >= 0) {
      float px = sc0.x + static_cast<float>(pos / n) * (sc1.x - sc0.x);
      dl->AddLine(ImVec2(px, sc0.y), ImVec2(px, sc1.y), C(col::Playhead), 1.f);
    }
  }
  ImGui::SetCursorScreenPos(sc0);
  ImGui::InvisibleButton("overview", ImVec2(std::max(1.f, sc1.x - sc0.x), std::max(1.f, sc1.y - sc0.y)));
  if (ImGui::IsItemActive() && n > 0) {
    double f = (mouse.x - sc0.x) / std::max(1.f, sc1.x - sc0.x) * n;
    s.viewStart = std::clamp(f - s.viewLen * 0.5, 0.0, std::max(0.0, n - s.viewLen));
  }
  if (ImGui::IsItemHovered()) a.setHint("Overview: drag to scroll");

  // ---- bottom bar
  dl->AddRectFilled(b0, b1, C(col::Panel));
  dl->AddLine(b0, ImVec2(b1.x, b0.y), C(col::BtnLo));
  float x = b0.x + 4;
  const float bh = b1.y - b0.y - 4, by = b0.y + 2;
  auto chip = [&](const char* id, const char* label, bool on, const char* hint) {
    ImVec2 ts = fm->CalcTextSizeA(fs, FLT_MAX, 0, label);
    const float w = ts.x + 14;
    ImGui::SetCursorScreenPos(ImVec2(x, by));
    ImGui::InvisibleButton(id, ImVec2(w, bh));
    const bool hv = ImGui::IsItemHovered();
    dl->AddRectFilled(ImVec2(x, by), ImVec2(x + w, by + bh), on ? C(col::Lit, 0.85f) : hv ? C(col::BtnHi) : C(col::Btn), 3);
    dl->AddText(fm, fs, ImVec2(x + 7, by + (bh - ts.y) * 0.5f), on ? C(col::Lcd) : C(col::Text), label);
    if (hv && hint) a.setHint(hint);
    x += w + 4;
    return ImGui::IsItemClicked();
  };
  if (chip("v_wave", "WAVE", !s.spectral, "Waveform view (W)")) s.spectral = false;
  if (chip("v_spec", "SPECTRUM", s.spectral, "Spectrogram view (W)")) s.spectral = true;
  if (s.spectral && chip("v_brush", "BRUSH", s.brushMode, "Spectral brush: paint time/frequency areas, then apply a gain to them (B)")) s.brushMode = !s.brushMode;
  if (s.spectral && !s.brush.empty() && chip("v_apply", "APPLY...", false, "Apply a gain to the painted areas")) openTool(s, Tool::Spectral);
  x += 6;
  static const char* kSnap[] = {"SNAP OFF", "SNAP ZERO", "SNAP MARK", "SNAP BEAT"};
  if (chip("v_snap", kSnap[s.snap], s.snap != 0, "Snap: off / zero crossings / markers / beats (Shift+Alt while dragging: no snap)")) s.snap = (s.snap + 1) % 4;
  if (chip("v_time", s.barsTime ? "BAR:BEAT" : "MIN:SEC", false, "Time display")) s.barsTime = !s.barsTime;
  x += 6;
  if (chip("z_out", "-", false, "Zoom out (-)")) zoomBy(s, 2.0, s.viewStart + s.viewLen * 0.5);
  if (chip("z_in", "+", false, "Zoom in (+)")) zoomBy(s, 0.5, s.viewStart + s.viewLen * 0.5);
  if (chip("z_sel", "SEL", false, "Zoom to the selection")) zoomTo(s, s.sel);
  if (chip("z_all", "ALL", false, "Show everything (0)")) zoomAll(s);
  // Readouts (right).
  updateLevels(s);
  char rd[200];
  if (!s.status.empty() && ImGui::GetTime() < s.statusUntil) {
    std::snprintf(rd, sizeof rd, "%s", s.status.c_str());
  } else if (s.frames() == 0) {
    rd[0] = 0;
  } else {
    auto db = [](double v) { return v < -150 ? -INFINITY : v; };
    std::string pitch = s.pitch.hz > 0 && s.pitch.confidence > 0.5 ? noteName(s.pitch.note) + (s.pitch.cents >= 0 ? " +" : " ") + std::to_string(static_cast<int>(std::lround(s.pitch.cents))) + "c" : "-";
    std::snprintf(rd, sizeof rd, "%s  PEAK %.1f  RMS %.1f  LUFS %.1f  TP %.1f  PITCH %s", s.sel.empty() ? "ALL" : "SEL", db(s.levels.peakDb),
                  db(s.levels.rmsDb), db(s.levels.lufs), db(s.levels.truePeakDb), pitch.c_str());
  }
  ImVec2 ts = fm->CalcTextSizeA(fs, FLT_MAX, 0, rd);
  const float rx = std::max(x + 8, b1.x - ts.x - 8);
  dl->PushClipRect(ImVec2(x, b0.y), b1, true);
  dl->AddText(fm, fs, ImVec2(rx, b0.y + (b1.y - b0.y - ts.y) * 0.5f), C(col::LcdText), rd);
  dl->PopClipRect();
}

}  // namespace gl::packs::edison
