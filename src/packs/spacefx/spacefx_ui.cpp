// Gridline — "spacefx" pack: editors. Layouts follow the grouping of the
// classic counterparts (graph / tap display / IR view on top, grouped
// controls below) drawn in Gridline's monochrome style.
#include <complex>
#include <filesystem>
#include <map>
#include <vector>
#include "../PacksUi.h"
#include "../../engine/Dsp.h"
#include "../../engine/Program.h"
#include "sfx_common.h"
#include "sfx_skin.h"
#include "sfx_widgets.h"
#include "sfx_flcurves.h"
#include "sfx_mbd.h"
#include "spacefx.h"
#include "../../core/Platform.h"

namespace gl::ed {
double svfResponseDb(int mode, double cutoff, double reso01, double hz, double sr);  // app/PluginEditorsEq.cpp
}

namespace gl::packs::spacefx {

namespace {

using namespace ui2;
using theme::pal;
namespace fs = std::filesystem;
constexpr double kPiD = 3.14159265358979323846;

// ============================================================== Convolver
struct ConvUi {
  bool db = true;
  bool eqTab = false;  // impulse view / equalizer view
  const Sample* cached = nullptr;
  int cachedW = 0;
  std::vector<float> colL, colR;  // per-column peaks
  int drag = -1;
  int envDrag = -1, eqDrag = -1;  // dragged envelope / EQ point
  const Sample* specOf = nullptr;
  std::vector<float> specDb;  // IR magnitude (dB) on 240 log bins, 20 Hz .. 20 kHz
};
std::map<uint64_t, ConvUi> convUi;

void openIrDialog(App& a, const PluginTarget& t, const std::string& cur) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = "Load impulse response";
  const char* h = gl::platform::homeEnv();
  a.fileDialog.dir = !cur.empty() ? fs::path(cur).parent_path().string() : (h ? h : ".");
  a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3"};
  PluginTarget tt = t;
  a.fileDialog.onOk = [&a, tt](const std::string& p) {
    if (const PluginState* st = tt.state(a.doc.song())) {
      PluginState ns = *st;
      ConvData cd = parseConvData(ns.data);
      cd.path = p;
      ns.data = formatConvData(cd);
      ns.params[CvSource] = SrcFile;
      setPluginState(a, tt, ns, "load impulse");
    }
  };
}

void irView(App& a, const PluginTarget& t, const PluginState& st, float width, float height) {
  const auto& P = pal();
  ConvUi& u = convUi[t.key()];
  IrView v = convolverIr(st);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##irview", ImVec2(width, height));
  ImVec2 p1(p.x + width, p.y + height);
  graphBg(dl, p, p1);
  // Drop an audio file onto the view to load it.
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
      std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (isAudioFile(path)) {
        PluginState ns = st;
        ConvData cd = parseConvData(ns.data);
        cd.path = path;
        ns.data = formatConvData(cd);
        ns.params[CvSource] = SrcFile;
        setPluginState(a, t, ns, "load impulse");
      }
    }
    ImGui::EndDragDropTarget();
  }
  if (!v.ir) {
    const char* msg = v.error.empty() ? "No impulse" : v.error.c_str();
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (height - ts.y) * 0.5f), P.textDim, msg);
    smallText(a, dl, ImVec2(p.x + 10, p1.y - 18), P.textFaint, "Drop an audio file here or press Load");
    return;
  }
  const Sample& ir = *v.ir;
  const double rate = ir.sampleRate, len = static_cast<double>(ir.frames()) / rate;
  // Time axis spans the longest length the current trim allows.
  double avail = std::max(len, std::min(60.0, (v.rawSeconds - val(st, CvStart) * 0.001) * val(st, CvStretch)));
  avail = std::max(avail, 0.02);
  const float x0 = p.x + 10, x1 = p1.x - 10, w = x1 - x0;
  auto X = [&](double sec) { return x0 + static_cast<float>(sec / avail) * w; };
  // Ruler.
  double step = avail > 6 ? 1.0 : avail > 3 ? 0.5 : avail > 1.2 ? 0.25 : avail > 0.5 ? 0.1 : avail > 0.2 ? 0.05 : 0.01;
  for (double tt = 0; tt <= avail + 1e-9; tt += step) {
    float x = X(tt);
    dl->AddLine(ImVec2(x, p.y + 18), ImVec2(x, p1.y - 6), P.gridStep);
    char b[24];
    if (avail < 1.2)
      std::snprintf(b, sizeof b, "%.0f ms", tt * 1000);
    else
      std::snprintf(b, sizeof b, "%.2g s", tt);
    smallText(a, dl, ImVec2(x + 3, p.y + 5), P.textFaint, b);
  }
  // Column peaks (cached per IR / width).
  int cols = static_cast<int>(w);
  int used = std::max(1, static_cast<int>(w * len / avail));
  if (u.cached != v.ir.get() || u.cachedW != cols) {
    u.cached = v.ir.get();
    u.cachedW = cols;
    u.colL.assign(static_cast<size_t>(used), 0.f);
    u.colR.assign(static_cast<size_t>(used), 0.f);
    size_t n = ir.frames();
    for (int c = 0; c < used; ++c) {
      size_t a0 = static_cast<size_t>(static_cast<double>(c) / used * n), a1 = std::max(a0 + 1, static_cast<size_t>(static_cast<double>(c + 1) / used * n));
      float ml = 0, mr = 0;
      for (size_t i = a0; i < a1 && i < n; ++i) {
        ml = std::max(ml, std::fabs(ir.left[i]));
        mr = std::max(mr, std::fabs(ir.right[i]));
      }
      u.colL[static_cast<size_t>(c)] = ml;
      u.colR[static_cast<size_t>(c)] = mr;
    }
  }
  float gmax = 1e-9f;
  for (size_t c = 0; c < u.colL.size(); ++c) gmax = std::max({gmax, u.colL[c], u.colR[c]});
  const float top = p.y + 22, bot = p1.y - 8, laneH = (bot - top - 6) * 0.5f;
  for (int lane = 0; lane < 2; ++lane) {
    const std::vector<float>& col = lane ? u.colR : u.colL;
    float base = top + laneH * (lane + 1) + lane * 6;
    dl->AddLine(ImVec2(x0, base), ImVec2(x1, base), P.gridBar);
    smallText(a, dl, ImVec2(x1 - 10, base - laneH + 2), P.textFaint, lane ? "R" : "L");
    for (size_t c = 0; c < col.size(); ++c) {
      double n = col[c] / gmax;
      double hgt = u.db ? std::clamp((20 * std::log10(std::max(1e-6, n)) + 60) / 60, 0.0, 1.0) : n;
      float x = x0 + static_cast<float>(c);
      dl->AddLine(ImVec2(x, base), ImVec2(x, base - static_cast<float>(hgt) * laneH), theme::withAlpha(P.text, 0.55f));
    }
  }
  // Region beyond the length.
  if (X(len) < x1) dl->AddRectFilled(ImVec2(X(len), top - 4), ImVec2(x1, bot), theme::withAlpha(P.surface800, 0.6f));
  // Envelope overlay (fade in, fade out, tighten) on a 0..1 scale.
  double fin = val(st, CvFadeIn) * 0.001, fout = val(st, CvFadeOut) * len, tight = val(st, CvTighten) * 9.0;
  std::vector<ImVec2> env;
  for (int i = 0; i <= 160; ++i) {
    double tt = len * i / 160.0, g = 1.0;
    if (fin > 1e-4 && tt < fin) g *= 0.5 - 0.5 * std::cos(kPiD * tt / fin);
    if (fout > 1e-4 && len - tt < fout) g *= 0.5 - 0.5 * std::cos(kPiD * (len - tt) / fout);
    if (tight > 0) g *= std::exp(-tight * tt / len);
    env.push_back(ImVec2(X(tt), bot - static_cast<float>(g) * (bot - top)));
  }
  dl->AddPolyline(env.data(), static_cast<int>(env.size()), P.paper, 0, 1.6f);
  // Tempo-synced length marker.
  if (val(st, CvSync) >= 0.5f) {
    double sec = kConvSyncBeats[std::clamp(static_cast<int>(val(st, CvSyncLen) + 0.5f), 0, 6)] * 60.0 / a.doc.song().tempo;
    if (sec < avail) {
      float x = X(sec);
      for (float y = top; y < bot; y += 6) dl->AddLine(ImVec2(x, y), ImVec2(x, y + 3), P.accent, 1.5f);
      smallText(a, dl, ImVec2(x + 4, bot - 14), P.accent, "sync");
    }
  }
  // Handles: 0 fade-in end, 1 fade-out start, 2 length.
  ImVec2 hs[3] = {ImVec2(X(std::min(fin, len)), top), ImVec2(X(len - fout), top), ImVec2(X(len), top + (bot - top) * 0.5f)};
  ImGuiIO& io = ImGui::GetIO();
  int hover = -1;
  for (int i = 0; i < 3; ++i)
    if (std::fabs(io.MousePos.x - hs[i].x) < 8 && io.MousePos.y > p.y && io.MousePos.y < p1.y &&
        (i == 2 || std::fabs(io.MousePos.y - hs[i].y) < 12))
      hover = i;
  if (ImGui::IsItemActivated()) u.drag = hover;
  if (ImGui::IsItemActive() && u.drag >= 0) {
    double tt = std::clamp((io.MousePos.x - x0) / w, 0.0f, 1.0f) * avail;
    if (u.drag == 0) dragParam(a, t, CvFadeIn, std::clamp(tt * 1000.0, 0.0, 500.0));
    if (u.drag == 1) dragParam(a, t, CvFadeOut, std::clamp((len - tt) / len, 0.0, 1.0));
    if (u.drag == 2) dragParam(a, t, CvLength, std::clamp(tt, 0.02, 60.0));
  }
  if (ImGui::IsItemDeactivated()) {
    u.drag = -1;
    a.doc.endGesture();
  }
  static const char* names[3] = {"Fade in", "Fade out", "Length"};
  for (int i = 0; i < 3; ++i) {
    bool hot = hover == i || u.drag == i;
    if (i == 2) {
      dl->AddLine(ImVec2(hs[i].x, top - 4), ImVec2(hs[i].x, bot), hot ? P.paper : P.textDim, hot ? 2.f : 1.f);
      dl->AddRectFilled(ImVec2(hs[i].x - 4, hs[i].y - 10), ImVec2(hs[i].x + 4, hs[i].y + 10), hot ? P.paper : P.text, 2);
    } else {
      dl->AddRectFilled(ImVec2(hs[i].x - 5, hs[i].y - 5), ImVec2(hs[i].x + 5, hs[i].y + 5), hot ? P.paper : P.text, 2);
    }
    if (hot) a.setHint(std::string(names[i]) + "   (drag to edit the impulse envelope)");
  }
  char info[96];
  std::snprintf(info, sizeof info, "%.2f s · %.0f Hz · %s", len, rate, v.paths == 4 ? "true stereo" : "stereo");
  ImVec2 ts = ImGui::CalcTextSize(info);
  smallText(a, dl, ImVec2(x1 - ts.x * 0.85f - 6, p1.y - 18), P.textDim, info);
  // IR amplitude envelope (0.5 = unity): double-click adds a point, drag moves, right-click deletes.
  if (val(st, CvEnvOn) >= 0.5f && u.drag < 0) {
    ConvData cd = parseConvData(st.data);
    auto Y = [&](double vv) { return bot - static_cast<float>(vv) * (bot - top); };
    auto tOf = [&](float x) { return std::clamp((x - x0) / w, 0.0f, 1.0f) * avail; };
    auto vOf = [&](float y) { return std::clamp((bot - y) / (bot - top), 0.0f, 1.0f); };
    std::vector<ImVec2> line;
    for (int i = 0; i <= 200; ++i) {
      double tt = avail * i / 200.0;
      line.push_back(ImVec2(X(tt), Y(cd.env.empty() ? 0.5 : envAt(cd.env, tt))));
    }
    dl->AddPolyline(line.data(), static_cast<int>(line.size()), P.accent, 0, 1.8f);
    dl->AddLine(ImVec2(x0, Y(0.5)), ImVec2(x1, Y(0.5)), theme::withAlpha(P.accent, 0.25f));
    int hot = -1;
    for (size_t i = 0; i < cd.env.size(); ++i)
      if (std::hypot(io.MousePos.x - X(cd.env[i].t), io.MousePos.y - Y(cd.env[i].v)) < 7) hot = static_cast<int>(i);
    bool changed = false;
    if (ImGui::IsItemActivated() && hot >= 0) u.envDrag = hot;
    if (ImGui::IsItemActive() && u.envDrag >= 0 && u.envDrag < static_cast<int>(cd.env.size())) {
      EnvPoint& q = cd.env[static_cast<size_t>(u.envDrag)];
      double lo = u.envDrag > 0 ? cd.env[static_cast<size_t>(u.envDrag - 1)].t : 0.0;
      double hi = u.envDrag + 1 < static_cast<int>(cd.env.size()) ? cd.env[static_cast<size_t>(u.envDrag + 1)].t : avail;
      q.t = std::clamp(static_cast<double>(tOf(io.MousePos.x)), lo, hi);
      q.v = vOf(io.MousePos.y);
      changed = true;
    }
    if (ImGui::IsItemDeactivated()) u.envDrag = -1;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot < 0) {
      if (cd.env.empty()) cd.env = {EnvPoint{0, 0.5, 0}, EnvPoint{avail, 0.5, 0}};
      EnvPoint q;
      q.t = tOf(io.MousePos.x);
      q.v = vOf(io.MousePos.y);
      cd.env.insert(std::upper_bound(cd.env.begin(), cd.env.end(), q.t, [](double x, const EnvPoint& e) { return x < e.t; }), q);
      changed = true;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hot >= 0) {
      cd.env.erase(cd.env.begin() + hot);
      changed = true;
    }
    for (size_t i = 0; i < cd.env.size(); ++i) {
      bool h = static_cast<int>(i) == hot || static_cast<int>(i) == u.envDrag;
      dl->AddCircleFilled(ImVec2(X(cd.env[i].t), Y(cd.env[i].v)), h ? 5.5f : 4.f, h ? P.paper : P.accent);
    }
    if (hot >= 0 || ImGui::IsItemHovered())
      a.setHint("IR envelope: double-click adds a point, drag moves it, right-click deletes (middle line = unity)");
    if (changed) {
      PluginState ns = st;
      ns.data = formatConvData(cd);
      setPluginState(a, t, ns, "edit impulse envelope", gestureKey(t, 0xC0E1));
    }
  }
}

