// Gridline — J-184: Pixel Tone pictures from image files. Synthetic PNG / JPEG /
// BMP / TGA / GIF files (written with stb_image_write into a temporary folder)
// are decoded and reduced (row limit, column / pixel caps, aspect, alpha on
// black), corrupt files are refused, the picture text keeps its source line,
// a loaded picture changes the sound and a black one is silent, and pictures
// dropped on a Pixel Tone window or channel row load there (nowhere else).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "../Check.h"
#include "../../src/core/BrowserIndex.h"
#include "../../src/core/ExternalDrop.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/ChannelIns.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/packs/synths/pixel.h"
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb/stb_image_write.h"

using namespace gl;
using namespace gl::packs::synths::pixel;
namespace fs = std::filesystem;

namespace {

void append(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size)); }

std::string png(int w, int h, int comp, const std::vector<uint8_t>& px) {
  std::string out;
  stbi_write_png_to_func(append, &out, w, h, comp, px.data(), w * comp);
  return out;
}
std::string jpg(int w, int h, const std::vector<uint8_t>& rgb, int q = 92) {
  std::string out;
  stbi_write_jpg_to_func(append, &out, w, h, 3, rgb.data(), q);
  return out;
}
std::string bmp(int w, int h, const std::vector<uint8_t>& rgb) {
  std::string out;
  stbi_write_bmp_to_func(append, &out, w, h, 3, rgb.data());
  return out;
}
std::string tga(int w, int h, const std::vector<uint8_t>& rgb) {
  std::string out;
  stbi_write_tga_to_func(append, &out, w, h, 3, rgb.data());
  return out;
}

void writeFile(const fs::path& p, const std::string& bytes) {
  std::ofstream o(p, std::ios::binary);
  o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

const uint8_t* at(const Picture& p, int x, int y) { return &p.rgb[(static_cast<size_t>(y) * static_cast<size_t>(p.w) + static_cast<size_t>(x)) * 3]; }

struct Buf {
  std::vector<float> l, r;
  bool finite() const {
    for (size_t i = 0; i < l.size(); ++i)
      if (!std::isfinite(l[i]) || !std::isfinite(r[i])) return false;
    return true;
  }
  double rms() const {
    double acc = 0;
    for (size_t i = 0; i < l.size(); ++i) acc += 0.5 * (static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]);
    return l.empty() ? 0.0 : std::sqrt(acc / static_cast<double>(l.size()));
  }
};

Buf render(const PluginState& s, double seconds, int block = 256) {
  Buf out;
  auto g = createGenerator(s.type);
  ChannelAux aux;
  prepareChannelAux(s, aux);
  const size_t n = static_cast<size_t>(seconds * 44100.0);
  out.l.assign(n, 0.f), out.r.assign(n, 0.f);
  if (!g) return out;
  ProcessContext ctx;
  ctx.sampleRate = 44100.0;
  ctx.tempo = 120;
  g->noteOn(1, 60, 0.9f, 0);
  for (size_t pos = 0; pos < n; pos += static_cast<size_t>(block)) {
    const int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - pos));
    g->process(s, aux, ctx, out.l.data() + pos, out.r.data() + pos, k);
  }
  return out;
}

void testSizes() {
  int w = 0, h = 0;
  pictureSize(4000, 3000, 23, w, h);
  expect(w == 31 && h == 23, "size: a 4000 x 3000 photo at 23 rows becomes 31 x 23 (aspect kept)");
  pictureSize(100, 10, 23, w, h);
  expect(w == 100 && h == 10, "size: a small picture is kept as it is (never enlarged)");
  pictureSize(10000, 500, 999, w, h);
  expect(w == kMaxColumns && h == 51, "size: a wide picture is held to 1024 columns, aspect kept");
  pictureSize(2000, 2000, 999, w, h);
  expect(w * h <= kMaxPixels && std::abs(w - h) <= 1 && w >= 510, "size: a big square at 999 rows is held to the pixel budget");
  pictureSize(50, 5000, 999, w, h);
  expect(w == 10 && h == 999, "size: a tall picture is held to Max rows");
  pictureSize(3000, 1, 999, w, h);
  expect(w == 1024 && h == 1, "size: a one-row strip keeps one row");
  pictureSize(0, 10, 23, w, h);
  expect(w == 0 && h == 0, "size: an empty source has no size");
}

