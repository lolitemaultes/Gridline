// Gridline — Image Synth editor (J-19) on the reference image-to-sound
// generator's panel (its 380 × 170 strip: frequency wheels, scale box,
// option checks, file buttons, bitmap strip) widened to 760 and palette:
// range / scale / options, scan and envelope wheels on top; source, file /
// canvas tools and voices; then the picture as a spectrogram (frequency axis
// on the left, time along the bottom). In Canvas mode the picture is a
// 64 × 32 drawing surface.
#include "../flc_kit.h"
#include <cstdlib>
#include <filesystem>
#include <map>
#include "../../engine/Dsp.h"
#include "models_widgets.h"
#include "../../core/Platform.h"

namespace gl::packs::models {

using namespace ui2;

namespace {

std::map<uint64_t, int>& brushes() {
  static std::map<uint64_t, int> m;
  return m;
}

std::vector<uint8_t> canvasOf(const std::string& data) {
  std::string hex = imageDataCanvas(data);
  return hex.empty() ? starterCanvas() : decodeCanvas(hex);
}

void setCanvas(App& a, const PluginTarget& t, const PluginState& st, const std::vector<uint8_t>& px, const char* label, uint64_t merge = 0) {
  PluginState ns = st;
  ns.data = makeImageData(imageDataPath(st.data), encodeCanvas(px));
  ns.params[ImSource] = SrcCanvas;
  if (!(ns == st)) setPluginState(a, t, ns, label, merge);
}

void openImageDialog(App& a, const PluginTarget& t) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = "Load image";
  const char* h = gl::platform::homeEnv();
  a.fileDialog.dir = h ? h : ".";
  a.fileDialog.exts = {".bmp", ".ppm", ".pgm", ".tga"};
  PluginTarget tt = t;
  a.fileDialog.onOk = [&a, tt](const std::string& path) {
    const PluginState* cur = tt.state(a.doc.song());
    if (!cur) return;
    std::string err;
    if (!loadImageFile(path, &err)) {
      a.setHint("Could not load image: " + err);
      return;
    }
    PluginState ns = *cur;
    ns.data = makeImageData(path, imageDataCanvas(cur->data));
    ns.params[ImSource] = SrcFile;
    setPluginState(a, tt, ns, "load image");
  };
}

bool isImageFile(const std::string& p) {
  std::string e = std::filesystem::path(p).extension().string();
  for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".bmp" || e == ".ppm" || e == ".pgm" || e == ".tga";
}

void imageView(App& a, const PluginTarget& t, const PluginState& st, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float axisW = 54, axisH = 18;
  ImVec2 p(p0.x + axisW, p0.y);
  ImVec2 sz(size.x - axisW, size.y - axisH);
  ImGui::InvisibleButton("##image", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), P.surface950, 6);
  int src = ival(st, ImSource);
  ImagePtr im = imageFor(st);
  const int W = im->w, H = std::min(im->h, kImgMaxRows);
  // Picture: each pixel drawn in its own colour (user content).
  int cols = std::min(W, 256);
  float cw = sz.x / cols, rh = sz.y / H;
  for (int y = 0; y < H; ++y)
    for (int cx = 0; cx < cols; ++cx) {
      int x = cx * W / cols;
      size_t i = static_cast<size_t>(y * im->w + x);
      float r = im->r[i], g = im->g[i], b = im->b[i];
      if (r + g + b < 0.03f) continue;
      ImU32 c = IM_COL32(static_cast<int>(std::min(1.0f, r) * 255), static_cast<int>(std::min(1.0f, g) * 255), static_cast<int>(std::min(1.0f, b) * 255), 255);
      if (src == SrcCanvas) c = theme::withAlpha(P.paper, std::min(1.0f, r));
      dl->AddRectFilled(ImVec2(p.x + cx * cw, p.y + y * rh), ImVec2(p.x + (cx + 1) * cw + 0.5f, p.y + (y + 1) * rh + 0.5f), c);
    }
  if (src == SrcCanvas) {
    for (int x = 8; x < kCanvasW; x += 8) dl->AddLine(ImVec2(p.x + x * cw, p.y), ImVec2(p.x + x * cw, p.y + sz.y), P.gridStep);
  }
  dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), P.line, 2);
  // Frequency axis (for C4 with key tracking) and time axis.
  double lo = val(st, ImLow, 110), hi = std::max(lo * 1.01, static_cast<double>(val(st, ImHigh, 7000)));
  int scale = ival(st, ImScale);
  ImGui::PushFont(a.fontMono, 9.5f);
  for (int k = 0; k <= 4; ++k) {
    int row = (H - 1) * k / 4;
    double hz = imageRowHz(row, H, scale, lo, hi, 1.0);
    char b[16];
    if (hz >= 1000)
      std::snprintf(b, sizeof b, "%.1fk", hz / 1000);
    else
      std::snprintf(b, sizeof b, "%.0f", hz);
    float y = p.y + (row + 0.5f) * rh;
    dl->AddText(ImVec2(p0.x + 6, y - 6), P.textFaint, b);
  }
  double pixel = val(st, ImPixel, 0.05f), total = pixel * W;
  for (int k = 0; k <= 4; ++k) {
    char b[16];
    std::snprintf(b, sizeof b, "%.2f s", total * k / 4);
    float x = p.x + sz.x * k / 4;
    dl->AddText(ImVec2(std::min(x + 2, p.x + sz.x - 40), p.y + sz.y + 3), P.textFaint, b);
  }
  float start = val(st, ImStart);
  if (start > 0) {
    float sx = p.x + sz.x * start;
    dl->AddLine(ImVec2(sx, p.y), ImVec2(sx, p.y + sz.y), P.accent, 1.5f);
  }
  ImGui::PopFont();
  // Canvas painting.
  if (src == SrcCanvas) {
    static int lx = -1, ly = -1;
    ImVec2 m = ImGui::GetIO().MousePos;
    int x = std::clamp(static_cast<int>((m.x - p.x) / cw), 0, kCanvasW - 1);
    int y = std::clamp(static_cast<int>((m.y - p.y) / rh), 0, kCanvasH - 1);
    if (act) {
      if (ImGui::IsItemActivated()) lx = -1;
      int level = ImGui::IsMouseDown(ImGuiMouseButton_Right) ? 0 : brushes()[t.key()];
      auto px = canvasOf(st.data);
      int x0 = lx < 0 ? x : lx, y0 = ly < 0 ? y : ly;
      int steps = std::max(std::abs(x - x0), std::abs(y - y0));
      for (int s = 0; s <= steps; ++s) {
        int xx = steps ? x0 + (x - x0) * s / steps : x, yy = steps ? y0 + (y - y0) * s / steps : y;
        px[static_cast<size_t>(yy * kCanvasW + xx)] = static_cast<uint8_t>(level);
      }
      lx = x;
      ly = y;
      setCanvas(a, t, st, px, "paint canvas", 0x43414E5600000000ull ^ t.key());
    }
    if (ImGui::IsItemDeactivated()) {
      a.doc.endGesture();
      lx = ly = -1;
    }
    if (hov) a.setHint("Canvas: drag to paint (brush level) · right-drag to erase · rows are pitches, columns are time");
  } else if (hov) {
    a.setHint("The picture is played left to right: each row is a sine (top = high), brightness = level; red = left, green = right");
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
      std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (isImageFile(path) && loadImageFile(path)) {
        PluginState ns = st;
        ns.data = makeImageData(path, imageDataCanvas(st.data));
        ns.params[ImSource] = SrcFile;
        setPluginState(a, t, ns, "load image");
      }
    }
    ImGui::EndDragDropTarget();
  }
}

