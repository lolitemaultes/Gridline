// Gridline — J-75: external drag & drop, platform-independent part (see ExternalDrop.h).
#include "ExternalDrop.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include "I18n.h"
#include "Platform.h"

namespace gl::xdrop {

namespace fs = std::filesystem;
using browse::DropAction;
using browse::DropTarget;
using browse::Kind;

namespace {
// J-103: never destroyed: detached download / file-promise threads (curl, URLDownloadToFile,
// NSURLSession handlers) post their result whenever it arrives, even during static teardown.
std::mutex& gMu = *new std::mutex;
std::vector<Event>& gQueue = *new std::vector<Event>;
std::string& gDropFolder = *new std::string;
std::atomic<uint64_t> gToken{0};
std::atomic<bool> gAccepted{false};
std::atomic<bool> gCancelled{false};

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string fmt(const char* f, ...) GL_PRINTF_FORMAT(1, 2);
std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percentDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
      out += static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}
}  // namespace

// ------------------------------------------------------------------ items

Kind kindOf(const Item& it) {
  if (it.dir) return Kind::Folder;
  const std::string& p = it.path.empty() ? it.name : it.path;
  const Kind k = browse::kindOf(p);
  if (k == Kind::Other && it.pending && it.guessed) return Kind::Audio;
  return k;
}

std::string displayName(const Item& it) {
  if (!it.name.empty()) return it.name;
  return fs::path(it.path).filename().string();
}

// ----------------------------------------------------------------- events

void post(const Event& e) {
  std::lock_guard<std::mutex> l(gMu);
  gQueue.push_back(e);
}

std::vector<Event> take() {
  std::lock_guard<std::mutex> l(gMu);
  std::vector<Event> out;
  out.swap(gQueue);
  return out;
}

uint64_t newToken() { return ++gToken; }

void setDropFolder(const std::string& dir) {
  std::lock_guard<std::mutex> l(gMu);
  gDropFolder = dir;
}

std::string dropFolder() {
  std::lock_guard<std::mutex> l(gMu);
  return gDropFolder;
}

std::string uniquePath(const std::string& dir, const std::string& name) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::string clean;
  for (char c : name) clean += (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) ? '_' : c;
  if (clean.empty() || clean == "." || clean == "..") clean = "Dropped file";
  const fs::path base(clean);
  const std::string stem = base.stem().string(), ext = base.extension().string();
  fs::path p = fs::path(dir) / clean;
  for (int n = 2; fs::exists(p, ec) && n < 10000; ++n) p = fs::path(dir) / (stem + " (" + std::to_string(n) + ")" + ext);
  return p.string();
}

void setHoverAccepted(bool on) { gAccepted = on; }
bool hoverAccepted() { return gAccepted; }
void setCancelled(bool on) { gCancelled = on; }
bool cancelled() { return gCancelled; }

// ---------------------------------------------------------------- formats

const std::vector<std::string>& audioUtis() {
  static const std::vector<std::string> u = {"com.microsoft.waveform-audio", "public.wav", "org.xiph.flac", "public.mp3",
                                             "org.xiph.ogg-vorbis", "public.aiff-audio", "public.aifc-audio", "com.apple.m4a-audio",
                                             "public.mpeg-4-audio", "public.midi-audio"};
  return u;
}

std::string extensionForUti(const std::string& uti) {
  static const std::map<std::string, std::string> m = {
      {"com.microsoft.waveform-audio", ".wav"}, {"public.wav", ".wav"},         {"com.microsoft.wave", ".wav"},
      {"org.xiph.flac", ".flac"},              {"public.flac", ".flac"},        {"public.mp3", ".mp3"},
      {"org.xiph.ogg-vorbis", ".ogg"},         {"org.xiph.ogg", ".ogg"},        {"public.ogg-vorbis", ".ogg"},
      {"com.wavpack.wv", ".wv"},               {"public.aiff-audio", ".aiff"},  {"public.aifc-audio", ".aifc"},
      {"com.apple.m4a-audio", ".m4a"},         {"public.mpeg-4-audio", ".m4a"}, {"public.midi-audio", ".mid"},
      {"public.midi", ".mid"},                 {"com.apple.coreaudio-format", ".caf"},
      {"public.zip-archive", ".zip"},          {"public.json", ".json"},        {"public.folder", ""},
      {"public.jpeg", ".jpg"},                 {"public.png", ".png"},          {"com.adobe.pdf", ".pdf"},
      {"public.plain-text", ".txt"},
  };
  auto it = m.find(lower(uti));
  if (it != m.end()) return it->second;
  // Dynamic identifiers of a known extension: "dyn.…" carry no readable
  // extension; "public.filename-extension.wav"-like forms do.
  const std::string l = lower(uti);
  const std::string tag = "filename-extension.";
  if (size_t p = l.find(tag); p != std::string::npos) return "." + l.substr(p + tag.size());
  return "";
}