void testFormats(const fs::path& dir) {
  // PNG with alpha: left half opaque red, right half transparent white.
  {
    const int W = 64, H = 48;
    std::vector<uint8_t> px(static_cast<size_t>(W * H * 4));
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        uint8_t* p = &px[static_cast<size_t>((y * W + x) * 4)];
        if (x < W / 2) p[0] = 255, p[1] = 0, p[2] = 0, p[3] = 255;
        else p[0] = p[1] = p[2] = 255, p[3] = 0;
      }
    writeFile(dir / "alpha.png", png(W, H, 4, px));
    Picture p;
    std::string err;
    const bool ok = loadPictureFile((dir / "alpha.png").u8string(), 24, p, &err);
    expect(ok && p.w == 32 && p.h == 24 && p.srcW == 64 && p.srcH == 48 && p.name == "alpha.png", "png: 64 x 48 loads as 32 x 24 with its name");
    expect(ok && at(p, 2, 5)[0] == 255 && at(p, 2, 5)[1] == 0, "png: opaque red stays red");
    expect(ok && at(p, 28, 5)[0] == 0 && at(p, 28, 5)[1] == 0 && at(p, 28, 5)[2] == 0, "png: transparent pixels lie on black (silent)");
    // Half transparent: 50 % of the colour.
    for (size_t i = 0; i < px.size(); i += 4) px[i] = 200, px[i + 1] = 100, px[i + 2] = 0, px[i + 3] = 128;
    writeFile(dir / "half.png", png(W, H, 4, px));
    const bool ok2 = loadPictureFile((dir / "half.png").u8string(), 999, p, &err);
    expect(ok2 && p.w == 64 && p.h == 48 && std::abs(at(p, 10, 10)[0] - 100) <= 1 && std::abs(at(p, 10, 10)[1] - 50) <= 1, "png: half alpha halves the level");
  }
  // JPEG: a horizontal grey ramp keeps its ramp.
  {
    const int W = 400, H = 300;
    std::vector<uint8_t> rgb(static_cast<size_t>(W * H * 3));
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        for (int c = 0; c < 3; ++c) rgb[static_cast<size_t>((y * W + x) * 3 + c)] = static_cast<uint8_t>(x * 255 / (W - 1));
    writeFile(dir / "ramp.jpg", jpg(W, H, rgb));
    Picture p;
    std::string err;
    const bool ok = loadPictureFile((dir / "ramp.jpg").u8string(), 23, p, &err);
    expect(ok && p.w == 31 && p.h == 23, "jpg: 400 x 300 at 23 rows becomes 31 x 23");
    expect(ok && at(p, 0, 10)[0] < 12 && at(p, 30, 10)[0] > 243 && at(p, 15, 10)[0] > 110 && at(p, 15, 10)[0] < 145, "jpg: the ramp survives the reduction");
    writeFile(dir / "ramp.JPEG", jpg(W, H, rgb));
    expect(isPictureFile((dir / "ramp.JPEG").u8string()) && loadPictureFile((dir / "ramp.JPEG").u8string(), 23, p), "jpg: upper-case extension");
  }
  // BMP and TGA: exact colours when no reduction is needed.
  {
    const int W = 30, H = 20;
    std::vector<uint8_t> rgb(static_cast<size_t>(W * H * 3));
    for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = static_cast<uint8_t>((i * 37) & 255);
    writeFile(dir / "colours.bmp", bmp(W, H, rgb));
    writeFile(dir / "colours.tga", tga(W, H, rgb));
    for (const char* f : {"colours.bmp", "colours.tga"}) {
      Picture p;
      const bool ok = loadPictureFile((dir / f).u8string(), 23, p);
      char n[96];
      std::snprintf(n, sizeof n, "%s: 30 x 20 loads with exact colours", f);
      expect(ok && p.w == W && p.h == H && p.rgb == rgb, n);
    }
  }
  // GIF (first frame): a hand-made 2 x 1 picture, red then blue.
  {
    const unsigned char gif[] = {'G', 'I', 'F', '8', '9', 'a', 2, 0, 1, 0, 0x80, 0, 0,  // screen 2 x 1, 2-colour table
                                 255, 0, 0, 0, 0, 255,                                 // red, blue
                                 0x2C, 0, 0, 0, 0, 2, 0, 1, 0, 0,                      // image 2 x 1
                                 2, 2, 0x44, 0x0A, 0, 0x3B};
    writeFile(dir / "two.gif", std::string(reinterpret_cast<const char*>(gif), sizeof gif));
    Picture p;
    std::string err;
    const bool ok = loadPictureFile((dir / "two.gif").u8string(), 23, p, &err);
    expect(ok && p.w == 2 && p.h == 1 && at(p, 0, 0)[0] == 255 && at(p, 1, 0)[2] == 255 && at(p, 1, 0)[0] == 0, "gif: first frame decodes");
    if (!ok) std::printf("# gif: %s\n", err.c_str());
  }
  // Refused: corrupt, truncated, empty, missing, not a picture.
  {
    std::string err;
    Picture p;
    std::mt19937 rng(7);
    std::string noise(4096, '\0');
    for (char& c : noise) c = static_cast<char>(rng() & 255);
    writeFile(dir / "noise.png", noise);
    expect(!loadPictureFile((dir / "noise.png").u8string(), 23, p, &err) && !err.empty() && p.w == 0, "corrupt: random bytes are refused with a reason");
    std::vector<uint8_t> rgb(static_cast<size_t>(64 * 64 * 3), 90);
    std::string cut = png(64, 64, 3, rgb);
    cut.resize(cut.size() / 2);
    writeFile(dir / "cut.png", cut);
    err.clear();
    expect(!loadPictureFile((dir / "cut.png").u8string(), 23, p, &err) && !err.empty(), "corrupt: a truncated PNG is refused");
    writeFile(dir / "empty.jpg", "");
    expect(!loadPictureFile((dir / "empty.jpg").u8string(), 23, p, &err), "corrupt: an empty file is refused");
    expect(!loadPictureFile((dir / "missing.png").u8string(), 23, p, &err) && !err.empty(), "corrupt: a missing file is refused");
    writeFile(dir / "notes.txt", "hello");
    expect(!loadPictureFile((dir / "notes.txt").u8string(), 23, p, &err), "corrupt: a text file is refused");
    expect(!isPictureFile("a.txt") && isPictureFile("b.PNG") && isPictureFile("c.hdr") && isPictureFile("d.psd"), "extensions");
  }
}