// EQ curve applied to the impulse (log 20 Hz .. 20 kHz, ±18 dB) over the IR's spectrum.
void eqView(App& a, const PluginTarget& t, const PluginState& st, float width, float height) {
  const auto& P = pal();
  ConvUi& u = convUi[t.key()];
  IrView v = convolverIr(st);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##eqview", ImVec2(width, height));
  ImVec2 p1(p.x + width, p.y + height);
  graphBg(dl, p, p1);
  const float x0 = p.x + 34, x1 = p1.x - 10, w = x1 - x0, top = p.y + 12, bot = p1.y - 20;
  auto Xx = [&](double x) { return x0 + static_cast<float>(x) * w; };
  auto Yv = [&](double vv) { return bot - static_cast<float>(vv) * (bot - top); };
  for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
    float x = Xx(std::log(f / 20.0) / std::log(1000.0));
    dl->AddLine(ImVec2(x, top), ImVec2(x, bot), P.gridStep);
    char b[16];
    std::snprintf(b, sizeof b, f >= 1000 ? "%gk" : "%g", f >= 1000 ? f / 1000 : f);
    smallText(a, dl, ImVec2(x + 2, bot + 4), P.textFaint, b);
  }
  for (int db : {-18, -9, 0, 9, 18}) {
    float y = Yv(0.5 + db / 36.0);
    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), db == 0 ? P.gridBar : P.gridStep);
    char b[16];
    std::snprintf(b, sizeof b, "%+d", db);
    smallText(a, dl, ImVec2(p.x + 6, y - 6), P.textFaint, b);
  }
  // IR magnitude spectrum (cached per IR).
  if (v.ir && u.specOf != v.ir.get()) {
    u.specOf = v.ir.get();
    constexpr int N = 16384;
    detail::Fft f;
    f.init(N);
    std::vector<float> buf(2 * N, 0.f);
    const Sample& ir = *v.ir;
    for (size_t i = 0; i < ir.frames() && i < static_cast<size_t>(N); ++i) buf[2 * i] = 0.5f * (ir.left[i] + ir.right[i]);
    f.run(buf.data(), false);
    u.specDb.assign(240, -120.f);
    for (int b = 0; b < 240; ++b) {
      double fa = convEqHz(b / 240.0), fb = convEqHz((b + 1) / 240.0);
      int ka = std::clamp(static_cast<int>(fa * N / ir.sampleRate), 1, N / 2), kb = std::clamp(static_cast<int>(fb * N / ir.sampleRate), ka + 1, N / 2 + 1);
      double e = 0;
      for (int k = ka; k < kb; ++k) e += static_cast<double>(buf[2 * k]) * buf[2 * k] + static_cast<double>(buf[2 * k + 1]) * buf[2 * k + 1];
      u.specDb[static_cast<size_t>(b)] = static_cast<float>(10 * std::log10(std::max(1e-20, e / (kb - ka))));
    }
  }
  if (!u.specDb.empty()) {
    float mx = *std::max_element(u.specDb.begin(), u.specDb.end());
    std::vector<ImVec2> sp;
    for (size_t b = 0; b < u.specDb.size(); ++b) {
      double n = std::clamp((u.specDb[b] - mx + 60.0) / 60.0, 0.0, 1.0);
      sp.push_back(ImVec2(Xx((b + 0.5) / u.specDb.size()), bot - static_cast<float>(n) * (bot - top)));
    }
    for (size_t i = 0; i + 1 < sp.size(); ++i) {
      ImVec2 q[4] = {sp[i], sp[i + 1], ImVec2(sp[i + 1].x, bot), ImVec2(sp[i].x, bot)};
      dl->AddConvexPolyFilled(q, 4, theme::withAlpha(P.text, 0.08f));
    }
  }
  // Curve + points: double-click adds, drag moves, right-click deletes.
  ConvData cd = parseConvData(st.data);
  ImGuiIO& io = ImGui::GetIO();
  std::vector<ImVec2> line;
  for (int i = 0; i <= 240; ++i) {
    double x = i / 240.0;
    line.push_back(ImVec2(Xx(x), Yv(cd.eq.empty() ? 0.5 : envAt(cd.eq, x))));
  }
  dl->AddPolyline(line.data(), static_cast<int>(line.size()), theme::withAlpha(P.accent, 0.4f + 0.6f * val(st, CvEqMix)), 0, 2.0f);
  int hot = -1;
  for (size_t i = 0; i < cd.eq.size(); ++i)
    if (std::hypot(io.MousePos.x - Xx(cd.eq[i].t), io.MousePos.y - Yv(cd.eq[i].v)) < 7) hot = static_cast<int>(i);
  bool changed = false;
  auto xOf = [&](float x) { return std::clamp((x - x0) / w, 0.0f, 1.0f); };
  auto vOf = [&](float y) { return std::clamp((bot - y) / (bot - top), 0.0f, 1.0f); };
  if (ImGui::IsItemActivated()) u.eqDrag = hot;
  if (ImGui::IsItemActive() && u.eqDrag >= 0 && u.eqDrag < static_cast<int>(cd.eq.size())) {
    EnvPoint& q = cd.eq[static_cast<size_t>(u.eqDrag)];
    double lo = u.eqDrag > 0 ? cd.eq[static_cast<size_t>(u.eqDrag - 1)].t : 0.0;
    double hi = u.eqDrag + 1 < static_cast<int>(cd.eq.size()) ? cd.eq[static_cast<size_t>(u.eqDrag + 1)].t : 1.0;
    q.t = std::clamp(static_cast<double>(xOf(io.MousePos.x)), lo, hi);
    q.v = vOf(io.MousePos.y);
    changed = true;
  }
  if (ImGui::IsItemDeactivated()) u.eqDrag = -1;
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot < 0) {
    if (cd.eq.empty()) cd.eq = {EnvPoint{0, 0.5, 0}, EnvPoint{1, 0.5, 0}};
    EnvPoint q;
    q.t = xOf(io.MousePos.x);
    q.v = vOf(io.MousePos.y);
    cd.eq.insert(std::upper_bound(cd.eq.begin(), cd.eq.end(), q.t, [](double x, const EnvPoint& e) { return x < e.t; }), q);
    changed = true;
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hot >= 0) {
    cd.eq.erase(cd.eq.begin() + hot);
    changed = true;
  }
  for (size_t i = 0; i < cd.eq.size(); ++i) {
    bool h = static_cast<int>(i) == hot || static_cast<int>(i) == u.eqDrag;
    dl->AddCircleFilled(ImVec2(Xx(cd.eq[i].t), Yv(cd.eq[i].v)), h ? 5.5f : 4.f, h ? P.paper : P.accent);
  }
  if (hot >= 0) {
    char b[64];
    std::snprintf(b, sizeof b, "%.0f Hz  %+.1f dB", convEqHz(cd.eq[static_cast<size_t>(hot)].t), (cd.eq[static_cast<size_t>(hot)].v - 0.5) * 36.0);
    a.setHint(b);
  } else if (ImGui::IsItemHovered()) {
    a.setHint("IR equalizer: double-click adds a point, drag moves it, right-click deletes; EQ MIX blends it in");
  }
  if (changed) {
    PluginState ns = st;
    ns.data = formatConvData(cd);
    setPluginState(a, t, ns, "edit impulse EQ", gestureKey(t, 0xC0E2));
  }
  smallText(a, dl, ImVec2(x1 - 150, top + 2), P.textDim, cd.eq.empty() ? "flat - double-click to add points" : "EQ curve");
}

// Reference convolver panel 714 × 346: mix / width / delay / processing wheels
// across the top, impulse tools in the middle row, the impulse view underneath.
const skin::Skin& convSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2A2F38, 0x323842, 0x545B63, 0x72D703, 0x40484E, 0xACB4BA);
    s.pillBg = skin::hex(0x3A414B);
    s.lcdText = skin::hex(0x9BE84A);
    s.select = skin::hex(0x49B5F5);
    s.accent2 = skin::hex(0x4FB4D0);
    return s;
  }();
  return k;
}

void editorConvolver(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const PluginState st = *t.state(a.doc.song());
  ConvUi& u = convUi[t.key()];
  const skin::Skin& K = convSkin();
  constexpr float kW = 714, kH = 346;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const ImU32 teal = skin::hex(0x33C18C), green = skin::hex(0x72D703), orange = skin::hex(0xF8AD10), blue = skin::hex(0x4FB4D0);
  // Top row: mix, separations, pre-delay, self-convolution, stretch, EQ mix (+ generated-space shape).
  W(a, t, CvDry, F, 15, 7, 48, K, "DRY", skin::WheelBigLine, teal);
  W(a, t, CvInWidth, F, 90, 16, 39, K, "IN WIDTH", skin::WheelBigLine, green);
  W(a, t, CvWet, F, 135, 7, 48, K, "WET", skin::WheelBigLine, green);
  W(a, t, CvWidth, F, 189, 16, 39, K, "WIDTH", skin::WheelBigLine, green);
  W(a, t, CvPreDelay, F, 255, 7, 48, K, "DELAY", skin::WheelBigLine, orange);
  W(a, t, CvSelfConv, F, 313, 7, 48, K, "SELF", skin::WheelBigLine, orange);
  W(a, t, CvStretch, F, 371, 7, 48, K, "STRETCH", skin::WheelBigLine, orange);
  W(a, t, CvEqMix, F, 429, 7, 48, K, "EQ MIX", skin::WheelBigLine, orange);
  const bool file = static_cast<int>(val(st, CvSource) + 0.5f) == SrcFile;
  W(a, t, CvSize, F, 494, 16, 39, K, "SIZE", skin::WheelBigLine, blue);
  W(a, t, CvTone, F, 545, 16, 39, K, "TONE", skin::WheelBigLine, blue);
  if (file) dim(dl, F, 490, 12, 98, 56, K);
  chk(a, t, CvNormalize, F, 600, 5, 100, 15, K, "NORMALIZE");
  sel(a, t, CvStereo, F, 600, 23, 106, 44, K);
  // Middle row: impulse source + file tools, trim / fades, IR cuts, view tabs and options.
  dl->AddLine(F.p(5, 72), F.p(709, 72), skin::darken(K.panel, 0.35f), 1.0f);
  text(dl, F, 6, 76, "IMPULSE", K.textDim);
  menu(a, t, CvSource, F, 5, 88, 110, 17, K);
  if (btn("##loadir", F, 5, 110, 54, 26, K, "LOAD", false)) openIrDialog(a, t, parseConvData(st.data).path);
  if (ImGui::IsItemHovered()) a.setHint("Load a WAV / FLAC / WavPack impulse response (or drop a file on the view)");
  if (btn("##irdb", F, 61, 110, 54, 26, K, u.db ? "dB" : "LIN", u.db)) u.db = !u.db;
  if (ImGui::IsItemHovered()) a.setHint("Impulse display: dB or linear amplitude");
  box(dl, F, 124, 76, 236, 66, K, nullptr);
  W(a, t, CvStart, F, 132, 82, 34, K, "START");
  W(a, t, CvLength, F, 177, 82, 34, K, "LENGTH");
  W(a, t, CvFadeIn, F, 222, 82, 34, K, "FADE IN");
  W(a, t, CvFadeOut, F, 267, 82, 34, K, "FADE OUT");
  W(a, t, CvTighten, F, 312, 82, 34, K, "TIGHTEN");
  box(dl, F, 366, 76, 100, 66, K, nullptr);
  W(a, t, CvLowCut, F, 374, 82, 34, K, "LOW CUT", skin::WheelBigLine, blue);
  W(a, t, CvHighCut, F, 422, 82, 34, K, "HI CUT", skin::WheelBigLine, blue);
  if (btn("##tabir", F, 476, 76, 114, 20, K, "IMPULSE", !u.eqTab)) u.eqTab = false;
  if (btn("##tabeq", F, 592, 76, 114, 20, K, "EQUALIZER", u.eqTab)) u.eqTab = true;
  chk(a, t, CvEnvOn, F, 476, 101, 76, 16, K, "ENVELOPE");
  if (ImGui::IsItemHovered()) a.setHint("Apply the impulse amplitude envelope (edit it on the impulse view)");
  chk(a, t, CvReverse, F, 556, 101, 68, 16, K, "REVERSE");
  menu(a, t, CvStretchMode, F, 628, 100, 78, 17, K);
  if (ImGui::IsItemHovered()) a.setHint("Stretch: resample (pitch follows) or keep the pitch");
  chk(a, t, CvSync, F, 476, 123, 100, 16, K, "TEMPO LENGTH");
  if (val(st, CvSync) >= 0.5f) menu(a, t, CvSyncLen, F, 580, 122, 126, 17, K);
  // Info line.
  {
    std::string path = parseConvData(st.data).path;
    std::string info = file ? (path.empty() ? std::string("No file - drop one on the view") : fs::path(path).filename().string())
                            : std::string("Generated space - SIZE and TONE shape it");
    text(dl, F, 6, 152, info.c_str(), K.text, 8.0f);
    if (val(st, CvLatency) > 0) {
      char b[48];
      std::snprintf(b, sizeof b, "reported latency %d smp", static_cast<int>(val(st, CvLatency)));
      text(dl, F, 560, 152, b, K.textDim, 7.5f);
    }
  }
  at(F, 0, 171);
  if (u.eqTab)
    eqView(a, t, st, F.u(kW), F.u(kH - 171));
  else
    irView(a, t, st, F.u(kW), F.u(kH - 171));
  F.end(kW, kH);
}

// ============================================================ Multi Delay
struct MdUi {
  int sel = 0;
  int drag = -1;
};
std::map<uint64_t, MdUi> mdUi;

bool mdRef(const PluginState& st) { return val(st, MdModel) >= 0.5f; }
// Whether a tap's time is tempo-based (reference: per tap; legacy: global switch).
bool tapTempo(const PluginState& st, int k) { return mdRef(st) ? val(st, mdExt(k, MdxTempo)) >= 0.5f : val(st, MdSync) >= 0.5f; }
double tapSeconds(const PluginState& st, int k, double tempo) {
  return tapTempo(st, k) ? val(st, mdParam(k, MdSteps)) * 15.0 / tempo : val(st, mdParam(k, MdTimeMs)) * 0.001;
}