bool isWebUrl(const std::string& s) {
  const std::string l = lower(s.substr(0, 8));
  return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
}

std::string pathFromFileUrl(const std::string& url) {
  if (lower(url.substr(0, 7)) != "file://") return "";
  std::string rest = url.substr(7);
  if (rest.rfind("localhost/", 0) == 0) rest = rest.substr(9);
  if (rest.empty() || rest[0] != '/') return "";
  std::string p = percentDecode(rest);
#if defined(_WIN32)
  if (p.size() > 2 && p[0] == '/' && p[2] == ':') p = p.substr(1);  // file:///C:/x
#endif
  return p;
}

std::string fileNameForUrl(const std::string& url) {
  std::string u = url;
  if (size_t q = u.find_first_of("?#"); q != std::string::npos) u = u.substr(0, q);
  if (size_t s = u.find("://"); s != std::string::npos) u = u.substr(s + 3);
  const size_t slash = u.find('/');
  std::string path = slash == std::string::npos ? std::string() : u.substr(slash);
  while (!path.empty() && path.back() == '/') path.pop_back();
  std::string name = percentDecode(path.substr(path.find_last_of('/') + 1));
  if (name.empty()) name = "Download";
  return name;
}

namespace {
uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

double audioHeaderSeconds(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return -1;
  unsigned char h[12];
  if (!in.read(reinterpret_cast<char*>(h), 12)) return -1;
  if (std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVE", 4) == 0) {
    uint32_t rate = 0, blockAlign = 0;
    for (int guard = 0; guard < 64; ++guard) {
      unsigned char c[8];
      if (!in.read(reinterpret_cast<char*>(c), 8)) return -1;
      const uint32_t size = le32(c + 4);
      if (std::memcmp(c, "fmt ", 4) == 0) {
        unsigned char f[16];
        if (size < 16 || !in.read(reinterpret_cast<char*>(f), 16)) return -1;
        rate = le32(f + 4);
        blockAlign = le16(f + 12);
        in.seekg(static_cast<std::streamoff>(size - 16 + (size & 1)), std::ios::cur);
      } else if (std::memcmp(c, "data", 4) == 0) {
        if (!rate || !blockAlign) return -1;
        return static_cast<double>(size / blockAlign) / rate;
      } else {
        in.seekg(static_cast<std::streamoff>(size + (size & 1)), std::ios::cur);
      }
    }
    return -1;
  }
  if (std::memcmp(h, "fLaC", 4) == 0) {
    // First metadata block (STREAMINFO) starts at byte 4: 4-byte header, then
    // 10 bytes of block sizes, 20 bits rate, 3 channels, 5 bps, 36 total samples.
    in.seekg(8);
    unsigned char s[18];
    if (!in.read(reinterpret_cast<char*>(s), 18)) return -1;
    const uint32_t rate = (static_cast<uint32_t>(s[10]) << 12) | (s[11] << 4) | (s[12] >> 4);
    const uint64_t total = (static_cast<uint64_t>(s[13] & 0x0F) << 32) | (static_cast<uint64_t>(s[14]) << 24) | (s[15] << 16) | (s[16] << 8) | s[17];
    if (!rate || !total) return -1;
    return static_cast<double>(total) / rate;
  }
  return -1;
}

// ---------------------------------------------------------------- layout

std::vector<Place> layoutClips(const std::vector<Tick>& lengths, int track, Tick tick, bool sequential, int trackCount) {
  std::vector<Place> out;
  const int last = std::max(0, trackCount - 1);
  Tick at = std::max<Tick>(0, tick);
  for (size_t i = 0; i < lengths.size(); ++i) {
    if (sequential) {
      out.push_back({std::clamp(track, 0, last), at});
      at += std::max<Tick>(1, lengths[i]);
    } else {
      out.push_back({std::clamp(track + static_cast<int>(i), 0, last), at});
    }
  }
  return out;
}

// --------------------------------------------------------------- planning

bool autoChannelName(const std::string& name, const std::string& pluginName, const std::string& oldData) {
  if (name.empty() || name == pluginName) return true;
  if (oldData.empty() || oldData.size() > 4096 || oldData.find('\n') != std::string::npos) return false;
  const std::string stem = fs::path(oldData).stem().string();
  return !stem.empty() && name == stem;
}

SampleSlot sampleSlot(const std::string& type) {
  // J-107: Seltmann is a sample player too (a bare path in its data = a new file to analyse)
  if (type == "gl.sampler" || type == "gl.slicer" || type == "gl.granular" || type == "gl.synth" || type == "gl.seltmann") return SampleSlot::Whole;
  if (type == "gl.scratch" || type == "gl.grainscan" || type == "gl.splinescratch" || type == "gl.convolver" || type == "gl.platter") return SampleSlot::FirstLine;
  return SampleSlot::None;
}

std::string withSample(const std::string& type, const std::string& data, const std::string& path) {
  if (sampleSlot(type) != SampleSlot::FirstLine) return path;
  const size_t nl = data.find('\n');
  return nl == std::string::npos ? path : path + data.substr(nl);
}

bool rowTakesSample(const std::string& type) { return type != "gl.synth" && sampleSlot(type) != SampleSlot::None; }

RowZone rowZone(float y, float rowTop, float rowHeight) {
  const float band = std::max(4.0f, rowHeight * 0.25f);
  if (y < rowTop + band) return RowZone::Before;
  if (y >= rowTop + rowHeight - band) return RowZone::After;
  return RowZone::Replace;
}

namespace {

bool isProject(Kind k) { return k == Kind::Project || k == Kind::Flp; }

// Short description of one or many items ("Kick.wav" / "3 files").
std::string what(const std::vector<Item>& items, const std::vector<size_t>& idx) {
  if (idx.size() == 1) return displayName(items[idx[0]]);
  return fmt(i18n::tr("%d files"), static_cast<int>(idx.size()));
}

}  // namespace

Plan plan(const std::vector<Item>& items, const std::vector<browse::DropItem>& kinds, const Context& ctx, const Mods& mods) {
  Plan p;
  // Projects open wherever they are dropped; nothing else of the drop happens.
  for (size_t i = 0; i < items.size() && i < kinds.size(); ++i)
    if (isProject(kinds[i].kind)) {
      p.steps.push_back({i, DropAction::OpenProject, 0, 0, 0});
      for (size_t j = 0; j < items.size(); ++j)
        if (j != i) p.skipped.push_back(j);
      p.label = fmt(i18n::tr("Drop to open the project %s"), displayName(items[i]).c_str());
      if (!p.skipped.empty()) p.label += fmt(i18n::tr(" (%d other files ignored)"), static_cast<int>(p.skipped.size()));
      return p;
    }

  bool sampleTaken = false;   // single-sample targets take the first sample only
  bool rowTaken = false;      // J-97: a channel row is replaced by the first sample / preset only
  bool pictureTaken = false;  // J-184: Pixel Tone takes the first picture only
  for (size_t i = 0; i < items.size() && i < kinds.size(); ++i) {
    const browse::DropItem& d = kinds[i];
    if (d.kind == Kind::Other || d.kind == Kind::Shape || d.kind == Kind::Score) {
      p.unsupported.push_back(i);
      continue;
    }
    DropAction act = browse::dropAction(d, ctx.target, ctx.targetType, ctx.targetUsesSample);
    if (d.kind == Kind::Image && act == DropAction::None) {  // J-184: pictures only open in Pixel Tone; elsewhere as before
      p.unsupported.push_back(i);
      continue;
    }
    // On a channel row: Shift (or a second sample / preset) makes new channels instead of replacing.
    const bool replaces = act == DropAction::ReplaceSample || act == DropAction::ReplaceInstrument || act == DropAction::ReplaceFromPreset;
    if (replaces && (mods.shift || rowTaken))
      act = act == DropAction::ReplaceFromPreset ? DropAction::NewChannelFromPreset : DropAction::NewSamplerChannel;
    else if (replaces)
      rowTaken = true;
    if (act == DropAction::LoadSample) {
      if (sampleTaken) act = DropAction::None;
      else sampleTaken = true;
    }
    if (act == DropAction::LoadPicture) {  // J-184: one picture
      if (pictureTaken) act = DropAction::None;
      else pictureTaken = true;
    }
    if (act == DropAction::None) {
      p.skipped.push_back(i);
      continue;
    }
    p.steps.push_back({i, act, ctx.track, ctx.tick, 0});
  }
  // Clips (audio and MIDI pattern clips) of a playlist drop: stacked or in a row.
  {
    std::vector<size_t> placed;
    std::vector<Tick> lens;
    for (Step& s : p.steps)
      if (s.action == DropAction::AudioClip || s.action == DropAction::MidiPatternClip) {
        placed.push_back(static_cast<size_t>(&s - p.steps.data()));
        const Tick l = s.item < ctx.lengths.size() ? ctx.lengths[s.item] : 0;
        lens.push_back(l > 0 ? l : std::max<Tick>(1, ctx.fallbackLength));
      }
    const std::vector<Place> at = layoutClips(lens, ctx.track, ctx.tick, mods.shift, ctx.trackCount);
    for (size_t k = 0; k < placed.size(); ++k) {
      Step& s = p.steps[placed[k]];
      s.track = at[k].track;
      s.tick = at[k].tick;
      s.length = lens[k];
    }
  }

  // ---- hint text ----
  std::map<DropAction, std::vector<size_t>> by;
  for (const Step& s : p.steps) by[s.action].push_back(s.item);
  std::vector<std::string> parts;
  const int bar = static_cast<int>(std::max<Tick>(0, ctx.tick) / std::max<Tick>(1, ctx.barTicks)) + 1;
  for (const auto& [act, idx] : by) {
    const int n = static_cast<int>(idx.size());
    const std::string w = what(items, idx);
    switch (act) {
      case DropAction::AudioClip: {
        int lastTrack = ctx.track;
        for (const Step& s : p.steps)
          if (s.action == act) lastTrack = std::max(lastTrack, s.track);
        if (n == 1)
          parts.push_back(fmt(i18n::tr("add an audio clip on track %d at bar %d"), ctx.track + 1, bar));
        else if (mods.shift || lastTrack == ctx.track)
          parts.push_back(fmt(i18n::tr("add %d audio clips one after another on track %d from bar %d"), n, ctx.track + 1, bar));
        else
          parts.push_back(fmt(i18n::tr("add %d audio clips on tracks %d-%d at bar %d"), n, ctx.track + 1, lastTrack + 1, bar));
        break;
      }
      case DropAction::NewSamplerChannel:
        if (ctx.insertHere)
          parts.push_back(n == 1 ? std::string(i18n::tr("insert a sampler channel here")) : fmt(i18n::tr("insert %d sampler channels here"), n));
        else
          parts.push_back(n == 1 ? std::string(i18n::tr("add a sampler channel")) : fmt(i18n::tr("add %d sampler channels"), n));
        break;
      case DropAction::ReplaceSample:
        parts.push_back(ctx.targetName.empty() ? fmt(i18n::tr("replace the channel's sample with %s"), w.c_str())
                                               : fmt(i18n::tr("replace the sample of %s with %s"), ctx.targetName.c_str(), w.c_str()));
        break;
      case DropAction::ReplaceInstrument:
        parts.push_back(ctx.targetName.empty() ? fmt(i18n::tr("replace the channel's instrument with a sampler playing %s"), w.c_str())
                                               : fmt(i18n::tr("replace %s with a sampler playing %s"), ctx.targetName.c_str(), w.c_str()));
        break;
      case DropAction::ReplaceFromPreset:
        parts.push_back(ctx.targetName.empty() ? fmt(i18n::tr("replace the channel's instrument with the preset %s"), w.c_str())
                                               : fmt(i18n::tr("replace %s with the preset %s"), ctx.targetName.c_str(), w.c_str()));
        break;
      case DropAction::LoadSample:
        parts.push_back(ctx.targetName.empty() ? fmt(i18n::tr("load %s as the sample"), w.c_str())
                                               : fmt(i18n::tr("load %s into %s"), w.c_str(), ctx.targetName.c_str()));
        break;
      case DropAction::ConvolverIR: parts.push_back(fmt(i18n::tr("load %s as an impulse response"), w.c_str())); break;
      case DropAction::ApplyPreset: parts.push_back(fmt(i18n::tr("load the preset %s"), w.c_str())); break;
      case DropAction::NewChannelFromPreset:
        if (ctx.insertHere)
          parts.push_back(n == 1 ? fmt(i18n::tr("insert a channel from the preset %s here"), w.c_str()) : fmt(i18n::tr("insert %d channels from presets here"), n));
        else
          parts.push_back(n == 1 ? fmt(i18n::tr("add a channel from the preset %s"), w.c_str()) : fmt(i18n::tr("add %d channels from presets"), n));
        break;
      case DropAction::InsertEffectPreset: parts.push_back(fmt(i18n::tr("insert the effect preset %s"), w.c_str())); break;
      case DropAction::ApplyChain: parts.push_back(fmt(i18n::tr("load the effect chain %s"), w.c_str())); break;
      case DropAction::ImportMidiChannels:
        parts.push_back(n == 1 ? fmt(i18n::tr("import %s as a pattern"), w.c_str()) : fmt(i18n::tr("import %d MIDI files as patterns"), n));
        break;
      case DropAction::MidiPatternClip:
        parts.push_back(n == 1 ? fmt(i18n::tr("import %s as a pattern clip on track %d at bar %d"), w.c_str(), ctx.track + 1, bar)
                               : fmt(i18n::tr("import %d MIDI files as pattern clips from track %d at bar %d"), n, ctx.track + 1, bar));
        break;
      case DropAction::ImportMidiNotes:
        parts.push_back(ctx.onChannel && !ctx.targetName.empty() ? fmt(i18n::tr("insert the notes of %s into %s"), w.c_str(), ctx.targetName.c_str())
                                                                 : fmt(i18n::tr("insert the notes of %s"), w.c_str()));
        break;
      case DropAction::AddBrowserFolder:
        parts.push_back(n == 1 ? fmt(i18n::tr("add the folder %s to the browser"), w.c_str()) : fmt(i18n::tr("add %d folders to the browser"), n));
        break;
      case DropAction::LoadPicture:
        parts.push_back(ctx.targetName.empty() ? fmt(i18n::tr("load %s as the picture"), w.c_str())
                                               : fmt(i18n::tr("load the picture %s into %s"), w.c_str(), ctx.targetName.c_str()));
        break;
      case DropAction::OpenProject:
      case DropAction::None: break;
    }
  }
  if (!parts.empty()) {
    p.label = i18n::tr("Drop to ");
    for (size_t k = 0; k < parts.size(); ++k) p.label += (k ? i18n::tr(" and ") : "") + parts[k];
    const int left = static_cast<int>(p.unsupported.size() + p.skipped.size());
    if (left == 1) p.label += i18n::tr(" (1 file left out)");
    if (left > 1) p.label += fmt(i18n::tr(" (%d files left out)"), left);
  } else if (!p.skipped.empty()) {
    p.label = fmt(i18n::tr("%s cannot be dropped here"), what(items, p.skipped).c_str());
  } else if (!p.unsupported.empty()) {
    p.label = fmt(i18n::tr("Gridline cannot open %s"), what(items, p.unsupported).c_str());
  }
  return p;
}

// ------------------------------------------------------- platform fallback
#if !defined(__APPLE__) && !defined(_WIN32)  // Windows: host/ui/ExternalDropWin.cpp
namespace native {
bool install(void*) { return false; }
void poll() {}
// No system download service here: `curl` when installed (Linux), on a
// background thread. Untested on Windows.
void download(uint64_t token, size_t index, const std::string& url) {
  std::thread([token, index, url] {
    const std::string dest = uniquePath(dropFolder(), fileNameForUrl(url));
    auto quote = [](const std::string& s) {
      std::string q = "'";
      for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
      return q + "'";
    };
    Event e;
    e.token = token;
    e.index = index;
    const std::string cmd = "curl -fsSL --max-time 300 -o " + quote(dest) + " " + quote(url) + " >/dev/null 2>&1";
    if (std::system(cmd.c_str()) == 0) {
      e.type = EventType::Arrived;
      e.path = dest;
    } else {
      e.type = EventType::Failed;
      e.error = "download failed";
      std::error_code ec;
      fs::remove(dest, ec);
    }
    post(e);
  }).detach();
}
}  // namespace native
#endif

}  // namespace gl::xdrop