void testLargePhoto(const fs::path& dir) {
  const int W = 4000, H = 3000;
  std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3);
  std::mt19937 rng(11);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      uint8_t* p = &rgb[(static_cast<size_t>(y) * W + static_cast<size_t>(x)) * 3];
      const int n = static_cast<int>(rng() & 31);
      p[0] = static_cast<uint8_t>(std::min(255, x * 255 / W + n)), p[1] = static_cast<uint8_t>(std::min(255, y * 255 / H + n)), p[2] = static_cast<uint8_t>(n * 4);
    }
  const std::string bytes = jpg(W, H, rgb, 90);
  writeFile(dir / "photo.jpg", bytes);
  const auto t0 = std::chrono::steady_clock::now();
  Picture p;
  std::string err;
  const bool ok = loadPictureFile((dir / "photo.jpg").u8string(), 23, p, &err);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("# 4000 x 3000 JPEG (%.1f MB): read, decoded and reduced in %.0f ms\n", bytes.size() / 1048576.0, ms);
  expect(ok && p.w == 31 && p.h == 23, "photo: 4000 x 3000 JPEG becomes 31 x 23");
  expect(ms < 20000, "photo: decoded in reasonable time (worker thread in the app)");
  const auto t1 = std::chrono::steady_clock::now();
  const bool ok2 = loadPictureFile((dir / "photo.jpg").u8string(), 999, p, &err);
  const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  std::printf("# same photo at 999 rows: %d x %d in %.0f ms, picture text %zu bytes\n", p.w, p.h, ms2, formatPicture(p).size());
  expect(ok2 && p.w * p.h <= kMaxPixels && p.h < 999 && formatPicture(p).size() < 1700000, "photo: at 999 rows the pixel budget keeps the state small");
}