void mdGraph(App& a, const PluginTarget& t, const PluginState& st, MdUi& u, float width, float height) {
  const auto& P = pal();
  const double tempo = a.doc.song().tempo;
  const bool ref = mdRef(st);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##mdgraph", ImVec2(width, height));
  ImVec2 p1(p.x + width, p.y + height);
  graphBg(dl, p, p1);
  double tmax = 0.25;
  bool anyTempo = false;
  for (int k = 0; k < kTaps; ++k)
    if (val(st, mdParam(k, MdOn)) >= 0.5f) {
      tmax = std::max(tmax, tapSeconds(st, k, tempo));
      anyTempo = anyTempo || tapTempo(st, k);
    }
  const bool sync = ref ? anyTempo : val(st, MdSync) >= 0.5f;
  double span = std::clamp(tmax * 3.2, 0.5, 8.0);
  if (sync) span = std::ceil(span * tempo / 60.0) * 60.0 / tempo;
  const float x0 = p.x + 24, x1 = p1.x - 10, w = x1 - x0;
  auto X = [&](double sec) { return x0 + static_cast<float>(sec / span) * w; };
  const float laneH = (height - 30) / 2;
  // Grid.
  if (sync) {
    double stepS = 15.0 / tempo;
    int steps = static_cast<int>(span / stepS);
    for (int i = 0; i <= steps; ++i) {
      float x = X(i * stepS);
      dl->AddLine(ImVec2(x, p.y + 16), ImVec2(x, p1.y - 6), i % 4 == 0 ? P.gridBeat : P.gridStep);
      if (i % 4 == 0 && steps <= 64) {
        char b[32];
        std::snprintf(b, sizeof b, "%d", i / 4 + 1);
        smallText(a, dl, ImVec2(x + 3, p.y + 3), P.textFaint, b);
      }
    }
  } else {
    double st2 = span > 4 ? 0.5 : span > 1.5 ? 0.25 : 0.1;
    for (double tt = 0; tt <= span + 1e-9; tt += st2) {
      float x = X(tt);
      dl->AddLine(ImVec2(x, p.y + 16), ImVec2(x, p1.y - 6), P.gridStep);
      char b[16];
      std::snprintf(b, sizeof b, "%.0f ms", tt * 1000);
      smallText(a, dl, ImVec2(x + 3, p.y + 3), P.textFaint, b);
    }
  }
  for (int lane = 0; lane < 2; ++lane) {
    float base = p.y + 18 + laneH * (lane + 1) + lane * 4;
    dl->AddLine(ImVec2(x0, base), ImVec2(x1, base), P.gridBar);
    smallText(a, dl, ImVec2(p.x + 8, base - 14), P.textFaint, lane ? "R" : "L");
  }
  auto stem = [&](int lane, double sec, double amp, ImU32 col, float wd) {
    if (sec > span || amp < 0.004) return;
    float base = p.y + 18 + laneH * (lane + 1) + lane * 4;
    float x = X(sec), h = static_cast<float>(std::min(1.0, amp)) * (laneH - 4);
    dl->AddRectFilled(ImVec2(x - wd, base - h), ImVec2(x + wd, base), col, 1.5f);
  };
  ImVec2 handle[kTaps];
  bool has[kTaps] = {};
  for (int pass = 0; pass < 2; ++pass)
    for (int k = 0; k < kTaps; ++k) {
      if (val(st, mdParam(k, MdOn)) < 0.5f) continue;
      bool sel = k == u.sel;
      if ((pass == 1) != sel) continue;  // selected tap drawn on top
      ImU32 col = sel ? P.paper : theme::withAlpha(P.text, 0.45f);
      double sec = tapSeconds(st, k, tempo), off = val(st, mdParam(k, MdOffset)) * 0.001;
      double out = std::fabs(val(st, mdParam(k, MdOutLevel))), fb = std::fabs(val(st, mdParam(k, MdFeedback)) * val(st, MdFbScale));
      double pan = val(st, mdParam(k, MdOutPan));
      double gl = pan > 0 ? 1 - pan : 1, gr = pan < 0 ? 1 + pan : 1;
      int mode = static_cast<int>(val(st, mdParam(k, MdFbMode)) + 0.5f);
      bool alternate = mode == 1 || (ref && mode == 2);
      double amp = out;
      if (ref && mode == MfbOff) {
        stem(0, 0, out * gl, col, sel ? 2.f : 1.5f);
        stem(1, 0, out * gr, col, sel ? 2.f : 1.5f);
        amp = 0;
      } else if (ref && val(st, mdExt(k, MdxLeaveFirst)) < 0.5f) {
        amp *= fb;  // the first pass is processed too
      }
      for (int e = 1; e < 64 && amp >= 0.004; ++e) {
        double tl = e * sec + std::max(0.0, -off), tr = e * sec + std::max(0.0, off);
        if (std::min(tl, tr) > span) break;
        if (alternate) {
          if (e % 2)
            stem(0, tl, amp * gl, col, sel ? 2.f : 1.5f);
          else
            stem(1, tr, amp * gr, col, sel ? 2.f : 1.5f);
        } else {
          stem(0, tl, amp * gl, col, sel ? 2.f : 1.5f);
          stem(1, tr, amp * gr, col, sel ? 2.f : 1.5f);
        }
        amp *= fb;
      }
      handle[k] = ImVec2(X(sec), p.y + 18 + laneH - static_cast<float>(std::min(1.0, out * gl)) * (laneH - 4));
      has[k] = sec <= span;
    }
  ImGuiIO& io = ImGui::GetIO();
  int hover = -1;
  for (int k = 0; k < kTaps; ++k)
    if (has[k] && std::hypot(io.MousePos.x - handle[k].x, io.MousePos.y - handle[k].y) < 10) hover = (hover < 0 || k == u.sel) ? k : hover;
  if (ImGui::IsItemActivated() && hover >= 0) {
    u.drag = hover;
    u.sel = hover;
  }
  if (ImGui::IsItemActive() && u.drag >= 0) {
    double sec = std::clamp((io.MousePos.x - x0) / w, 0.0f, 1.0f) * span;
    if (tapTempo(st, u.drag)) {
      double steps = std::max(0.25, std::round(sec * tempo / 15.0 * 4) / 4);
      dragParam(a, t, mdParam(u.drag, MdSteps), std::min(64.0, steps));
    } else {
      dragParam(a, t, mdParam(u.drag, MdTimeMs), std::clamp(sec * 1000, 1.0, 5000.0));
    }
    double lvl = std::clamp((p.y + 18 + laneH - io.MousePos.y) / (laneH - 4), 0.0f, 1.25f);
    dragParam(a, t, mdParam(u.drag, MdOutLevel), lvl);
  }
  if (ImGui::IsItemDeactivated()) {
    u.drag = -1;
    a.doc.endGesture();
  }
  for (int k = 0; k < kTaps; ++k) {
    if (!has[k]) continue;
    bool hot = hover == k || u.drag == k;
    ImU32 c = k == u.sel ? P.paper : P.textDim;
    dl->AddCircleFilled(handle[k], hot ? 8.f : 7.f, P.surface950);
    dl->AddCircle(handle[k], hot ? 8.f : 7.f, hot ? P.accent : c, 0, 1.5f);
    char b[4];
    std::snprintf(b, sizeof b, "%d", k + 1);
    ImGui::PushFont(a.fontMono, 10.0f);
    ImVec2 ts = ImGui::CalcTextSize(b);
    dl->AddText(ImVec2(handle[k].x - ts.x * 0.5f, handle[k].y - ts.y * 0.5f), c, b);
    ImGui::PopFont();
  }
  if (hover >= 0) {
    char b[96];
    std::snprintf(b, sizeof b, "Tap %d: %.0f ms   (drag: time ↔, output level ↕)", hover + 1, tapSeconds(st, hover, tempo) * 1000);
    a.setHint(b);
  }
}

// Reference delay-bank panel 580 × 280: tap selector with enable boxes and
// the global wheels on top; IN / FILTER / FEEDBACK / FB FILTER / GRAIN / OUT
// sections for the selected tap below. Gridline adds the tap timeline and a
// CHARACTER section (modulation, diffusion) underneath (to y 392).
const skin::Skin& mdSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x828A93, 0x6F7981, 0x8C949B, 0xFFFFFF, 0x3F484D, 0xD5D9DB);
    s.fillPressed = skin::hex(0xFFA830);
    s.pillBg = skin::hex(0x565F68);
    s.pillText = skin::hex(0xB9BFC3);
    s.textDim = skin::hex(0xD5D9DB);
    s.handle = skin::hex(0x35373C);
    s.select = skin::hex(0xFFA830);
    s.lcdBg = skin::hex(0x454E57);
    s.lcdText = skin::hex(0xC9D3DC);
    s.graphBg = skin::hex(0x3A4148);
    s.graphGrid = skin::hex(0x4A535B);
    s.graphGridStrong = skin::hex(0x5D6770);
    s.graphText = skin::hex(0xB9BFC3);
    s.accent2 = skin::hex(0xFFA830);
    return s;
  }();
  return k;
}

// Filter response sketch for a tap filter. Legacy: kFilterTypes order (off,
// low, high, band) on the SVF; reference: the eight-type cascaded biquads.
void tapFilterVisual(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& K, bool ref, int type, double cut, double res, double gainA,
                     int stages) {
  skin::graphWell(dl, p0, p1, K);
  if (type <= 0) {
    skin::label(dl, (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f - 6, "OFF", K.graphText, 10.0f);
    return;
  }
  const int mode = type == 1 ? 0 : type == 2 ? 2 : 1;
  const flc::Bq q = flc::mdDesign(type, cut, res, gainA, stages, 48000);
  ImVec2 pts[48];
  const float w = p1.x - p0.x - 4, h = p1.y - p0.y - 6;
  for (int i = 0; i < 48; ++i) {
    double hz = 20.0 * std::pow(1000.0, i / 47.0);
    double db = ref ? flc::bqMagDb(q, stages, hz, 48000) : ed::svfResponseDb(mode, cut, res, hz, 48000);
    db = std::clamp(db, -30.0, 12.0);
    pts[i] = ImVec2(p0.x + 2 + w * i / 47.0f, p0.y + 3 + static_cast<float>((12 - db) / 42.0) * h);
  }
  dl->AddPolyline(pts, 48, K.fillPressed, 0, 1.5f);
}

// Cascade cells ×1 / ×2 / ×3 bound to a stage-count parameter (1..3).
void stageCells(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& K) {
  using namespace flskin;
  const int cur = static_cast<int>(std::lround(pval(a, t, param)));
  static const char* lb[3] = {"1", "2", "3"};
  for (int i = 0; i < 3; ++i) {
    ImGui::PushID(param * 4 + i);
    float cw = w / 3.0f;
    if (btn("##st", F, x + i * cw, y, cw - 1, h, K, lb[i], cur == i + 1)) a.doc.setParam(t.pid(param), i + 1, false);
    if (ImGui::IsItemHovered()) a.setHint("Filter stages (identical cascaded sections)");
    ImGui::PopID();
  }
}

void editorMultiDelay(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const PluginState st = *t.state(a.doc.song());
  MdUi& u = mdUi[t.key()];
  u.sel = std::clamp(u.sel, 0, kTaps - 1);
  const int k = u.sel;
  auto P = [&](int f) { return mdParam(k, f); };
  auto E = [&](int e) { return mdExt(k, e); };
  const bool ref = mdRef(st);
  const skin::Skin& K = mdSkin();
  constexpr float kW = 580, kH = 392;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const ImU32 orange = skin::hex(0xFFA830);
  // Tap selector: enable boxes over numbered tabs (click selects, right-click solos).
  for (int i = 0; i < kTaps; ++i) {
    ImGui::PushID(i);
    chk(a, t, mdParam(i, MdOn), F, 13 + i * 28, 9, 18, 18, K, nullptr, orange);
    char nb[4];
    std::snprintf(nb, sizeof nb, "%d", i + 1);
    if (btn("##tab", F, 8 + i * 28, 34, 26, 26, K, nb, i == k)) u.sel = i;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      PluginState ns = st;
      for (int j = 0; j < kTaps; ++j) ns.params[static_cast<size_t>(mdParam(j, MdOn))] = j == i ? 1.f : 0.f;
      setPluginState(a, t, ns, "solo delay tap");
      u.sel = i;
    }
    if (ImGui::IsItemHovered()) a.setHint("Edit this tap · right-click to solo it");
    // Routing mark: this tap feeds the next one.
    if (i < kTaps - 1 && std::fabs(val(st, mdParam(i, MdToNext))) > 1e-4f)
      dl->AddTriangleFilled(F.p(33 + i * 28, 15), F.p(37 + i * 28, 18), F.p(33 + i * 28, 21), orange);
    ImGui::PopID();
  }
  {
    static const char* models[2] = {"LEGACY", "REF"};
    sel(a, t, MdModel, F, 238, 8, 70, 18, K, true, models);
    if (ImGui::IsItemHovered()) a.setHint("Delay model: legacy (older projects) or the reference bank behaviour");
  }
  if (ref)
    chk(a, t, MdOversample, F, 238, 36, 82, 20, K, "OVERSAMPLE", orange);
  else
    chk(a, t, MdSync, F, 238, 36, 90, 20, K, "TEMPO SYNC", orange);
  cap(dl, F, 417, 8, "DRY", K.textDim);
  cap(dl, F, 462, 8, "WET", K.textDim);
  cap(dl, F, 507, 8, "IN", K.textDim);
  cap(dl, F, 551, 8, "FB", K.textDim);
  W(a, t, MdDry, F, 397, 21, 40, K, nullptr);
  W(a, t, MdWet, F, 442, 21, 40, K, nullptr);
  W(a, t, MdInput, F, 487, 21, 40, K, nullptr);
  W(a, t, MdFbScale, F, 531, 21, 40, K, nullptr);
  dl->AddRectFilled(F.p(8, 63), F.p(572, 65), skin::hex(0x353E47));
  // Sections for the selected tap.
  box(dl, F, 8, 69, 57, 205, K, "IN", 18);
  box(dl, F, 69, 69, 107, 205, K, "FILTER", 18);
  box(dl, F, 180, 69, 150, 205, K, "FEEDBACK", 18);
  box(dl, F, 334, 69, 107, 205, K, "FB FILTER", 18);
  box(dl, F, 445, 69, 66, 205, K, "GRAIN", 18);
  box(dl, F, 515, 69, 57, 205, K, "OUT", 18);
  {
    char tb[16];
    std::snprintf(tb, sizeof tb, "TAP %d", k + 1);
    cap(dl, F, 36, 100, tb, skin::hex(0xFFA830), 8.0f);
  }
  W(a, t, P(MdInPan), F, 14, 157, 44, K, "PAN");
  W(a, t, P(MdInLevel), F, 14, 216, 44, K, "VOL", skin::WheelBigLine | skin::WheelCentered);
  static const char* shortTypes[8] = {"OFF", "LP", "HP", "BP", "NOT", "LS", "PK", "HS"};
  auto ival = [&](int prm) { return static_cast<int>(std::lround(val(st, prm))); };
  if (ref) {
    // FILTER: type / cascade, response, POST, gain; cut / res.
    skin::digi(a, t, P(MdInFType), F.p(75, 97), F.sz(46, 17), K, shortTypes[std::clamp(ival(P(MdInFType)), 0, 7)], F.font(8.0f));
    stageCells(a, t, E(MdxInStages), F, 123, 97, 47, 17, K);
    tapFilterVisual(dl, F.p(76, 120), F.p(169, 152), K, true, ival(P(MdInFType)), val(st, P(MdInCut)), val(st, P(MdInRes)), val(st, E(MdxInGain)),
                    ival(E(MdxInStages)));
    chk(a, t, E(MdxInPost), F, 78, 169, 44, 14, K, "POST", orange);
    W(a, t, E(MdxInGain), F, 126, 157, 40, K, "GAIN");
    W(a, t, P(MdInCut), F, 75, 216, 44, K, "CUT");
    W(a, t, P(MdInRes), F, 126, 216, 44, K, "RES");
    // FEEDBACK: leave-first and tempo buttons, mode cells.
    chk(a, t, E(MdxLeaveFirst), F, 186, 91, 34, 14, K, "1ST", orange);
    if (ImGui::IsItemHovered()) a.setHint("Leave the first echo unprocessed (feedback filter and volume act from the second pass)");
    chk(a, t, E(MdxTempo), F, 186, 109, 34, 14, K, "BPM", orange);
    if (ImGui::IsItemHovered()) a.setHint("Time in steps (tempo) or in milliseconds");
    static const char* modeNames[4] = {"OFF", "NORM", "INV", "P.PONG"};
    static const int modeVals[4] = {MfbOff, MfbNormal, MfbInvert, MfbPingPong};
    const int curMode = ival(P(MdFbMode));
    for (int i = 0; i < 4; ++i) {
      ImGui::PushID(900 + i);
      if (btn("##fm", F, 222 + (i % 2) * 53, 90 + (i / 2) * 18, 51, 16, K, modeNames[i], curMode == modeVals[i]))
        a.doc.setParam(t.pid(P(MdFbMode)), modeVals[i], false);
      ImGui::PopID();
    }
  } else {
    menu(a, t, P(MdInFType), F, 75, 97, 95, 17, K);
    tapFilterVisual(dl, F.p(76, 122), F.p(169, 200), K, false, std::min(3, ival(P(MdInFType))), val(st, P(MdInCut)), val(st, P(MdInRes)), 1, 1);
    W(a, t, P(MdInCut), F, 75, 216, 44, K, "CUT");
    W(a, t, P(MdInRes), F, 126, 216, 44, K, "RES");
    static const char* modes[4] = {"NORM", "PING", "INV", "OFF"};
    sel(a, t, P(MdFbMode), F, 186, 94, 138, 22, K, true, modes);
  }
  dl->AddLine(F.p(198, 131), F.p(258, 131), skin::hex(0x424B54), 1.0f);
  const int timeP = tapTempo(st, k) ? P(MdSteps) : P(MdTimeMs);
  V(a, t, timeP, F, 185, 138, 27, 115, K, "TIME", orange);
  V(a, t, P(MdOffset), F, 214, 138, 28, 115, K, "OFS", orange);
  V(a, t, P(MdSep), F, 244, 138, 28, 115, K, "SEP", orange);
  W(a, t, P(MdFbPan), F, 280, 157, 44, K, "PAN");
  W(a, t, P(MdFeedback), F, 280, 216, 44, K, "VOL", ref ? skin::WheelBigLine | skin::WheelCentered : skin::WheelBigLine);
  {
    std::string tv = pfmt(a, t, timeP);
    skin::valueStrip(dl, F.p(280, 133), F.p(326, 147), K, tv.c_str(), F.font(7.5f));
  }
  if (ref) {
    skin::digi(a, t, P(MdFbFType), F.p(340, 97), F.sz(46, 17), K, shortTypes[std::clamp(ival(P(MdFbFType)), 0, 7)], F.font(8.0f));
    stageCells(a, t, E(MdxFbStages), F, 388, 97, 47, 17, K);
    tapFilterVisual(dl, F.p(341, 120), F.p(434, 152), K, true, ival(P(MdFbFType)), val(st, P(MdFbCut)), val(st, P(MdFbRes)), val(st, E(MdxFbGain)),
                    ival(E(MdxFbStages)));
    W(a, t, E(MdxFbGain), F, 391, 157, 40, K, "GAIN");
  } else {
    menu(a, t, P(MdFbFType), F, 340, 97, 95, 17, K);
    tapFilterVisual(dl, F.p(341, 122), F.p(434, 200), K, false, std::min(3, ival(P(MdFbFType))), val(st, P(MdFbCut)), val(st, P(MdFbRes)), 1, 1);
  }
  W(a, t, P(MdFbCut), F, 340, 216, 44, K, "CUT");
  W(a, t, P(MdFbRes), F, 391, 216, 44, K, "RES");
  V(a, t, P(MdGateDiv), F, 452, 110, 25, 143, K, "DIV", orange);
  V(a, t, P(MdGateShape), F, 480, 110, 25, 143, K, "SH", orange);
  W(a, t, P(MdToNext), F, 521, 95, 44, K, "NEXT", ref ? skin::WheelBigLine | skin::WheelCentered : skin::WheelBigLine);
  W(a, t, P(MdOutPan), F, 521, 157, 44, K, "PAN");
  W(a, t, P(MdOutLevel), F, 521, 216, 44, K, "VOL", ref ? skin::WheelBigLine | skin::WheelCentered : skin::WheelBigLine);
  // Timeline of every tap (drag: time / output level) + character.
  at(F, 8, 282);
  mdGraph(a, t, st, u, F.u(426), F.u(104));
  box(dl, F, 440, 282, 132, 104, K, "CHARACTER", 18);
  W(a, t, P(MdModRate), F, 446, 310, 36, K, "RATE");
  W(a, t, P(MdModDepth), F, 488, 310, 36, K, "DEPTH");
  W(a, t, P(MdDiffuse), F, 530, 310, 36, K, "DIFF");
  F.end(kW, kH);
}

