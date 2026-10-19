// Gridline — plugin editor thumbnails for the plugin picker. A child process
// (--make-thumbs, off-screen) opens every built-in instrument / effect on the
// default project, draws its editor once in a bare window at the top-left,
// reads those pixels back and stores a scaled PNG in <config>/thumbs/. The
// picker loads them lazily as textures.
#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <thread>
#include <vector>
#include "../core/Png.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "BrowserUi.h"
#include "PluginUI.h"
#include "Theme.h"

namespace gl::browserui {

namespace fs = std::filesystem;

namespace {

std::string safeName(const std::string& type) {
  std::string s;
  for (char c : type) s += std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' ? c : '_';
  return s;
}

struct Tex {
  GLuint id = 0;
  ImVec2 size;
  bool tried = false;
};
std::map<std::string, Tex> textures;
std::atomic<uint64_t> texGeneration{0};
uint64_t seenGeneration = 0;

std::atomic<bool> runActive{false};

// ---- --make-thumbs mode ----
struct ThumbRun {
  bool on = false;
  std::vector<std::string> types;
  size_t idx = 0;
  int phase = 0;
  PluginTarget target;
  ObjectId channel = kNoId;
  ImVec2 pos, size;  // editor window rect (logical px)
  bool drawn = false;
  bool setup = false;  // the current plugin's channel / slot exists
  int written = 0;
};
ThumbRun run;

}  // namespace

std::string thumbPath(App& a, const std::string& type) {
  return (fs::path(a.configDir()) / "thumbs" / (safeName(type) + ".png")).string();
}

ImTextureID pluginThumb(App& a, const std::string& type, ImVec2* size) {
  if (texGeneration.load() != seenGeneration) {  // a thumbnail run finished: reload
    seenGeneration = texGeneration.load();
    for (auto& [t, x] : textures)
      if (!x.id) x.tried = false;
  }
  Tex& t = textures[type];
  if (!t.tried) {
    t.tried = true;
    png::Image img;
    if (png::readFile(thumbPath(a, type), img)) {
      std::vector<uint8_t> rgba(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4, 255);
      for (size_t i = 0, n = static_cast<size_t>(img.width) * static_cast<size_t>(img.height); i < n; ++i)
        for (size_t c = 0; c < static_cast<size_t>(img.channels) && c < 4; ++c) rgba[i * 4 + c] = img.pixels[i * static_cast<size_t>(img.channels) + c];
      GLint last = 0;
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
      glGenTextures(1, &t.id);
      glBindTexture(GL_TEXTURE_2D, t.id);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
      t.size = ImVec2(static_cast<float>(img.width), static_cast<float>(img.height));
    }
  }
  if (size) *size = t.size;
  return t.id ? static_cast<ImTextureID>(t.id) : ImTextureID_Invalid;
}

void startThumbnailRun(App& a) {
  if (runActive.exchange(true)) return;
  const std::string exe = host::selfExecutablePath();
  const fs::path dir = fs::path(a.configDir()) / "thumbs";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string frame = (dir / ".frame.ppm").string();
  std::string cmd = "'" + exe + "' --make-thumbs --size 1400x1000 --screenshot '" + frame + "' --frames 100000 > /dev/null 2>&1";
  a.notify("Rendering plugin thumbnails in the background");
  std::thread([cmd]() {
    int rc = std::system(cmd.c_str());
    (void)rc;
    ++texGeneration;
    runActive = false;
  }).detach();
}

bool thumbnailRunActive() { return runActive.load(); }

// ------------------------------------------------------------ thumb mode

void beginThumbMode(App& a, const std::string& onlyType) {
  run = ThumbRun{};
  run.on = true;
  for (const auto& p : pluginCatalog())
    if (p.kind != PluginKind::NoteFx && (onlyType.empty() || p.type == onlyType)) run.types.push_back(p.type);
  std::error_code ec;
  fs::create_directories(fs::path(a.configDir()) / "thumbs", ec);
  a.showBrowser = a.showRack = a.showPiano = a.showPlaylist = a.showMixer = false;
}

bool thumbMode() { return run.on; }

void thumbDraw(App& a) {
  run.drawn = false;
  if (!run.on || run.idx >= run.types.size()) return;
  const std::string& type = run.types[run.idx];
  const PluginInfo* info = findPlugin(type);
  if (!info) return;
  if (run.phase == 0) {
    run.setup = false;
    a.settingsChannel = kNoId;
    a.fxEditor = {kNoId, -1};
    if (info->kind == PluginKind::Generator) {
      run.channel = a.addInstrument(type);
      run.target = PluginTarget{run.channel, -1};
      a.settingsChannel = kNoId;
      run.setup = run.channel != kNoId;
    } else {
      const Song& s = a.doc.song();
      if (s.mixer.size() < 2) return;
      a.doc.setSlotPlugin(s.mixer[1].id, 0, type);
      run.target = PluginTarget{s.mixer[1].id, 0};
      run.channel = kNoId;
      run.setup = true;
    }
    a.syncEngine();
  }
  const float width = info->editorWidth > 0 ? info->editorWidth : (type == "gl.eq" ? 780.0f : 600.0f);
  ImGuiViewport* vp = ImGui::GetMainViewport();
  if (!run.setup) return;
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSizeConstraints(ImVec2(width + 16, 0), ImVec2(width + 16, vp->Size.y));
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  if (ImGui::Begin("##thumbgen", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
    drawPluginBody(a, run.target, width);
    run.pos = ImGui::GetWindowPos();
    run.size = ImGui::GetWindowSize();
    run.drawn = true;
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

bool thumbCapture(App& a, int fbW, int fbH) {
  if (!run.on) return true;
  if (run.idx >= run.types.size()) {
    std::fprintf(stderr, "thumbnails: %d written\n", run.written);
    return false;
  }
  if (run.phase >= 4) {
    const ImVec2 sc = ImGui::GetIO().DisplayFramebufferScale;
    int x0 = static_cast<int>(run.pos.x * sc.x), y0 = static_cast<int>(run.pos.y * sc.y);
    int w = std::min(static_cast<int>(run.size.x * sc.x), fbW - x0), h = std::min(static_cast<int>(run.size.y * sc.y), fbH - y0);
    if (run.drawn && w > 8 && h > 8) {
      png::Image img;
      img.width = w;
      img.height = h;
      img.channels = 3;
      img.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
      std::vector<uint8_t> raw(img.pixels.size());
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(x0, fbH - y0 - h, w, h, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
      for (int y = 0; y < h; ++y)
        std::copy_n(raw.data() + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 3, static_cast<size_t>(w) * 3,
                    img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3);
      // 320 px wide, aspect kept (tall editors are cut at 3:2).
      const int tw = 320;
      int th = std::max(1, h * tw / w);
      png::Image small = png::resize(img, tw, th);
      if (small.height > 260) {
        small.height = 260;
        small.pixels.resize(static_cast<size_t>(small.width) * static_cast<size_t>(small.height) * 3);
      }
      if (png::writeFile(thumbPath(a, run.types[run.idx]), small)) ++run.written;
    }
    // Tear down: remove the channel / clear the slot, next plugin.
    if (run.setup && run.channel != kNoId) a.doc.removeChannel(run.channel);
    else if (run.setup) a.doc.setSlotPlugin(run.target.owner, run.target.slot, "");
    run.setup = false;
    a.syncEngine();
    run.phase = 0;
    ++run.idx;
    return true;
  }
  ++run.phase;
  return true;
}

}  // namespace gl::browserui