const skin::Skin& imageSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x283842, 0x26353F, 0x3F484D, 0x4ECCFF, 0x3F484D, 0xB9BFC3);
    s.panelHi = skin::hex(0x31454F);
    s.handle = skin::hex(0x2A2F33);
    s.select = skin::hex(0x4ECCFF);
    s.accent2 = skin::hex(0xFFEAAA);
    s.lcdBg = skin::hex(0x0D2432);
    s.lcdText = skin::hex(0x9FD8EE);
    s.pillBg = skin::hex(0x2A2F33);
    s.graphBg = skin::hex(0x0D1A22);
    s.graphCurve = skin::hex(0x4ECCFF);
    return s;
  }();
  return k;
}

}  // namespace

void drawImageSynth(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = imageSkin();
  constexpr float kW = 760, kH = 392;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  ImGui::PushID("gl.imagesynth");
  const int src = ival(st, ImSource);
  // ---- band 1 (reference strip): frequency range wheels, scale, options, title, voices
  flk::wheel(c, ImLow, 7, 15, 40, "Low");
  flk::wheel(c, ImHigh, 59, 15, 40, "High");
  flk::text(c, 118, 12, "SCALE");
  flk::menu(c, ImScale, 117, 24, 98, 24);
  flk::chk(c, ImUseBlue, 228, 14, 70, 12, "USE BLUE");
  flk::chk(c, ImGrainy, 301, 14, 72, 12, "GRAINY");
  flk::chk(c, ImLoop, 228, 32, 70, 12, "LOOP");
  flk::chk(c, ImWiden, 301, 32, 72, 12, "WIDEN");
  flk::chk(c, ImReverse, 228, 50, 70, 12, "REVERSE");
  flk::chk(c, ImKeyTrack, 301, 50, 72, 12, "KEY TRK");
  // Scan / envelope / output wheels to the right of the reference strip.
  skin::well(dl, F.p(384, 6), F.p(754, 78), K, 4 * F.s);
  const int wp[6] = {ImPixel, ImStart, ImGamma, ImAttack, ImRelease, ImVolume};
  const char* wn[6] = {"Pixel", "Start", "Contrast", "Attack", "Release", "Volume"};
  for (int i = 0; i < 6; ++i) flk::wheel(c, wp[i], 398 + i * 52, 14, 34, wn[i]);
  // ---- band 2: source, file / picture / canvas tools, title, voices
  static const char* srcN[3] = {"PICTURE", "FILE", "CANVAS"};
  auto sd = describeParam(a.doc.song(), t.pid(ImSource));
  int nSrc = sd && !sd->choices.empty() ? std::min(3, static_cast<int>(sd->choices.size())) : 3;
  flk::sw(c, ImSource, 6, 85, 176, 31, srcN, nSrc, true);
  if (src == SrcPattern) {
    flk::menu(c, ImPattern, 190, 89, 150, 24);
  } else if (src == SrcFile) {
    std::string path = imageDataPath(st.data);
    if (flk::btn(c, "##open", 190, 85, 50, 31, "OPEN", false)) openImageDialog(a, t);
    if (ImGui::IsItemHovered()) a.setHint("Load an image (BMP · PPM/PGM · TGA)");
    std::string nm = path.empty() ? "drop a file on the picture" : std::filesystem::path(path).filename().string();
    bool bad = !path.empty() && !loadImageFile(path);
    flk::lcd(c, 246, 89, 210, 24, bad ? "cannot read this file" : nm.c_str());
  } else {
    int& brush = brushes()[t.key()];
    if (brush <= 0) brush = 15;
    static const std::pair<const char*, int> levels[] = {{"SOFT", 5}, {"MID", 10}, {"FULL", 15}};
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(i);
      if (flk::btn(c, "##brush", 190 + i * 46, 85, 44, 31, levels[i].first, brush == levels[i].second)) brush = levels[i].second;
      if (ImGui::IsItemHovered()) a.setHint("Brush level for painting (right-drag erases)");
      ImGui::PopID();
    }
    if (flk::btn(c, "##clear", 334, 85, 50, 31, "CLEAR", false))
      setCanvas(a, t, st, std::vector<uint8_t>(static_cast<size_t>(kCanvasW * kCanvasH), 0), "clear canvas");
    if (flk::btn(c, "##sketch", 388, 85, 56, 31, "SKETCH", false)) {
      PluginState ns = st;
      ns.data = makeImageData(imageDataPath(st.data), "");
      setPluginState(a, t, ns, "reset canvas");
    }
  }
  if (src != SrcCanvas) {
    if (flk::btn(c, "##copy", 462, 85, 110, 31, "COPY TO CANVAS", false)) {  // trace the picture into the editable canvas
      ImagePtr im = imageFor(st);
      std::vector<uint8_t> px(static_cast<size_t>(kCanvasW * kCanvasH), 0);
      for (int y = 0; y < kCanvasH; ++y)
        for (int x = 0; x < kCanvasW; ++x) {
          float m = 0;
          for (int yy = y * im->h / kCanvasH; yy < std::max(y * im->h / kCanvasH + 1, (y + 1) * im->h / kCanvasH); ++yy)
            for (int xx = x * im->w / kCanvasW; xx < std::max(x * im->w / kCanvasW + 1, (x + 1) * im->w / kCanvasW); ++xx) {
              size_t i = static_cast<size_t>(yy * im->w + xx);
              m = std::max(m, std::max(im->r[i], std::max(im->g[i], im->b[i])));
            }
          px[static_cast<size_t>(y * kCanvasW + x)] = static_cast<uint8_t>(std::lround(std::min(1.0f, m) * 15));
        }
      setCanvas(a, t, st, px, "copy picture to canvas");
    }
    if (ImGui::IsItemHovered()) a.setHint("Copies the current picture into the canvas so you can paint over it");
  }
  skin::text(dl, F.p(590, 91), skin::hex(0x7D888E), "IMAGE · SYNTH", F.font(11));
  flk::text(c, 700, 80, "VOICES");
  flk::digi(c, ImVoices, 700, 92, 50, 22);
  // ---- picture (the reference's bitmap strip, enlarged)
  skin::well(dl, F.p(6, 124), F.p(754, 386), K, 4 * F.s);
  ImGui::SetCursorScreenPos(F.p(10, 128));
  imageView(a, t, st, F.sz(740, 254));
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::models