// ============================================================= Band Delay
struct BdUi {
  int target = 0, bank = 0;
  int lastIdx = -1;
  float lastVal = 0;
};
std::map<uint64_t, BdUi> bdUi;

void setCurves(App& a, const PluginTarget& t, const BandCurves& c, const char* label, uint64_t merge = 0) {
  PluginState ns = *t.state(a.doc.song());
  ns.data = formatBandCurves(c);
  setPluginState(a, t, ns, label, merge);
}

void bdShape(BandCurves& c, int bank, int target, int shape) {
  static uint32_t seed = 0x2468ACEu;
  for (int i = 0; i < kBdPoints; ++i) {
    double u = static_cast<double>(i) / (kBdPoints - 1), v = 0.5;
    switch (shape) {
      case 0: v = target == BtLevel ? 1.0 : 0.5; break;
      case 1: v = 0.1 + 0.85 * u; break;
      case 2: v = 0.95 - 0.85 * u; break;
      case 3: v = 0.5 + 0.4 * std::sin(u * 6 * kPiD); break;
      case 4:
        seed = seed * 1664525u + 1013904223u;
        v = 0.1 + 0.85 * ((seed >> 8) & 0xFFFF) / 65535.0;
        break;
      default: break;
    }
    c.v[bank][target][i] = static_cast<float>(std::clamp(v, 0.0, 1.0));
  }
}

void bdGraph(App& a, const PluginTarget& t, const PluginState& st, BdUi& u, float width, float height) {
  const auto& P = pal();
  BandCurves c = parseBandCurves(st.data);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##bdgraph", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  ImVec2 p1(p.x + width, p.y + height);
  graphBg(dl, p, p1);
  const float gx0 = p.x + 34, gx1 = p1.x - 8, gw = gx1 - gx0, top = p.y + 10, bot = p1.y - 20, gh = bot - top;
  // Output spectrum behind the bars.
  {
    float db[96];
    pui::spectrum(t.tap(a), t.key() ^ 0xBD, 96, db, a.engine.sampleRate());
    std::vector<ImVec2> sp;
    for (int i = 0; i < 96; ++i) {
      float x = gx0 + gw * i / 95.0f;
      float y = bot - std::clamp((db[i] + 90.0f) / 90.0f, 0.0f, 1.0f) * gh;
      sp.push_back(ImVec2(x, y));
    }
    for (size_t i = 0; i + 1 < sp.size(); ++i) {
      ImVec2 q[4] = {sp[i], sp[i + 1], ImVec2(sp[i + 1].x, bot), ImVec2(sp[i].x, bot)};
      dl->AddConvexPolyFilled(q, 4, theme::withAlpha(P.text, 0.05f));
    }
  }
  // Frequency labels.
  for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
    float x = pui::freqToX(f, gx0, gw);
    dl->AddLine(ImVec2(x, top), ImVec2(x, bot), P.gridStep);
    char b[16];
    if (f >= 1000)
      std::snprintf(b, sizeof b, "%gk", f / 1000);
    else
      std::snprintf(b, sizeof b, "%g", f);
    smallText(a, dl, ImVec2(x + 2, bot + 4), P.textFaint, b);
  }
  // Value axis labels.
  double maxT = val(st, BdMaxTime);
  if (val(st, BdSync) >= 0.5f) maxT = kBdSyncBeats[std::clamp(static_cast<int>(val(st, BdSyncTime) + 0.5f), 0, 7)] * 60.0 / a.doc.song().tempo;
  auto label = [&](double v) -> std::string {
    char b[24];
    switch (u.target) {
      case BtDelay: std::snprintf(b, sizeof b, "%.0f ms", v * maxT * 1000); break;
      case BtPan:
        if (std::fabs(v - 0.5) < 0.01) return "C";
        std::snprintf(b, sizeof b, "%.0f%s", std::fabs(v - 0.5) * 200, v < 0.5 ? "L" : "R");
        break;
      default: std::snprintf(b, sizeof b, "%.0f%%", v * 100); break;
    }
    return b;
  };
  for (double v : {0.0, 0.5, 1.0}) {
    float y = bot - static_cast<float>(v) * gh;
    dl->AddLine(ImVec2(gx0, y), ImVec2(gx1, y), v == 0.5 ? P.gridBeat : P.gridStep);
    smallText(a, dl, ImVec2(p.x + 4, y - 6), P.textFaint, label(v).c_str());
  }
  // Bars of the edited bank.
  auto xOf = [&](int i) { return pui::freqToX(bandPointHz(i), gx0, gw); };
  float bw = gw / kBdPoints * 0.36f;
  const float* cur = c.v[u.bank][u.target];
  const float* other = c.v[1 - u.bank][u.target];
  // J-93: shaded bars (bright cap, fading body); the bar under the mouse lights up with a readout.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool overGraph = ImGui::IsItemHovered() && mouse.x >= gx0 && mouse.x <= gx1;
  int hotBar = -1;
  if (overGraph) {
    double f = pui::xToFreq(mouse.x, gx0, gw);
    hotBar = std::clamp(static_cast<int>(std::lround(std::log(f / 20.0) / std::log(1000.0) * (kBdPoints - 1))), 0, kBdPoints - 1);
  }
  for (int i = 0; i < kBdPoints; ++i) {
    float x = xOf(i), y = bot - cur[i] * gh;
    float y0 = u.target == BtPan ? bot - 0.5f * gh : bot;
    const bool hot = i == hotBar;
    const ImU32 body = hot ? P.accent : P.text;
    skin::gradientRect(dl, ImVec2(x - bw, std::min(y, y0)), ImVec2(x + bw, std::max(y, y0) + 1), theme::withAlpha(body, hot ? 0.75f : 0.58f),
                       theme::withAlpha(body, hot ? 0.45f : 0.22f), 2);
    dl->AddLine(ImVec2(x - bw, y), ImVec2(x + bw, y), hot ? theme::mix(P.accent, P.paper, 0.4f) : P.paper, 2);
  }
  if (hotBar >= 0) {
    char b[48];
    const double hz = bandPointHz(hotBar);
    const std::string v = label(cur[hotBar]);
    if (hz >= 1000) std::snprintf(b, sizeof b, "%.1f kHz  %s", hz / 1000.0, v.c_str());
    else std::snprintf(b, sizeof b, "%.0f Hz  %s", hz, v.c_str());
    const float bx = std::min(xOf(hotBar) + bw + 6, gx1 - 110), by = std::max(top + 14, bot - cur[hotBar] * gh - 18);
    dl->AddRectFilled(ImVec2(bx - 4, by - 2), ImVec2(bx + 104, by + 14), theme::withAlpha(P.surface950, 0.9f), 3);
    smallText(a, dl, ImVec2(bx, by), P.text, b);
  }
  std::vector<ImVec2> ol, mix;
  double morph = val(st, BdMorph);
  for (int i = 0; i < kBdPoints; ++i) {
    ol.push_back(ImVec2(xOf(i), bot - other[i] * gh));
    double m = c.v[0][u.target][i] + (c.v[1][u.target][i] - c.v[0][u.target][i]) * morph;
    mix.push_back(ImVec2(xOf(i), bot - static_cast<float>(m) * gh));
  }
  dl->AddPolyline(ol.data(), kBdPoints, theme::withAlpha(P.textDim, 0.6f), 0, 1.0f);
  dl->AddPolyline(mix.data(), kBdPoints, P.accent, 0, 1.4f);
  smallText(a, dl, ImVec2(gx1 - 190, top + 2), P.textFaint, u.bank ? "editing B · line: A · blue: morphed" : "editing A · line: B · blue: morphed");
  // Drawing.
  ImGuiIO& io = ImGui::GetIO();
  auto idxAt = [&](float mx) {
    double f = pui::xToFreq(mx, gx0, gw);
    double pos = std::log(f / 20.0) / std::log(1000.0) * (kBdPoints - 1);
    return std::clamp(static_cast<int>(std::lround(pos)), 0, kBdPoints - 1);
  };
  bool hov = ImGui::IsItemHovered();
  if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    int idx = idxAt(io.MousePos.x);
    float v = std::clamp((bot - io.MousePos.y) / gh, 0.0f, 1.0f);
    if (io.KeyShift && u.target == BtDelay) v = std::round(v * 16) / 16;  // quantize times
    int a0 = u.lastIdx < 0 ? idx : u.lastIdx;
    float v0 = u.lastIdx < 0 ? v : u.lastVal;
    int lo = std::min(a0, idx), hi = std::max(a0, idx);
    for (int i = lo; i <= hi; ++i) {
      float f = hi == lo ? 1.f : static_cast<float>(i - a0) / static_cast<float>(idx - a0);
      c.v[u.bank][u.target][i] = v0 + (v - v0) * f;
    }
    u.lastIdx = idx;
    u.lastVal = v;
    setCurves(a, t, c, "draw band curve", gestureKey(t, 0xBD00 + static_cast<uint64_t>(u.target)));
  }
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    int idx = idxAt(io.MousePos.x);
    c.v[u.bank][u.target][idx] = defaultBandCurves().v[u.bank][u.target][idx];
    setCurves(a, t, c, "reset band");
  }
  if (ImGui::IsItemDeactivated()) {
    u.lastIdx = -1;
    a.doc.endGesture();
  }
  if (hov) {
    int idx = idxAt(io.MousePos.x);
    float x = xOf(idx);
    dl->AddRect(ImVec2(x - bw - 2, top), ImVec2(x + bw + 2, bot), P.accent, 2);
    char b[96];
    std::snprintf(b, sizeof b, "Band %d · %.0f Hz · %s   (drag to draw, Shift snaps times, right-click resets)", idx + 1, bandPointHz(idx),
                  label(cur[idx]).c_str());
    a.setHint(b);
  }
}

// Reference multiband-delay panel 560 × 290: band display on top (with the
// mode strip), target / bank selectors and the wheel row underneath.
const skin::Skin& bdSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x22211B, 0x2B2A24, 0x0B0B0B, 0xF0F0F0, 0x42474A, 0xBFBFBF);
    s.fillPressed = skin::hex(0xD9D8D5);
    s.select = skin::hex(0x90B9E2);
    s.lcdBg = skin::hex(0x24262F);
    s.lcdText = skin::hex(0xC9D6E4);
    s.pillBg = skin::hex(0x34332C);
    s.textDim = skin::hex(0xA8A8A8);
    s.graphBg = skin::hex(0x191814);
    s.graphGrid = skin::hex(0x2E2D27);
    s.graphGridStrong = skin::hex(0x45443C);
    s.accent2 = skin::hex(0x90B9E2);
    return s;
  }();
  return k;
}

// J-42: band-delay model — engine controls plus the 16 bands of the bank
// nearest the morph position (drag a bar to draw delay / volume / pan).
void editorBandDelayRef(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  static std::map<uint64_t, int> targetSel;
  int& target = targetSel[t.key()];
  pui::beginSection("Band delays", width - 16);
  const int ks[] = {BdRScale, BdRTenth, BdRFeedback, BdRSmooth, BdRKeep, BdRMorph, BdRMix, BdRHarmonic, BdRWeight, BdRHp};
  const char* kn[] = {"Scale", "x0.1", "Feedback", "Smooth", "Keep pitch", "Morph", "Dry/wet", "Harmonic", "Weighted", "High pass"};
  for (int i = 0; i < 10; ++i) {
    if (i) ImGui::SameLine();
    pui::knobCell(a, t, ks[i], kn[i], 62, 14);
  }
  pui::choiceButtons(a, t, BdRMode, 260);
  ImGui::SameLine();
  pui::choiceButtons(a, t, BdRSat, 200);
  static const char* tn[3] = {"Delay", "Volume", "Pan"};
  for (int k = 0; k < 3; ++k) {
    if (k) ImGui::SameLine();
    if (ImGui::RadioButton(tn[k], target == k)) target = k;
  }
  float v[mbd::kVals];
  mbd::parseBanks(st.data, v);
  const int bank = std::clamp(static_cast<int>(std::lround(7.0 * val(st, BdRMorph))), 0, 7);
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), size(std::max(200.f, width - 40), 140);
  ImGui::InvisibleButton("##bands", size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  pui::displayFrame(dl, p0, ImVec2(p0.x + size.x, p0.y + size.y));
  const float bw = size.x / mbd::kBands;
  for (int u = 0; u < mbd::kBands; ++u) {
    const float y = v[(bank * 3 + target) * mbd::kBands + u];
    const float x0 = p0.x + u * bw + 2, x1 = p0.x + (u + 1) * bw - 2, yb = p0.y + size.y - 2, yt = yb - y * (size.y - 4);
    dl->AddRectFilled(ImVec2(x0, std::min(yt, yb - 1)), ImVec2(x1, yb), theme::withAlpha(pal().accent, 0.85f));
  }
  if (ImGui::IsItemActive()) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int u = std::clamp(static_cast<int>((m.x - p0.x) / bw), 0, mbd::kBands - 1);
    const float y = std::clamp(1.f - (m.y - p0.y) / size.y, 0.f, 1.f);
    PluginState ns = st;
    v[(bank * 3 + target) * mbd::kBands + u] = y;
    size_t at = ns.data.find("\nmbd8");
    if (at == std::string::npos) at = ns.data.find("mbd8");
    std::string d = at == std::string::npos ? ns.data : ns.data.substr(0, at);
    d += "\nmbd8";
    char buf[24];
    for (int i = 0; i < mbd::kVals; ++i) {
      std::snprintf(buf, sizeof buf, " %.9g", static_cast<double>(v[i]));
      d += buf;
    }
    ns.data = d;
    setPluginState(a, t, ns, "draw bands", t.key() ^ 0xB0D);
  }
  char cap[64];
  std::snprintf(cap, sizeof cap, "Bank %d  ·  %s", bank + 1, tn[target]);
  pui::caption(cap);
  pui::endSection();
  ImGui::Spacing();
  pui::choiceButtons(a, t, BdModel, 240);
}