void testText() {
  Picture p;
  p.w = 3, p.h = 2;
  p.rgb = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
  const std::string plain = formatPicture(p);
  expect(plain.find("\nN ") == std::string::npos, "text: the built-in picture has no source line");
  p.name = "My photo.jpg", p.srcW = 300, p.srcH = 200;
  Picture q;
  expect(parsePicture(formatPicture(p), q) && q.rgb == p.rgb && q.name == "My photo.jpg" && q.srcW == 300 && q.srcH == 200, "text: name and source size round trip");
  expect(parsePicture(plain, q) && q.name.empty() && q.srcW == 0 && q.rgb == p.rgb, "text: older pictures parse as before");
  p.name = "two\nlines.png";
  expect(parsePicture(formatPicture(p), q) && q.name == "two lines.png" && q.rgb == p.rgb, "text: a newline in the name cannot break the picture");
  // Edits.
  Picture e = p;
  flipColumns(e);
  expect(at(e, 0, 0)[0] == 7 && at(e, 2, 0)[0] == 1 && at(e, 1, 1)[0] == 13, "edit: reverse mirrors the columns");
  flipColumns(e);
  flipRows(e);
  expect(at(e, 0, 0)[0] == 10 && at(e, 0, 1)[0] == 1, "edit: flip turns the rows over");
  invertColours(e);
  expect(at(e, 0, 0)[0] == 245 && e.name == p.name, "edit: invert makes the negative, the name stays");
}

void testSound(const fs::path& dir) {
  PluginState s = defaultPluginState("gl.pixeltone");
  expect(s.type == "gl.pixeltone" && s.params.size() == PxCount, "sound: Pixel Tone exists");
  const Buf def = render(s, 0.5);
  // A loaded picture: bright rows near the bottom (low partials).
  const int W = 80, H = 60;
  std::vector<uint8_t> rgb(static_cast<size_t>(W * H * 3), 0);
  for (int y = 40; y < 60; ++y)
    for (int x = 0; x < W; ++x) rgb[static_cast<size_t>((y * W + x) * 3)] = 255, rgb[static_cast<size_t>((y * W + x) * 3 + 1)] = 200;
  writeFile(dir / "low.png", png(W, H, 3, rgb));
  Picture p;
  expect(loadPictureFile((dir / "low.png").u8string(), 23, p), "sound: picture loads");
  PluginState q = s;
  q.data = formatPicture(p);
  const Buf low = render(q, 0.5);
  double diff = 0;
  for (size_t i = 0; i < low.l.size(); ++i) diff += std::fabs(static_cast<double>(low.l[i] - def.l[i]));
  expect(low.finite() && low.rms() > 1e-3 && diff > 1.0, "sound: a loaded picture sounds, and differently from the built-in one");
  // Black picture: silence.
  std::fill(rgb.begin(), rgb.end(), 0);
  writeFile(dir / "black.bmp", bmp(W, H, rgb));
  expect(loadPictureFile((dir / "black.bmp").u8string(), 23, p), "sound: black picture loads");
  q.data = formatPicture(p);
  const Buf black = render(q, 0.5);
  expect(black.finite() && black.rms() == 0.0, "sound: a black picture is silent");
  // A tall picture loaded at 200 rows, played with Max rows 23: still fine (squeezed when prepared).
  std::vector<uint8_t> tall(static_cast<size_t>(20 * 400 * 3), 180);
  writeFile(dir / "tall.png", png(20, 400, 3, tall));
  expect(loadPictureFile((dir / "tall.png").u8string(), 200, p) && p.h == 200, "sound: tall picture loads at 200 rows");
  q.data = formatPicture(p);
  q.params[PxMaxHeight] = 23;
  const Buf sq = render(q, 0.3, 64);
  expect(sq.finite() && sq.rms() > 1e-4, "sound: a taller stored picture plays squeezed to Max rows");
  // Old states keep working: short parameter vector, built-in picture.
  PluginState old;
  old.type = "gl.pixeltone";
  old.params = {7200, 2048, 0};
  normalizePluginState(old);
  expect(old.params.size() == PxCount && old.data.empty() && render(old, 0.2).rms() > 1e-4, "sound: an old short state is completed and plays the built-in picture");
}