void editorBandDelay(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const PluginState st = *t.state(a.doc.song());
  if (detail::pi(st, BdModel, 0) == 1) {
    editorBandDelayRef(a, t, width);
    return;
  }
  BdUi& u = bdUi[t.key()];
  const skin::Skin& K = bdSkin();
  constexpr float kW = 560, kH = 290;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  // Mode strip (which target / bank is being drawn) + band display.
  {
    static const char* tn[BtCount] = {"TIME", "FEEDBACK", "PAN", "LEVEL"};
    char b[64];
    std::snprintf(b, sizeof b, "%s  ·  BANK %c  ·  MORPH %.0f%%", tn[u.target], u.bank ? 'B' : 'A', val(st, BdMorph) * 100);
    dl->AddRectFilled(F.p(8, 5), F.p(552, 18), skin::hex(0x2C2B24), 2);
    text(dl, F, 12, 6, b, K.select, 7.5f);
  }
  at(F, 8, 20);
  bdGraph(a, t, st, u, F.u(544), F.u(160));
  // Selector row.
  static const char* targets[BtCount] = {"TIME", "FB", "PAN", "LEVEL"};
  for (int i = 0; i < BtCount; ++i) {
    ImGui::PushID(i);
    if (btn("##tgt", F, 8 + i * 42, 188, 40, 18, K, targets[i], u.target == i)) u.target = i;
    if (ImGui::IsItemHovered()) a.setHint("Draw this band curve");
    ImGui::PopID();
  }
  text(dl, F, 184, 191, "BANK", K.textDim);
  if (btn("##bka", F, 212, 188, 30, 18, K, "A", u.bank == 0)) u.bank = 0;
  if (btn("##bkb", F, 243, 188, 30, 18, K, "B", u.bank == 1)) u.bank = 1;
  if (btn("##bkcopy", F, 276, 188, 52, 18, K, u.bank ? "> A" : "> B", false)) {
    BandCurves c = parseBandCurves(st.data);
    std::copy(&c.v[u.bank][0][0], &c.v[u.bank][0][0] + BtCount * kBdPoints, &c.v[1 - u.bank][0][0]);
    setCurves(a, t, c, "copy band bank");
  }
  if (ImGui::IsItemHovered()) a.setHint("Copy every curve of this bank to the other bank");
  static const char* shapes[] = {"FLAT", "RISE", "FALL", "WAVE", "RND"};
  for (int i = 0; i < 5; ++i) {
    ImGui::PushID(100 + i);
    if (btn("##shp", F, 334 + i * 44, 188, 42, 18, K, shapes[i], false)) {
      BandCurves c = parseBandCurves(st.data);
      bdShape(c, u.bank, u.target, i);
      setCurves(a, t, c, "band curve shape");
    }
    if (ImGui::IsItemHovered()) a.setHint("Replace the curve with this shape");
    ImGui::PopID();
  }
  // Wheel row (labels underneath).
  W(a, t, BdDry, F, 8, 222, 40, K, "DRY");
  chk(a, t, BdSync, F, 54, 222, 40, 16, K, "SYNC", K.select);
  if (val(st, BdSync) >= 0.5f) {
    menu(a, t, BdSyncTime, F, 92, 234, 52, 18, K);
    cap(dl, F, 118, 258, "TIME", K.textDim);
  } else {
    W(a, t, BdMaxTime, F, 96, 222, 44, K, "TIME");
  }
  W(a, t, BdFeedback, F, 184, 222, 44, K, "FEEDBACK");
  W(a, t, BdSmooth, F, 242, 222, 44, K, "SMOOTH");
  chk(a, t, BdKeepPitch, F, 290, 227, 70, 17, K, "KEEP PITCH", K.select);
  W(a, t, BdMorph, F, 363, 222, 44, K, "MORPH");
  {
    static const char* sat[3] = {"OFF", "SOFT", "HARD"};
    sel(a, t, BdSat, F, 418, 222, 41, 45, K, false, sat);
  }
  W(a, t, BdLowCut, F, 464, 226, 40, K, "LOW CUT");
  W(a, t, BdWet, F, 510, 222, 44, K, "WET");
  F.end(kW, kH);
}

// =================================================================== Hall
struct HallUi {
  int drag = -1;
};
std::map<uint64_t, HallUi> hallUi;

void hallGraph(App& a, const PluginTarget& t, const PluginState& st, float width, float height) {
  const auto& P = pal();
  HallUi& u = hallUi[t.key()];
  const bool ref = static_cast<int>(val(st, HvModel) + 0.5f) == 1;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float meterW = 34;
  ImGui::InvisibleButton("##hallgraph", ImVec2(width - meterW - 6, height));
  ImVec2 p1(p.x + width - meterW - 6, p.y + height);
  graphBg(dl, p, p1);
  const float gx0 = p.x + 40, gx1 = p1.x - 10, gw = gx1 - gx0, top = p.y + 12, bot = p1.y - 20, gh = bot - top;
  double decay = std::max(0.2f, val(st, HvDecay));
  double rtMax = ref ? std::max(1.0, decay * 1.25)
                     : std::max(1.0, decay * std::max({1.0, static_cast<double>(val(st, HvLowMult)), static_cast<double>(val(st, HvHighMult))}) * 1.25);
  for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
    float x = pui::freqToX(f, gx0, gw);
    dl->AddLine(ImVec2(x, top), ImVec2(x, bot), P.gridStep);
    char b[16];
    if (f >= 1000)
      std::snprintf(b, sizeof b, "%gk", f / 1000);
    else
      std::snprintf(b, sizeof b, "%g", f);
    smallText(a, dl, ImVec2(x + 2, bot + 4), P.textFaint, b);
  }
  for (int i = 0; i <= 4; ++i) {
    float y = bot - gh * i / 4.0f;
    dl->AddLine(ImVec2(gx0, y), ImVec2(gx1, y), P.gridStep);
    char b[16];
    std::snprintf(b, sizeof b, "%.1fs", rtMax * i / 4);
    smallText(a, dl, ImVec2(p.x + 6, y - 6), P.textFaint, b);
  }
  // Output EQ response (dB, ±18 around the mid line) behind the decay curve.
  {
    dsp::Biquad eq;
    eq.set(dsp::Biquad::Peak, 48000, val(st, HvEqFreq), val(st, HvEqQ), val(st, HvEqGain));
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 120; ++i) {
      float x = gx0 + gw * i / 120.0f;
      double db = dsp::gainToDb(eq.magnitude(pui::xToFreq(x, gx0, gw), 48000));
      pts.push_back(ImVec2(x, top + gh * 0.5f - static_cast<float>(db / 18.0) * gh * 0.5f));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::withAlpha(P.accent, 0.55f), 0, 1.2f);
  }
  const bool frozen = static_cast<int>(val(st, HvFreeze) + 0.5f) != 0;
  std::vector<ImVec2> rt;
  for (int i = 0; i <= 160; ++i) {
    float x = gx0 + gw * i / 160.0f;
    double r = frozen ? rtMax : std::min(rtMax, hallRt60(st, pui::xToFreq(x, gx0, gw)));
    rt.push_back(ImVec2(x, bot - static_cast<float>(r / rtMax) * gh));
  }
  for (size_t i = 0; i + 1 < rt.size(); ++i) {
    ImVec2 q[4] = {rt[i], rt[i + 1], ImVec2(rt[i + 1].x, bot), ImVec2(rt[i].x, bot)};
    dl->AddConvexPolyFilled(q, 4, theme::withAlpha(P.text, 0.1f));
  }
  dl->AddPolyline(rt.data(), static_cast<int>(rt.size()), P.paper, 0, 2.0f);
  // Crossover handles: x = crossover, y = that band's decay.
  double fl = val(st, HvLowXover) * 0.5, fh = val(st, HvHighXover) * 2.0;
  if (ref) {  // reference model: low band = Decay, high band = Decay × Brightness
    fl = 60;
    fh = 16000;
  }
  const double hx0 = ref ? fl : val(st, HvLowXover), hx1 = ref ? fh : val(st, HvHighXover);
  ImVec2 h[2] = {ImVec2(pui::freqToX(hx0, gx0, gw), bot - static_cast<float>(std::min(rtMax, hallRt60(st, fl)) / rtMax) * gh),
                 ImVec2(pui::freqToX(hx1, gx0, gw), bot - static_cast<float>(std::min(rtMax, hallRt60(st, std::min(fh, 20000.0))) / rtMax) * gh)};
  ImGuiIO& io = ImGui::GetIO();
  int hover = -1;
  for (int i = 0; i < 2; ++i)
    if (std::hypot(io.MousePos.x - h[i].x, io.MousePos.y - h[i].y) < 10) hover = i;
  if (ImGui::IsItemActivated()) u.drag = hover;
  if (ImGui::IsItemActive() && u.drag >= 0) {
    double f = pui::xToFreq(io.MousePos.x, gx0, gw);
    double r = std::clamp((bot - io.MousePos.y) / gh, 0.01f, 1.0f) * rtMax;
    if (ref) {
      if (u.drag == 0) dragParam(a, t, HvDecay, std::clamp(r, 0.25, 20.0));
      if (u.drag == 1) dragParam(a, t, HvBright, std::clamp(r / decay, 0.0, 1.0));
    } else if (u.drag == 0) {
      dragParam(a, t, HvLowXover, std::clamp(f, 50.0, 1500.0));
      dragParam(a, t, HvLowMult, std::clamp(r / decay, 0.25, 4.0));
    } else {
      dragParam(a, t, HvHighXover, std::clamp(f, 1000.0, 16000.0));
      dragParam(a, t, HvHighMult, std::clamp(r / decay, 0.05, 2.0));
    }
  }
  if (ImGui::IsItemDeactivated()) {
    u.drag = -1;
    a.doc.endGesture();
  }
  for (int i = 0; i < 2; ++i) {
    bool hot = hover == i || u.drag == i;
    dl->AddLine(ImVec2(h[i].x, top), ImVec2(h[i].x, bot), theme::withAlpha(P.text, 0.25f));
    dl->AddCircleFilled(h[i], hot ? 6.5f : 5.f, hot ? P.accent : P.paper);
  }
  if (hover >= 0 && ref) a.setHint(hover == 0 ? "Decay: drag ↕ to set the tail length" : "Brightness: drag ↕ to set the high-band decay");
  if (hover >= 0 && !ref) a.setHint(hover == 0 ? "Low band: drag ↔ crossover, ↕ low decay" : "High band: drag ↔ crossover, ↕ high decay");
  char b[64];
  std::snprintf(b, sizeof b, frozen ? "FROZEN" : "RT60 %.2f s @ 1 kHz", hallRt60(st, 1000));
  smallText(a, dl, ImVec2(gx1 - 120, top + 2), frozen ? P.accent : P.textDim, b);
  // Envelope meter: live wet gain from the effect (ducking / gating).
  ImGui::SameLine(0, 6);
  ImVec2 m = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(meterW, height));
  dl->AddRectFilled(m, ImVec2(m.x + meterW, m.y + height), P.surface950, 6);
  float g = 1.0f;
  if (!t.isChannel())
    if (auto rtm = a.builder.effectRuntime(t.owner, t.slot)) g = static_cast<float>(dsp::dbToGain(rtm->meter.load(std::memory_order_relaxed)));
  if (ref && !t.isChannel())
    if (auto rtm = a.builder.effectRuntime(t.owner, t.slot)) g = std::clamp((rtm->meter.load(std::memory_order_relaxed) + 60.0f) / 60.0f, 0.0f, 1.0f);
  float mh = (height - 24) * std::clamp(g, 0.0f, 1.0f);
  const bool envOn = static_cast<int>(val(st, ref ? HvEnvRoute : HvEnvMode) + 0.5f) != 0;
  dl->AddRectFilled(ImVec2(m.x + 9, m.y + height - 12 - mh), ImVec2(m.x + meterW - 9, m.y + height - 12), envOn ? P.text : P.surface700, 3);
  smallText(a, dl, ImVec2(m.x + 5, m.y + 3), P.textFaint, "ENV");
}

// Reference big-reverb panel 940 × 450: INPUT / REVERB / SHIMMER (feedback) /
// OUTPUT strips across the top with value pills above each control; envelope
// (and Gridline's band damping) bottom left; the decay display bottom right.
const skin::Skin& hallSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x323E44, 0x2B353A, 0x1E2529, 0xDFDFDF, 0x3F484D, 0xB1B1B1);
    s.fillPressed = skin::hex(0xF2F2F2);
    s.valueBg = skin::hex(0xB4C8CF);
    s.valueText = skin::hex(0x292929);
    s.pillBg = skin::hex(0x26302F);
    s.handle = skin::hex(0x30373A);
    s.select = skin::hex(0xDFDFDF);
    s.lcdBg = skin::hex(0x1F2629);
    s.graphBg = skin::hex(0x383636);
    s.graphGrid = skin::hex(0x464343);
    s.graphGridStrong = skin::hex(0x5A5656);
    s.accent2 = skin::hex(0x00A3D2);
    return s;
  }();
  return k;
}

// Tick wheel (1..768 = 0..16 sixteenth steps) with a "steps:ticks" pill.
bool hallTicksWheel(App& a, const PluginTarget& t, int k, const skin::Form& F, float x, float y, float sz, const skin::Skin& K, const char* lbl) {
  using namespace flskin;
  double steps = 16.0 * (std::clamp(static_cast<double>(pval(a, t, k)), 1.0, 768.0) - 1.0) / 767.0;
  int whole = static_cast<int>(steps);
  char b[24];
  std::snprintf(b, sizeof b, "%d:%02d", whole, static_cast<int>((steps - whole) * 24.0 + 0.5));
  float pw = std::max(sz + 8, 46.0f);
  skin::valueStrip(ImGui::GetWindowDrawList(), F.p(x + sz * 0.5f - pw * 0.5f, y - 17), F.p(x + sz * 0.5f + pw * 0.5f, y - 4), K, b, F.font(7.5f));
  bool ch = W(a, t, k, F, x, y, sz, K, lbl);
  if (ImGui::IsItemHovered()) a.setHint(std::string(lbl) + ": steps:ticks of a 1/16 step (tempo synced)");
  return ch;
}
// Time wheel that swaps to its synced tick wheel when the "T" toggle is down.
void hallTimeCell(App& a, const PluginTarget& t, const PluginState& st, int msParam, int syncParam, int ticksParam, const skin::Form& F, float x, float y,
                  const skin::Skin& K, const char* lbl, float tx, float ty) {
  using namespace flskin;
  if (val(st, syncParam) >= 0.5f)
    hallTicksWheel(a, t, ticksParam, F, x, y, 36, K, lbl);
  else
    WV(a, t, msParam, F, x, y, 36, K, lbl);
  tog(a, t, syncParam, F, tx, ty, 14, 14, K, "T");
  if (ImGui::IsItemHovered()) a.setHint("Tempo sync this time");
}

// Reference model (FL-style big reverb, 940 × 450): INPUT / REVERB / FEEDBACK
// (or FEED-FORWARD voices) / OUTPUT across the top, ENVELOPE bottom left, the
// decay display + envelope meter bottom right.
void editorHallRef(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = hallSkin();
  constexpr float kW = 940, kH = 450;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  skin::Skin KE = K;  // envelope controls: grey value pills, blue fills
  KE.valueBg = skin::hex(0x818181);
  KE.fill = skin::hex(0x007CA0);
  KE.fillPressed = skin::hex(0x00A3D2);
  KE.select = skin::hex(0x00A3D2);
  const bool ff = static_cast<int>(val(st, HvFbMode) + 0.5f) == 1;
  box(dl, F, 6, 8, 116, 214, K, "INPUT", 22);
  box(dl, F, 128, 8, 330, 214, K, "REVERB", 22);
  box(dl, F, 464, 8, 232, 214, K, ff ? "PITCH VOICES" : "FEEDBACK", 22);
  box(dl, F, 702, 8, 232, 214, K, "OUTPUT", 22);
  box(dl, F, 6, 232, 284, 210, K, "ENVELOPE", 22);
  chk(a, t, HvOn, F, 136, 11, 60, 16, K, "ON");
  if (ImGui::IsItemHovered()) a.setHint("Reverb section on (off clears the tail)");
  chk(a, t, HvFbOn, F, 472, 11, 56, 16, K, "ON");
  menu(a, t, HvFbMode, F, 584, 31, 104, 13, K);
  // Input.
  VV(a, t, HvInGain, F, 18, 63, 32, 128, K, "GAIN");
  WV(a, t, HvInHighCut, F, 72, 63, 36, K, "HI CUT");
  WV(a, t, HvInLowCut, F, 72, 155, 36, K, "LO CUT");
  // Reverb.
  WV(a, t, HvDecay, F, 139, 95, 64, K, "DECAY");
  WV(a, t, HvBright, F, 224, 63, 36, K, "BRIGHT");
  WV(a, t, HvSize, F, 283, 63, 36, K, "SIZE");
  WV(a, t, HvDiffusion, F, 342, 63, 36, K, "DIFFUSE");
  WV(a, t, HvCharacter, F, 401, 63, 36, K, "CHARACTER");
  hallTimeCell(a, t, st, HvPreDelay, HvPreSync, HvPreTicks, F, 224, 155, K, "PRE-DELAY", 262, 193);
  WV(a, t, HvModDepth, F, 283, 155, 36, K, "MOD AMT");
  WV(a, t, HvModFreq, F, 342, 155, 36, K, "MOD FREQ");
  {
    // Three-row switch: normal / freeze / sustain (choice values 0 / 1 / 3).
    static const char* names[3] = {"NORMAL", "FREEZE", "SUSTAIN"};
    static const int vals[3] = {HfNormal, HfFreeze, HfSustain};
    static const char* hints[3] = {"Normal: input on, tail decays", "Freeze: input off, tail holds", "Sustain: input on, tail holds"};
    int cur = static_cast<int>(val(st, HvFreeze) + 0.5f);
    if (cur == HfInfinite) cur = HfSustain;
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(1100 + i);
      if (btn("##fz", F, 393, 131 + i * 17.0f, 58, 16, K, names[i], cur == vals[i])) a.doc.setParam(t.pid(HvFreeze), vals[i], false);
      if (ImGui::IsItemHovered()) a.setHint(hints[i]);
      ImGui::PopID();
    }
  }
  chk(a, t, HvHQ, F, 401, 194, 50, 17, K, "HQ");
  if (ImGui::IsItemHovered()) a.setHint("High quality: interpolated reads of the modulated lines and pre-delay");
  if (val(st, HvOn) < 0.5f) dim(dl, F, 130, 32, 326, 188, K);
  // Feedback shimmer or the feed-forward voices.
  if (!ff) {
    WV(a, t, HvFbHighCut, F, 475, 63, 36, K, "HI CUT");
    WV(a, t, HvFbLowCut, F, 475, 155, 36, K, "LO CUT");
    VV(a, t, HvFbPitch, F, 534, 63, 32, 128, K, "PITCH");
    VV(a, t, HvFbGain, F, 589, 63, 32, 128, K, "GAIN");
    hallTimeCell(a, t, st, HvFbDelay, HvFbSync, HvFbTicks, F, 644, 63, K, "DELAY", 672, 101);
    WV(a, t, HvFbMix, F, 644, 155, 36, K, "SOURCE");
    if (ImGui::IsItemHovered()) a.setHint("Shimmer source: reverb input (0 %) ↔ reverb tail (100 %)");
  } else {
    VV(a, t, HvFfDirect, F, 477, 63, 32, 128, K, "DIRECT");
    WV(a, t, HvFfPitch1, F, 532, 63, 36, K, "PITCH 1");
    WV(a, t, HvFfPitch2, F, 532, 155, 36, K, "PITCH 2");
    WV(a, t, HvFfAmp1, F, 587, 63, 36, K, "LEVEL 1");
    WV(a, t, HvFfAmp2, F, 587, 155, 36, K, "LEVEL 2");
    hallTimeCell(a, t, st, HvFfDelay1, HvFfSync1, HvFfTicks1, F, 644, 63, K, "DELAY 1", 674, 101);
    hallTimeCell(a, t, st, HvFfDelay2, HvFfSync2, HvFfTicks2, F, 644, 155, K, "DELAY 2", 674, 193);
  }
  if (val(st, HvFbOn) < 0.5f) dim(dl, F, 466, 48, 228, 172, K);
  // Output.
  VV(a, t, HvDry, F, 717, 63, 32, 128, K, "DRY");
  VV(a, t, HvWet, F, 772, 63, 32, 128, K, "WET");
  tog(a, t, HvLimiter, F, 806, 193, 14, 14, K, "L");
  if (ImGui::IsItemHovered()) a.setHint("Limit the reverb output");
  WV(a, t, HvEqFreq, F, 827, 63, 36, K, "PEAK FREQ");
  WV(a, t, HvEqGain, F, 886, 63, 36, K, "PEAK GAIN");
  WV(a, t, HvEqQ, F, 827, 155, 36, K, "PEAK Q");
  WV(a, t, HvWidth, F, 886, 155, 36, K, "WIDTH");
  // Envelope.
  {
    static const char* em[3] = {"OFF", "WET", "DECAY"};
    sel(a, t, HvEnvRoute, F, 12, 276, 48, 54, KE, false, em);
  }
  WV(a, t, HvEnvThresh, F, 72, 290, 36, KE, "THRES");
  WV(a, t, HvEnvScale, F, 129, 290, 36, KE, "SCALE");
  WV(a, t, HvEnvLowCut, F, 186, 290, 36, KE, "LO CUT");
  WV(a, t, HvEnvOffset, F, 243, 290, 36, KE, "OFFSET");
  WV(a, t, HvEnvAttack, F, 72, 380, 36, KE, "ATTACK");
  WV(a, t, HvEnvRelease, F, 129, 380, 36, KE, "DECAY");
  WV(a, t, HvEnvSmooth, F, 186, 380, 36, KE, "SMOOTH");
  chk(a, t, HvEnvSidechain, F, 14, 352, 52, 15, KE, "SC IN");
  if (ImGui::IsItemHovered()) a.setHint("Envelope detector listens to the mixer track's sidechain input");
  if (static_cast<int>(val(st, HvEnvRoute) + 0.5f) == HeOff) dim(dl, F, 66, 256, 222, 184, K);
  // Model selector + decay display (+ envelope meter at its right edge).
  menu(a, t, HvModel, F, 822, 238, 90, 14, K);
  at(F, 296, 254);
  hallGraph(a, t, st, F.u(638), F.u(188));
  F.end(kW, kH);
}

void editorHall(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const PluginState st = *t.state(a.doc.song());
  if (static_cast<int>(val(st, HvModel) + 0.5f) == 1) {
    editorHallRef(a, t, width);
    return;
  }
  const skin::Skin& K = hallSkin();
  constexpr float kW = 940, kH = 450;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const ImU32 envFill = skin::hex(0x007CA0);
  skin::Skin KE = K;  // envelope controls: grey value pills, blue fills
  KE.valueBg = skin::hex(0x818181);
  KE.fill = envFill;
  KE.fillPressed = skin::hex(0x00A3D2);
  KE.select = skin::hex(0x00A3D2);
  // Section frames + captions.
  box(dl, F, 6, 8, 116, 214, K, "INPUT", 22);
  box(dl, F, 128, 8, 330, 214, K, "REVERB", 22);
  box(dl, F, 464, 8, 232, 214, K, "SHIMMER", 22);
  box(dl, F, 702, 8, 232, 214, K, "OUTPUT", 22);
  box(dl, F, 6, 232, 168, 210, K, "ENVELOPE", 22);
  box(dl, F, 180, 232, 108, 210, K, "DAMPING", 22);
  chk(a, t, HvOn, F, 136, 11, 60, 16, K, "ON");
  chk(a, t, HvFbOn, F, 472, 11, 60, 16, K, "ON");
  menu(a, t, HvFbMode, F, 530, 31, 104, 13, K);
  // Input.
  VV(a, t, HvInGain, F, 18, 63, 32, 128, K, "GAIN");
  WV(a, t, HvInHighCut, F, 72, 63, 36, K, "HI CUT");
  WV(a, t, HvInLowCut, F, 72, 155, 36, K, "LO CUT");
  // Reverb.
  WV(a, t, HvDecay, F, 139, 95, 64, K, "DECAY");
  WV(a, t, HvHighMult, F, 224, 63, 36, K, "HI DECAY");
  WV(a, t, HvSize, F, 283, 63, 36, K, "SIZE");
  WV(a, t, HvDiffusion, F, 342, 63, 36, K, "DIFFUSE");
  WV(a, t, HvCharacter, F, 401, 63, 36, K, "EARLY");
  if (val(st, HvPreSync) >= 0.5f) {
    menu(a, t, HvPreSyncTime, F, 214, 160, 56, 18, K);
    cap(dl, F, 242, 192, "PRE-DELAY", K.textDim);
  } else {
    WV(a, t, HvPreDelay, F, 224, 155, 36, K, "PRE-DELAY");
  }
  tog(a, t, HvPreSync, F, 262, 192, 14, 14, K, "T");
  WV(a, t, HvModDepth, F, 283, 155, 36, K, "MOD AMP");
  WV(a, t, HvModRate, F, 342, 155, 36, K, "MOD FREQ");
  {
    static const char* fz[3] = {"OFF", "FREEZE", "INF"};
    sel(a, t, HvFreeze, F, 393, 128, 58, 51, K, false, fz);
  }
  chk(a, t, HvHQ, F, 401, 194, 50, 17, K, "HQ");
  // Shimmer.
  WV(a, t, HvFbHighCut, F, 475, 63, 36, K, "HI CUT");
  WV(a, t, HvFbLowCut, F, 475, 155, 36, K, "LO CUT");
  VV(a, t, HvFbPitch, F, 534, 63, 32, 128, K, "PITCH");
  VV(a, t, HvFbGain, F, 589, 63, 32, 128, K, "GAIN");
  WV(a, t, HvFbDelay, F, 644, 63, 36, K, "DELAY");
  WV(a, t, HvFbMix, F, 644, 155, 36, K, "TO REV");
  // Output.
  VV(a, t, HvDry, F, 717, 63, 32, 128, K, "DRY");
  VV(a, t, HvWet, F, 772, 63, 32, 128, K, "WET");
  tog(a, t, HvLimiter, F, 806, 193, 14, 14, K, "L");
  WV(a, t, HvEqFreq, F, 827, 63, 36, K, "EQ FREQ");
  WV(a, t, HvEqGain, F, 886, 63, 36, K, "EQ GAIN");
  WV(a, t, HvEqQ, F, 827, 155, 36, K, "EQ Q");
  WV(a, t, HvWidth, F, 886, 155, 36, K, "WIDTH");
  // Envelope.
  {
    static const char* em[4] = {"OFF", "DUCK", "GATE", "SWELL"};
    sel(a, t, HvEnvMode, F, 12, 276, 48, 74, KE, false, em);
  }
  WV(a, t, HvEnvThresh, F, 72, 290, 36, KE, "THRES");
  WV(a, t, HvEnvAmount, F, 124, 290, 36, KE, "AMOUNT");
  WV(a, t, HvEnvAttack, F, 72, 380, 36, KE, "ATTACK");
  WV(a, t, HvEnvRelease, F, 124, 380, 36, KE, "RELEASE");
  // Band damping (Gridline: per-band decay multipliers and crossovers).
  WV(a, t, HvLowMult, F, 192, 290, 36, K, "LO DECAY");
  WV(a, t, HvLowXover, F, 240, 290, 36, K, "LO XOVER");
  WV(a, t, HvHighXover, F, 216, 380, 36, K, "HI XOVER");
  menu(a, t, HvModel, F, 822, 238, 90, 14, K);
  // Decay display (+ envelope meter at its right edge).
  at(F, 296, 254);
  hallGraph(a, t, st, F.u(638), F.u(188));
  F.end(kW, kH);
}

// =============================================================== Ensemble
void ensembleVisual(App& a, const PluginState& st, float width, float height) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(width, height));
  ImVec2 p1(p.x + width, p.y + height);
  graphBg(dl, p, p1);
  int nv = static_cast<int>(val(st, EnLines) + 0.5f) == 1 ? 8 : 4;
  double modAmt = val(st, EnModAmount), depth = modAmt * modAmt * 0.6, rate = val(st, EnModRate);
  double move = val(st, EnMoveAmount), mrate = val(st, EnMoveRate), spread = val(st, EnSpread), phase = val(st, EnPhase);
  double maxD = 1.6;
  const float x0 = p.x + 14, x1 = p1.x - 14, y0 = p.y + 18, y1 = p1.y - 12;
  dl->AddLine(ImVec2((x0 + x1) * 0.5f, y0), ImVec2((x0 + x1) * 0.5f, y1), P.gridBeat);
  smallText(a, dl, ImVec2(x0, p.y + 4), P.textFaint, "L");
  smallText(a, dl, ImVec2(x1 - 6, p.y + 4), P.textFaint, "R");
  smallText(a, dl, ImVec2((x0 + x1) * 0.5f + 4, p.y + 4), P.textFaint, "voices · pan ↔ · delay ↕");
  for (int v = 0; v < nv; ++v) {
    double u = nv > 1 ? static_cast<double>(v) / (nv - 1) : 0.5;
    double base = 0.55 + 0.9 * u;
    double inc = rate * (0.82 + 0.36 * ((v * 5) % nv) / std::max(1, nv - 1));
    double minc = mrate * (0.7 + 0.6 * ((v * 3) % nv) / std::max(1, nv - 1));
    for (int k = 12; k >= 0; --k) {
      double tt = a.now - k * 0.04;
      double lfo = std::sin(2 * kPiD * (tt * inc + phase * v / nv + 0.37 * v));
      double d = base * (1 + depth * lfo);
      double pan = std::clamp(0.5 + (u - 0.5) * spread * 0.5 + move * 0.499 * std::sin(2 * kPiD * (tt * minc * 0.5 + 0.21 * v)), 0.0, 1.0);
      float x = x0 + static_cast<float>(pan) * (x1 - x0), y = y0 + static_cast<float>(d / maxD) * (y1 - y0);
      float al = k == 0 ? 1.0f : 0.25f * (1.0f - k / 13.0f);
      dl->AddCircleFilled(ImVec2(x, y), k == 0 ? 5.f : 3.f, theme::withAlpha(k == 0 ? P.paper : P.text, al));
    }
  }
}