void testDrops() {
  using browse::DropAction;
  using browse::DropTarget;
  using browse::Kind;
  expect(browse::kindOf("/x/Photo.JPG") == Kind::Image && browse::kindOf("/x/a.png") == Kind::Image && browse::listable(Kind::Image),
         "browser: pictures are listed");
  browse::DropItem img;
  img.kind = Kind::Image;
  expect(browse::dropAction(img, DropTarget::PluginWindow, "gl.pixeltone") == DropAction::LoadPicture, "drop: a Pixel Tone window loads a picture");
  expect(browse::dropAction(img, DropTarget::Rack, "gl.pixeltone") == DropAction::LoadPicture, "drop: a Pixel Tone channel row loads a picture");
  expect(browse::dropAction(img, DropTarget::PluginWindow, "gl.sampler", true) == DropAction::None &&
             browse::dropAction(img, DropTarget::Rack, "") == DropAction::None && browse::dropAction(img, DropTarget::Playlist) == DropAction::None &&
             browse::dropAction(img, DropTarget::Workspace) == DropAction::None,
         "drop: pictures do nothing elsewhere");
  auto file = [](const std::string& n) {
    xdrop::Item it;
    it.path = "/x/" + n;
    it.name = n;
    return it;
  };
  std::vector<xdrop::Item> items = {file("sunset.jpg"), file("moon.png")};
  std::vector<browse::DropItem> kinds;
  for (const auto& it : items) {
    browse::DropItem d;
    d.kind = xdrop::kindOf(it);
    kinds.push_back(d);
  }
  xdrop::Context c;
  c.target = DropTarget::PluginWindow;
  c.targetType = "gl.pixeltone";
  c.targetName = "Pixel Tone";
  xdrop::Plan p = xdrop::plan(items, kinds, c, {});
  expect(p.accepted() && p.steps.size() == 1 && p.steps[0].action == DropAction::LoadPicture && p.steps[0].item == 0 && p.skipped.size() == 1,
         "plan: the first picture loads, the second is left out");
  expect(p.label.find("load the picture sunset.jpg into Pixel Tone") != std::string::npos, "plan: the hint says what happens");
  c.target = DropTarget::Playlist;
  c.targetType.clear();
  p = xdrop::plan(items, kinds, c, {});
  expect(!p.accepted() && p.unsupported.size() == 2 && p.label == "Gridline cannot open 2 files", "plan: pictures elsewhere are refused as before");
}

}  // namespace

int main(int argc, char** argv) {
  // App test helper (j184_pixeltone_pictures.cmake): "writepng <file>" writes a 160 x 90 picture
  // (a sunset: warm sky over a dark sea, a bright sun), "writebad <file>" a corrupt one.
  if (argc == 3 && std::string(argv[1]) == "writepng") {
    const int W = 160, H = 90;
    std::vector<uint8_t> rgb(static_cast<size_t>(W * H * 3));
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        uint8_t* p = &rgb[static_cast<size_t>((y * W + x) * 3)];
        const double dx = x - 110.0, dy = y - 38.0, sun = std::exp(-(dx * dx + dy * dy) / 120.0);
        const double sky = y < 50 ? 1.0 - y / 70.0 : 0.15 * std::fabs(std::sin(x * 0.3 + y * 0.9));
        p[0] = static_cast<uint8_t>(std::min(255.0, 255 * (0.9 * sky + sun)));
        p[1] = static_cast<uint8_t>(std::min(255.0, 255 * (0.35 * sky * (1.0 - y / 90.0) + 0.9 * sun)));
        p[2] = static_cast<uint8_t>(std::min(255.0, 255 * (y < 50 ? 0.25 * (1 - sky) : 0.4 * sky) + 120 * sun));
      }
    writeFile(fs::u8path(argv[2]), png(W, H, 3, rgb));
    return fs::exists(fs::u8path(argv[2])) ? 0 : 1;
  }
  if (argc == 3 && std::string(argv[1]) == "writebad") {
    writeFile(fs::u8path(argv[2]), std::string("\x89PNG\r\n\x1a\nthis is not really a picture"));
    return 0;
  }
  const fs::path dir = fs::temp_directory_path() / ("gridline_test_pixeltone_" + std::to_string(std::random_device{}()));
  fs::create_directories(dir);
  testSizes();
  testFormats(dir);
  testLargePhoto(dir);
  testText();
  testSound(dir);
  testDrops();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return finish();
}