// Reference multi-voice chorus panel 747 × 217: three X/Y pads and the voice
// display on top, one row of wheels underneath.
const skin::Skin& ensSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x616161, 0x575757, 0x333A3E, 0xB3D5FF, 0x2D3337, 0xDADADA);
    s.fillPressed = skin::hex(0x50A0FF);
    s.select = skin::hex(0x50A0FF);
    s.lcdBg = skin::hex(0x1C2022);
    s.lcdText = skin::hex(0x8DA1B2);
    s.graphBg = skin::hex(0x2C2C2C);
    s.graphGrid = skin::hex(0x3A3A3A);
    s.graphGridStrong = skin::hex(0x4C4C4C);
    s.graphText = skin::hex(0xA0A0A0);
    s.textDim = skin::hex(0xCCCCCC);
    s.handle = skin::hex(0x3E6D92);
    return s;
  }();
  return k;
}

void editorEnsemble(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = ensSkin();
  constexpr float kW = 747, kH = 217;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const bool ref = static_cast<int>(val(st, EnModel) + 0.5f) == 1;  // J-26 reference engine: knob positions
  const int pDelay = ref ? EnK0 : EnDelay, pFb = ref ? EnK1 : EnFeedback, pAmt = ref ? EnK3 : EnModAmount, pRate = ref ? EnK2 : EnModRate;
  const int pMove = ref ? EnK4 : EnMoveAmount, pMoveR = ref ? EnK5 : EnMoveRate, pLines = ref ? EnK6 : EnLines;
  cap(dl, F, 73, 24, "DELAY / FEEDBACK", K.textDim);
  cap(dl, F, 204, 24, "MOD AMOUNT / RATE", K.textDim);
  cap(dl, F, 335, 24, "MOVEMENT / RATE", K.textDim);
  skin::xyPad(a, t, pDelay, pFb, F.p(23, 38), F.sz(100, 100), K);
  skin::xyPad(a, t, pAmt, pRate, F.p(154, 38), F.sz(100, 100), K);
  skin::xyPad(a, t, pMove, pMoveR, F.p(285, 38), F.sz(100, 100), K);
  at(F, 408, 33);
  ensembleVisual(a, st, F.u(296), F.u(112));
  if (!ref) V(a, t, EnSpread, F, 712, 35, 18, 100, K, "SPREAD", skin::hex(0x50A0FF), false);
  {
    static const char* eng[2] = {"OLD", "REF"};
    sel(a, t, EnModel, F, 678, 10, 64, 16, K, true, eng);
  }
  W(a, t, pDelay, F, 19, 149, 44, K, "DELAY");
  W(a, t, pFb, F, 83, 149, 44, K, "FEEDBACK");
  W(a, t, pAmt, F, 150, 149, 44, K, "AMOUNT");
  W(a, t, pRate, F, 214, 149, 44, K, "RATE");
  W(a, t, pMove, F, 281, 149, 44, K, "MOVE");
  W(a, t, pMoveR, F, 345, 149, 44, K, "SPEED");
  {
    static const char* ln[2] = {"4", "8"};
    sel(a, t, pLines, F, 420, 160, 50, 18, K, true, ln);
    cap(dl, F, 445, 182, "LINES", K.textDim);
  }
  W(a, t, ref ? EnK8 : EnHighPass, F, 490, 149, 44, K, "HP");
  W(a, t, ref ? EnK7 : EnLowPass, F, 545, 149, 44, K, "LP");
  W(a, t, ref ? EnK10 : EnPhase, F, 620, 149, 44, K, "PHASE");
  W(a, t, ref ? EnK9 : EnMix, F, 686, 149, 44, K, "MIX");
  F.end(kW, kH);
}

// ========================================================== Bucket Chorus
void bbdScope(App& a, const PluginState& st, float width, float height) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(width, height));
  ImVec2 p1(p.x + width, p.y + height);
  graphBg(dl, p, p1);
  const bool ref = val(st, BcModel) >= 0.5f;
  BbdModeSettings m = bbdSettings(st, a.doc.song().tempo);
  const int mode = static_cast<int>(val(st, BcMode) + 0.5f);
  const double lr = ref && mode < 3 ? flc::bcFixedMode(mode).lr : val(st, BcPhaseLR), start = val(st, BcStartPhase);
  const double span = m.rateHz > 0.01 ? std::max(2.0 / m.rateHz, 0.4) : 2.0;
  const float x0 = p.x + 44, x1 = p1.x - 10, y0 = p.y + 10, y1 = p1.y - 10;
  const double hiMs = std::max(m.minMs, m.maxMs), dmax = std::max(6.0, hiMs * 1.35);
  for (double ms : {0.0, dmax * 0.25, dmax * 0.5, dmax * 0.75}) {
    float y = y1 - static_cast<float>(ms / dmax) * (y1 - y0);
    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), P.gridStep);
    char b[16];
    std::snprintf(b, sizeof b, "%.1f ms", ms);
    smallText(a, dl, ImVec2(p.x + 6, y - 6), P.textFaint, b);
  }
  for (int c = 0; c < 2; ++c) {
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 200; ++i) {
      double ph = start + span * m.rateHz * i / 200.0 + (c ? lr : 0.0);
      double l01;
      if (ref) {
        l01 = m.wave == 0 ? flc::refSine(ph) : flc::refTri(ph);
      } else {
        double f = ph - std::floor(ph);
        l01 = 0.5 + 0.5 * (m.wave == 1 ? (f < 0.5 ? 4 * f - 1 : 3 - 4 * f) : std::sin(2 * kPiD * f));
      }
      double d = m.minMs + (m.maxMs - m.minMs) * l01;
      pts.push_back(ImVec2(x0 + (x1 - x0) * i / 200.0f, y1 - static_cast<float>(d / dmax) * (y1 - y0)));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), c ? P.textDim : P.paper, 0, 1.6f);
  }
  char b[96];
  if (m.rateHz < 1e-9)
    std::snprintf(b, sizeof b, "L ━  R ┄   %.2f–%.2f ms · LFO stopped", std::min(m.minMs, m.maxMs), hiMs);
  else
    std::snprintf(b, sizeof b, "L ━  R ┄   %.2f–%.2f ms · %.2f Hz", std::min(m.minMs, m.maxMs), hiMs, m.rateHz);
  smallText(a, dl, ImVec2(x1 - 230, p.y + 6), P.textDim, b);
}

// Reference bucket-brigade chorus panel 747 × 217 (Gridline adds the delay
// sweep display in the title strip): mode pads, delay sliders, LFO, levels.
const skin::Skin& bbdSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x212527, 0x2A2F32, 0x333A3E, 0xB5B3AE, 0x2D3337, 0xB9BFC3);
    s.fillPressed = skin::hex(0xDEDDDA);
    s.handle = skin::hex(0x32383D);
    s.select = skin::hex(0xCE0000);
    s.lcdBg = skin::hex(0x1C2022);
    s.lcdText = skin::hex(0xDEDDDA);
    s.graphBg = skin::hex(0x16191B);
    s.graphGrid = skin::hex(0x272C2F);
    s.graphGridStrong = skin::hex(0x394045);
    s.accent2 = skin::hex(0xD98509);
    return s;
  }();
  return k;
}

void editorBbdChorus(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = bbdSkin();
  constexpr float kW = 747, kH = 217;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  skin::Skin KS = K;  // sliders: light rail, light fill
  KS.rail = skin::hex(0x4A5054);
  KS.fill = skin::hex(0xDEDDDA);
  const ImU32 red = skin::hex(0xCE0000);
  const int mode = static_cast<int>(val(st, BcMode) + 0.5f);
  const bool custom = mode == 3, ref = val(st, BcModel) >= 0.5f;
  at(F, 20, 8);
  bbdScope(a, st, F.u(410), F.u(50));
  {
    static const char* models[2] = {"LEGACY", "REF"};
    sel(a, t, BcModel, F, 646, 22, 84, 18, K, true, models);
    if (ImGui::IsItemHovered()) a.setHint("Chorus model: legacy (older projects) or the reference bucket-brigade behaviour");
  }
  // Mode pads I / II / CUSTOM; I + II lights both (click the other pad, or shift-click, to link).
  {
    static const uint32_t cols[3] = {0xC9AA44, 0xD98509, 0x6F7A80};
    skin::well(dl, F.p(20, 72), F.p(172, 180), K, 4 * F.s);
    cap(dl, F, 96, 78, "MODE", K.textDim);
    cap(dl, F, 96, 104, mode == 2 ? "I + II" : (mode == 3 ? "CUSTOM" : (mode == 1 ? "II" : "I")), skin::hex(0xD98509), 8.0f);
    static const char* names[3] = {"I", "II", "CUSTOM"};
    const bool lit[3] = {mode == 0 || mode == 2, mode == 1 || mode == 2, mode == 3};
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(i);
      skin::Skin KB = K;
      KB.select = skin::hex(cols[i]);
      if (btn("##mode", F, 26 + i * 48, 125, 44, 45, KB, names[i], lit[i])) {
        int nm;
        if (i == 2)
          nm = 3;
        else if (ImGui::GetIO().KeyShift)
          nm = mode == 2 ? i : 2;  // shift-click toggles the I + II link
        else
          nm = mode == 1 - i ? 2 : i;  // the other fixed pad active → I + II
        a.doc.setParam(t.pid(BcMode), nm, false);
      }
      if (ImGui::IsItemHovered()) a.setHint(i < 2 ? "Fixed mode · click the other pad (or shift-click) for I + II" : "Custom: all delay and LFO controls");
      ImGui::PopID();
    }
  }
  // Controls a fixed mode overrides: times, wave, speed, sync (both models);
  // the reference model also fixes feedback, high pass, L/R phase, mix and invert.
  const bool fixedAll = !custom && ref;
  ImGui::BeginDisabled(!custom);
  V(a, t, BcTime1, F, 201, 73, 35, 104, KS, "TIME 1");
  V(a, t, BcTime2, F, 238, 73, 35, 104, KS, "TIME 2");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(fixedAll);
  V(a, t, BcFeedback, F, 275, 73, 35, 104, KS, "FEEDBACK");
  V(a, t, BcHighPass, F, 312, 73, 35, 104, KS, "HP");
  ImGui::EndDisabled();
  if (!custom) dim(dl, F, 199, 71, fixedAll ? 150 : 76, 120, K);
  W(a, t, BcStartPhase, F, 379, 73, 42, K, "START");
  ImGui::BeginDisabled(fixedAll);
  W(a, t, BcPhaseLR, F, 379, 135, 42, K, "L/R");
  ImGui::EndDisabled();
  if (fixedAll) dim(dl, F, 376, 132, 48, 58, K);
  ImGui::BeginDisabled(!custom);
  {
    static const char* wv[2] = {"SIN", "TRI"};
    sel(a, t, BcWave, F, 440, 77, 50, 33, K, false, wv);
  }
  chk(a, t, BcSync, F, 437, 160, 60, 17, K, "SYNC", red);
  if (val(st, BcSync) >= 0.5f) {
    if (ref) {
      V(a, t, BcSyncTicks, F, 510, 73, 35, 104, KS, "PERIOD");
      int ticks = static_cast<int>(std::lround(val(st, BcSyncTicks)));
      char tb[24];
      std::snprintf(tb, sizeof tb, "%d:%02d", ticks / 48, ticks % 48);
      skin::valueStrip(dl, F.p(497, 56), F.p(558, 70), K, tb, F.font(7.5f));
    } else {
      menu(a, t, BcSyncRate, F, 497, 115, 60, 18, K);
      cap(dl, F, 527, 180, "SPEED", K.textDim);
    }
  } else {
    V(a, t, BcRate, F, 510, 73, 35, 104, KS, "SPEED");
  }
  ImGui::EndDisabled();
  if (!custom) {
    dim(dl, F, 436, 71, 124, 120, K);
    cap(dl, F, 498, 196, "SET BY MODE", K.textDim);
  }
  chk(a, t, BcMonoIn, F, 566, 117, 70, 17, K, "MONO IN", red);
  ImGui::BeginDisabled(fixedAll);
  chk(a, t, BcInvertWet, F, 566, 139, 70, 17, K, "INV WET", red);
  ImGui::EndDisabled();
  chk(a, t, BcNoiseGate, F, 566, 160, 70, 17, K, ref ? "GATE" : "HISS GATE", red);
  W(a, t, BcHiss, F, 640, 73, 42, K, ref ? "NOISE" : "HISS");
  W(a, t, BcGain, F, 640, 135, 42, K, "GAIN", ref ? skin::WheelBigLine : skin::WheelBigLine | skin::WheelCentered);
  ImGui::BeginDisabled(fixedAll);
  V(a, t, BcMix, F, 690, 73, 35, 104, KS, "MIX");
  ImGui::EndDisabled();
  if (fixedAll) dim(dl, F, 688, 71, 39, 120, K);
  F.end(kW, kH);
}

// ================================================================== Swirl
void swirlVisual(App& a, const PluginState& st, float width, float height) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(width, height));
  ImVec2 p1(p.x + width, p.y + height);
  graphBg(dl, p, p1);
  int order = std::clamp(static_cast<int>(std::lround(val(st, SwOrder))), 1, 9);
  double depth = val(st, SwDepth), speed = val(st, SwSpeed), base = val(st, SwDelay), spread = val(st, SwSpread);
  double dmax = std::max(0.5, base * (1 + depth * 1.6) * 1.15);
  const float x0 = p.x + 10, x1 = p1.x - 10, y0 = p.y + 10, y1 = p1.y - 10;
  double span = std::clamp(2.0 / std::max(0.02, speed), 1.0, 20.0);
  for (int v = 0; v < order; ++v) {
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 120; ++i) {
      double tt = a.now - span + span * i / 120.0;
      double ph = tt * speed * (1.0 + 0.045 * v) + static_cast<double>(v) / order;
      double d = base * (1.0 + depth * 1.6 * (0.5 + 0.5 * std::sin(2 * kPiD * ph)));
      pts.push_back(ImVec2(x0 + (x1 - x0) * i / 120.0f, y1 - static_cast<float>(d / dmax) * (y1 - y0)));
    }
    double u = order > 1 ? static_cast<double>(v) / (order - 1) : 0.5;
    float al = 0.35f + 0.65f * static_cast<float>(1.0 - std::fabs(u * 2 - 1) * spread * 0.6);
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::withAlpha(P.paper, al), 0, 1.4f);
  }
  smallText(a, dl, ImVec2(p.x + 8, p.y + 4), P.textFaint, "voice delay over time");
}

void editorSwirl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  swirlVisual(a, st, width, 96);
  ImGui::Dummy(ImVec2(0, 2));
  pui::beginSection("SWIRL", width);
  pui::knobCell(a, t, SwOrder, "Voices", 70, 20);
  ImGui::SameLine(0, 10);
  const std::pair<int, const char*> fs[] = {{SwDepth, "Depth"}, {SwSpeed, "Speed"}, {SwDelay, "Delay"}, {SwSpread, "Spread"},
                                            {SwCross, "Cross"}, {SwDry, "Dry"},     {SwWet, "Wet"}};
  float cell = (width - 20 - 80) / 7;
  for (size_t i = 0; i < 7; ++i) {
    if (i) ImGui::SameLine(0, 0);
    faderCell(a, t, fs[i].first, fs[i].second, 112, cell);
  }
  pui::endSection();
}

// =========================================================== Retro Phaser
// Sweep frequency of a channel at song time `now` (seconds), as the DSP computes it.
double phaserFreqAt(const PluginState& st, double now, double tempo, int c) {
  if (val(st, VpManual) >= 0.5f) return val(st, VpManualFreq);
  const bool ref = val(st, VpModel) >= 0.5f;
  double fa = val(st, VpFreqMin), fb = val(st, VpFreqMax);
  double lo = std::min(fa, fb), hi = std::max(fa, fb);
  if (!ref) {
    double rate = val(st, VpRate);
    if (val(st, VpSync) >= 0.5f) rate = tempo / 60.0 / kLfoSyncBeats[std::clamp(static_cast<int>(val(st, VpSyncRate) + 0.5f), 0, 8)];
    double v = phaserLfo(static_cast<int>(val(st, VpWave) + 0.5f), now * rate + val(st, VpStartPhase) + (c ? val(st, VpPhaseLR) : 0.0), val(st, VpTension));
    return val(st, VpFreqMin) * std::pow(std::max(1.0, static_cast<double>(val(st, VpFreqMax)) / std::max(1.0f, val(st, VpFreqMin))), v);
  }
  double rlo = std::min(val(st, VpRateLo), val(st, VpRateHi)), rhi = std::max(val(st, VpRateLo), val(st, VpRateHi));
  double rate = std::clamp(static_cast<double>(val(st, VpRate)), rlo, rhi);
  double spd = rhi > rlo * 1.000001 ? std::clamp(std::log(rate / rlo) / std::log(rhi / rlo), 0.0, 1.0) : 0.0;
  if (val(st, VpSync) >= 0.5f) rate = tempo / 60.0 / (std::max(1.0f, val(st, VpSyncTicks)) / 192.0);
  double ph = now * rate + val(st, VpStartPhase) + (c ? val(st, VpPhaseLR) : 0.0);
  int wave = static_cast<int>(val(st, VpWave) + 0.5f);
  double v = wave == 0 ? flc::refSine(ph) : wave == 1 ? flc::refTri(ph) : phaserLfo(wave, ph, 0.5);
  double y = flc::vpBend(v, flc::vpTensionK(val(st, VpTension)));
  return lo * std::pow(2.0, std::log2(hi / std::max(1e-6, lo)) * flc::vpSweepPos(y, spd, val(st, VpDepthScale), val(st, VpDepthPull)));
}

void phaserGraph(App& a, const PluginState& st, float width, float height) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(width, height));
  ImVec2 p1(p.x + width, p.y + height);
  graphBg(dl, p, p1);
  const float gx0 = p.x + 10, gx1 = p1.x - 10, gw = gx1 - gx0, top = p.y + 10, bot = p1.y - 20, gh = bot - top;
  pui::drawFreqGrid(dl, ImVec2(gx0, top), ImVec2(gx1, bot), 24, true);
  const bool ref = val(st, VpModel) >= 0.5f;
  double fmin = std::min(val(st, VpFreqMin), val(st, VpFreqMax)), fmax = std::max(val(st, VpFreqMin), val(st, VpFreqMax));
  bool manual = val(st, VpManual) >= 0.5f;
  if (!manual)
    dl->AddRectFilled(ImVec2(pui::freqToX(fmin, gx0, gw), top), ImVec2(pui::freqToX(fmax, gx0, gw), bot), theme::withAlpha(P.text, 0.06f));
  int stages = ref ? 4 : kPhaserStages[std::clamp(static_cast<int>(val(st, VpStages) + 0.5f), 0, 4)];
  double fb = val(st, VpFeedback) * (val(st, VpInvertFb) >= 0.5f ? -1 : 1), mix = val(st, VpMix);
  const double sr = 48000;
  for (int c = 0; c < 2; ++c) {
    double f = phaserFreqAt(st, a.now, a.doc.song().tempo, c);
    double tn = std::tan(kPiD * std::clamp(f, 10.0, sr * 0.45) / sr), ac = (tn - 1) / (tn + 1);
    dsp::Biquad color;
    color.set(dsp::Biquad::LowShelf, sr, val(st, VpColorFreq), ref ? 0.5 : 0.7, val(st, VpColorGain));
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 200; ++i) {
      float x = gx0 + gw * i / 200.0f;
      double hz = pui::xToFreq(x, gx0, gw), w = 2 * kPiD * hz / sr;
      std::complex<double> z1 = std::polar(1.0, -w);
      std::complex<double> A = std::pow((ac + z1) / (1.0 + ac * z1), stages);
      std::complex<double> H = A / (1.0 - fb * A * z1);
      double mag = ref ? std::abs((1.0 - mix) + mix * H) * color.magnitude(hz, sr) : std::abs((1.0 - mix) + mix * H * color.magnitude(hz, sr));
      double db = std::clamp(dsp::gainToDb(mag), -24.0, 24.0);
      pts.push_back(ImVec2(x, top + gh * 0.5f - static_cast<float>(db / 24.0) * gh * 0.5f));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), c ? theme::withAlpha(P.textDim, 0.8f) : P.paper, 0, c ? 1.2f : 2.0f);
  }
  smallText(a, dl, ImVec2(gx1 - 120, top + 2), P.textDim, manual ? "manual position" : "live sweep · L ━ R ┄");
}

// Reference vintage-phaser panel 747 × 277 (shifted down 44 under Gridline's
// response display): phasing sliders, LFO, echo and levels on top, the
// advanced row (engine, drive, colour, filters, rate range, depth) underneath.
const skin::Skin& vpSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x616161, 0x585858, 0x30363A, 0xF2F2F2, 0xA4AAAE, 0x15181E);
    s.fillPressed = skin::hex(0xFFFFFF);
    s.handle = skin::hex(0x32383D);
    s.select = skin::hex(0xCE0000);
    s.textDim = skin::hex(0x1E222A);
    s.lcdBg = skin::hex(0x1C2022);
    s.lcdText = skin::hex(0xDFE1E3);
    s.graphBg = skin::hex(0x2A2C2E);
    s.graphGrid = skin::hex(0x3A3D40);
    s.graphGridStrong = skin::hex(0x4E5256);
    s.graphText = skin::hex(0xB9BFC3);
    s.valueBg = skin::hex(0x4C4C4C);
    s.valueText = skin::hex(0xEEEEEE);
    return s;
  }();
  return k;
}

void editorPhaser(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = vpSkin();
  constexpr float kW = 747, kH = 321, dy = 44;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  {
    skin::Skin KG = K;  // display: light text on the dark well
    KG.text = skin::hex(0xDFE1E3);
    KG.textDim = skin::hex(0xA9AFB4);
    skin::Scope scope(KG);
    at(F, 8, 6);
    phaserGraph(a, st, F.u(731), F.u(76));
  }
  skin::Scope scope(K);
  skin::Skin KS = K;  // sliders: dark rail, light fill
  KS.rail = skin::hex(0x353535);
  KS.fill = skin::hex(0xDEDDDA);
  skin::Skin KA = K;  // advanced row wheels
  KA.rail = skin::hex(0x8F8F8F);
  KA.fill = skin::hex(0xB5B3AE);
  KA.cap = skin::hex(0x3F474E);
  KA.text = skin::hex(0xDFE1E3);
  const ImU32 red = skin::hex(0xCE0000);
  const bool manual = val(st, VpManual) >= 0.5f, ref = val(st, VpModel) >= 0.5f;
  auto ticksText = [&](int prm) {
    int ticks = static_cast<int>(std::lround(val(st, prm)));
    char tb[24];
    std::snprintf(tb, sizeof tb, "%d:%02d", ticks / 48, ticks % 48);
    return std::string(tb);
  };
  // Phasing.
  if (manual) {
    VV(a, t, VpManualFreq, F, 23, dy + 71, 26, 106, KS, "FREQ");
  } else {
    VV(a, t, VpFreqMin, F, 23, dy + 71, 26, 106, KS, "MIN");
    VV(a, t, VpFreqMax, F, 57, dy + 71, 26, 106, KS, "MAX");
  }
  if (ref) chk(a, t, VpHq, F, 92, dy + 130, 40, 17, K, "HQ", red);
  chk(a, t, VpManual, F, 92, dy + 163, 60, 17, K, "MANUAL", red);
  VV(a, t, VpFeedback, F, 125, dy + 71, 32, 86, KS, "FEEDBACK");
  // LFO.
  ImGui::BeginDisabled(manual);
  W(a, t, VpStartPhase, F, 197, dy + 71, 42, K, "START");
  W(a, t, VpPhaseLR, F, 197, dy + 135, 42, K, "L/R");
  if (ref) {
    static const char* wv[2] = {"SIN", "TRI"};
    skin::Skin KW = K;  // light captions on the dark switch
    KW.textDim = skin::hex(0xA9AFB4);
    skin::switchSel(a, t, VpWave, F.p(259, dy + 75), F.sz(41, 34), KW, wv, 2, false);
  } else {
    menu(a, t, VpWave, F, 250, dy + 75, 64, 18, K);
    cap(dl, F, 282, dy + 96, "SHAPE", K.textDim);
  }
  W(a, t, VpTension, F, 258, dy + 135, 42, K, "TENSION");
  if (val(st, VpSync) >= 0.5f) {
    if (ref) {
      V(a, t, VpSyncTicks, F, 332, dy + 71, 32, 106, KS, "PERIOD");
      std::string tv = ticksText(VpSyncTicks);
      skin::valueStrip(dl, F.p(322, dy + 54), F.p(374, dy + 68), K, tv.c_str(), F.font(7.5f));
    } else {
      menu(a, t, VpSyncRate, F, 318, dy + 110, 58, 18, K);
      cap(dl, F, 347, dy + 132, "SPEED", K.textDim);
    }
  } else {
    VV(a, t, VpRate, F, 332, dy + 71, 32, 106, KS, "SPEED");
  }
  chk(a, t, VpSync, F, 318, dy + 192, 60, 14, K, "SYNC", red);
  ImGui::EndDisabled();
  if (manual) dim(dl, F, 192, dy + 66, 186, 142, K);
  // Echo.
  chk(a, t, VpDelayOn, F, 520, dy + 50, 70, 17, K, "ECHO", red);
  W(a, t, VpDelayMix, F, 407, dy + 71, 42, K, "MIX");
  W(a, t, VpDelayStereo, F, 456, dy + 71, 42, K, "STEREO", skin::WheelBigLine | skin::WheelCentered);
  if (val(st, VpDelaySync) >= 0.5f) {
    if (ref) {
      W(a, t, VpDelayTicks, F, 535, dy + 71, 60, K, nullptr);
      std::string tv = ticksText(VpDelayTicks);
      skin::valueStrip(dl, F.p(540, dy + 133), F.p(590, dy + 146), K, tv.c_str(), F.font(7.5f));
    } else {
      menu(a, t, VpDelaySyncTime, F, 532, dy + 90, 64, 18, K);
      cap(dl, F, 564, dy + 112, "TIME", K.textDim);
    }
  } else {
    W(a, t, VpDelayTime, F, 535, dy + 71, 60, K, nullptr);
    std::string tv = pfmt(a, t, VpDelayTime);
    skin::valueStrip(dl, F.p(540, dy + 133), F.p(590, dy + 146), K, tv.c_str(), F.font(7.5f));
  }
  chk(a, t, VpDelaySync, F, 556, dy + 149, 50, 14, K, "SYNC", red);
  if (ref) chk(a, t, VpDelayKeep, F, 556, dy + 167, 64, 14, K, "KEEP", red);
  if (ref && ImGui::IsItemHovered()) a.setHint("Keep pitch: time changes crossfade two read heads instead of gliding");
  W(a, t, VpDelayFb, F, 407, dy + 135, 42, K, "FEEDBACK");
  W(a, t, VpDelayLp, F, 456, dy + 135, 42, K, "LP");
  W(a, t, VpDelayHp, F, 505, dy + 135, 42, K, "HP");
  // Levels.
  W(a, t, VpNoise, F, 632, dy + 71, 42, K, "NOISE");
  chk(a, t, VpNoiseGate, F, 620, dy + 130, 64, 17, K, "GATE", red);
  chk(a, t, VpMonoIn, F, 620, dy + 163, 64, 17, K, "MONO IN", red);
  VV(a, t, VpMix, F, 691, dy + 71, 32, 106, KS, "MIX");
  // Advanced row.
  box(dl, F, 8, dy + 206, 731, 66, K, nullptr);
  KA.textDim = skin::hex(0xDFE1E3);
  {
    static const char* models[2] = {"LEGACY", "REF"};
    sel(a, t, VpModel, F, 150, dy + 213, 92, 17, KA, true, models);
    cap(dl, F, 196, dy + 233, "MODEL", KA.text);
  }
  if (ref) {
    static const char* eng[2] = {"CLS", "ANA"};
    skin::switchSel(a, t, VpEngine, F.p(15, dy + 212), F.sz(34, 34), KA, eng, 2, false);
    cap(dl, F, 32, dy + 255, "ENGINE", KA.text);
    W(a, t, VpDrive, F, 58, dy + 212, 38, KA, "DRIVE", skin::WheelBigLine | skin::WheelCentered);
    tog(a, t, VpInvertFb, F, 104, dy + 216, 40, 26, KA, "INV FB");
    W(a, t, VpRateLo, F, 505, dy + 212, 38, KA, "RATE 1");
    W(a, t, VpRateHi, F, 554, dy + 212, 38, KA, "RATE 2");
    W(a, t, VpDepthScale, F, 632, dy + 212, 38, KA, "DEPTH");
    W(a, t, VpDepthPull, F, 686, dy + 212, 38, KA, "PULL");
  } else {
    static const char* sg[5] = {"4", "6", "8", "10", "12"};
    sel(a, t, VpStages, F, 14, dy + 210, 100, 18, KA, true, sg);
    cap(dl, F, 64, dy + 230, "STAGES", KA.text);
    W(a, t, VpDrive, F, 505, dy + 212, 38, KA, "DRIVE");
    tog(a, t, VpInvertFb, F, 556, dy + 216, 44, 26, KA, "INV FB");
  }
  W(a, t, VpColorGain, F, 278, dy + 212, 38, KA, "COLOR");
  W(a, t, VpColorFreq, F, 329, dy + 212, 38, KA, "COLOR HZ");
  W(a, t, VpInputHp, F, 407, dy + 212, 38, KA, "IN HP");
  W(a, t, VpFbHp, F, 456, dy + 212, 38, KA, "FB HP");
  F.end(kW, kH);
}

}  // namespace

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (ty.compare(0, 3, "gl.") != 0) return false;
  ImGui::PushID("spacefx");
  bool ok = true;
  if (ty == "gl.convolver")
    editorConvolver(a, t, width);
  else if (ty == "gl.multidelay")
    editorMultiDelay(a, t, width);
  else if (ty == "gl.banddelay")
    editorBandDelay(a, t, width);
  else if (ty == "gl.hall")
    editorHall(a, t, width);
  else if (ty == "gl.ensemble")
    editorEnsemble(a, t, width);
  else if (ty == "gl.bbdchorus")
    editorBbdChorus(a, t, width);
  else if (ty == "gl.swirl")
    editorSwirl(a, t, width);
  else if (ty == "gl.vphaser")
    editorPhaser(a, t, width);
  else
    ok = false;
  ImGui::PopID();
  return ok;
}

}  // namespace gl::packs::spacefx
