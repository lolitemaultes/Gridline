// Gridline — FLP container reader, section splitter and import driver.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "Flp.h"

namespace gl::flp {

namespace fs = std::filesystem;

uint32_t Event::value() const {
  uint32_t v = 0;
  for (uint32_t i = 0; i < size && i < 4; ++i) v |= static_cast<uint32_t>(data[i]) << (8 * i);
  return v;
}
uint16_t Event::u16(size_t at) const {
  return at + 2 <= size ? static_cast<uint16_t>(data[at] | (data[at + 1] << 8)) : 0;
}
uint32_t Event::u32(size_t at) const {
  if (at + 4 > size) return 0;
  return static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8) |
         (static_cast<uint32_t>(data[at + 2]) << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
}
float Event::f32(size_t at) const {
  uint32_t u = u32(at);
  float f;
  std::memcpy(&f, &u, 4);
  return at + 4 <= size ? f : 0.0f;
}
double Event::f64(size_t at) const {
  if (at + 8 > size) return 0.0;
  uint64_t u = static_cast<uint64_t>(u32(at)) | (static_cast<uint64_t>(u32(at + 4)) << 32);
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

namespace {

void appendUtf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xF0 | (cp >> 18));
    s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

}  // namespace

std::string Event::text() const {
  std::string s;
  // UTF-16LE when even-sized and ends with a 16-bit NUL (or has zero high bytes).
  bool utf16 = size >= 2 && size % 2 == 0 && data[size - 1] == 0 && data[size - 2] == 0;
  if (utf16) {
    for (uint32_t i = 0; i + 1 < size; i += 2) {
      uint32_t cu = data[i] | (data[i + 1] << 8);
      if (cu == 0) break;
      if (cu >= 0xD800 && cu < 0xDC00) {  // high surrogate: needs a following low one
        uint32_t lo = i + 3 < size ? static_cast<uint32_t>(data[i + 2] | (data[i + 3] << 8)) : 0;
        if (lo >= 0xDC00 && lo < 0xE000) {
          cu = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        } else {
          cu = 0xFFFD;  // unpaired: replacement character keeps the text valid UTF-8
        }
      } else if (cu >= 0xDC00 && cu < 0xE000) {
        cu = 0xFFFD;  // stray low surrogate
      }
      appendUtf8(s, cu);
    }
    return s;
  }
  for (uint32_t i = 0; i < size && data[i]; ++i) appendUtf8(s, data[i]);  // Latin-1
  return s;
}

bool readBytes(std::vector<uint8_t> bytes, File& out, std::string* err) {
  out = File{};
  out.bytes = std::move(bytes);
  const auto& d = out.bytes;
  auto fail = [&](const char* m) {
    if (err) *err = m;
    return false;
  };
  auto rd32 = [&](size_t p) { return static_cast<uint32_t>(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (static_cast<uint32_t>(d[p + 3]) << 24)); };
  if (d.size() < 22 || std::memcmp(d.data(), "FLhd", 4) != 0) return fail("not an FL Studio project (no FLhd header)");
  uint32_t hl = rd32(4);
  if (hl < 6 || 8 + hl + 8 > d.size()) return fail("truncated header");
  out.format = static_cast<int16_t>(d[8] | (d[9] << 8));
  out.channelCount = d[10] | (d[11] << 8);
  out.ppq = d[12] | (d[13] << 8);
  size_t p = 8 + hl;
  if (std::memcmp(d.data() + p, "FLdt", 4) != 0) return fail("no FLdt data chunk");
  size_t end = std::min<size_t>(d.size(), p + 8 + rd32(p + 4));
  p += 8;
  while (p < end) {
    Event e;
    e.offset = static_cast<uint32_t>(p);
    e.id = d[p++];
    size_t n;
    if (e.id < 64) {
      n = 1;
    } else if (e.id < 128) {
      n = 2;
    } else if (e.id < 192) {
      n = 4;
      // FL 25.2.3+ writes event 172 with a size selector byte (1 → u16, 0 → u32).
      if (e.id == 172 && p < end) n = d[p] == 1 ? 3 : d[p] == 0 ? 5 : 4;
    } else {
      n = 0;
      int shift = 0;
      while (p < end) {
        uint8_t b = d[p++];
        n |= static_cast<size_t>(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80) || shift > 28) break;
      }
    }
    if (p + n > end) break;  // truncated tail: keep what we have
    e.data = d.data() + p;
    e.size = static_cast<uint32_t>(n);
    out.events.push_back(e);
    p += n;
  }
  for (const auto& e : out.events)
    if (e.id == 199) {
      out.version = e.text();
      break;
    }
  return true;
}

bool readFile(const std::string& path, File& out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return readBytes(std::move(bytes), out, err);
}

const Event* find(const std::vector<Event>& evs, uint8_t id) {
  for (const auto& e : evs)
    if (e.id == id) return &e;
  return nullptr;
}

Sections split(const File& f) {
  Sections s;
  const auto& ev = f.events;
  // Mixer: starts at the first insert-flags event (236), including the
  // insert colour / icon / name events written just before it.
  size_t mixerStart = ev.size();
  for (size_t i = 0; i < ev.size(); ++i)
    if (ev[i].id == 236) {
      mixerStart = i;
      while (mixerStart > 0) {
        uint8_t id = ev[mixerStart - 1].id;
        if (id == 149 || id == 42 || id == 95 || id == 204) --mixerStart;
        else break;
      }
      break;
    }
  enum Owner { Project, Pattern, Channel, Arrangement } owner = Project;
  // Files saved before FL 12.9 have a single playlist and no Arrangement.New
  // events: the playlist (233), its markers (148) and tracks (238) follow the
  // last channel. The first of them after a channel opens arrangement 0.
  s.legacyArrangement = true;
  for (size_t i = 0; i < mixerStart && s.legacyArrangement; ++i) s.legacyArrangement = ev[i].id != 99;
  int idx = 0;
  for (size_t i = 0; i < mixerStart; ++i) {
    const Event& e = ev[i];
    switch (e.id) {
      case 65: owner = Pattern, idx = static_cast<int>(e.value()); break;    // Pattern.New
      case 64: owner = Channel, idx = static_cast<int>(e.value()); break;    // Channel.New
      case 99: owner = Arrangement, idx = static_cast<int>(e.value()); break;  // Arrangement.New
      case 233: case 148: case 238:
        // (A pattern can come last, too; markers inside patterns stay with the pattern.)
        if (s.legacyArrangement && (owner == Channel || (owner == Pattern && e.id != 148))) owner = Arrangement, idx = 0;
        break;
      default: break;
    }
    // Project-wide events that can appear between sections.
    const bool global = e.id == 156 || e.id == 66 || e.id == 93 || e.id == 17 || e.id == 18 || e.id == 80 || e.id == 194 ||
                        e.id == 195 || e.id == 197 || e.id == 206 || e.id == 207 || e.id == 199 || e.id == 159 ||
                        e.id == 231 || e.id == 100 || e.id == 67 || e.id == 11 || e.id == 237;
    if (global) {
      s.project.push_back(e);
      continue;
    }
    switch (owner) {
      case Project: s.project.push_back(e); break;
      case Pattern: s.patterns[idx].push_back(e); break;
      case Channel: s.channels[idx].push_back(e); break;
      case Arrangement: s.arrangements[idx].push_back(e); break;
    }
  }
  // Mixer: insert … slots (each closed by 98 Slot.Index) … closed by 147 Insert.Output.
  Sections::Insert ins;
  std::vector<Event> slotEvents;
  int slotCount = 0;
  bool open = false;
  for (size_t i = mixerStart; i < ev.size(); ++i) {
    const Event& e = ev[i];
    if (e.id == 225) {
      s.mixerParams.push_back(e);
      continue;
    }
    if (e.id == 98) {
      int sl = std::clamp(slotCount, 0, 9);
      ins.slots[static_cast<size_t>(sl)].events = slotEvents;
      slotEvents.clear();
      ++slotCount;
      open = true;
      continue;
    }
    const bool pluginEvent = e.id == 201 || e.id == 212 || e.id == 213 || e.id == 203 || e.id == 155 || e.id == 128 || e.id == 41;
    // Older projects (FL 11 and before) have no 98 Slot.Index: a plugin's events
    // just follow one another, so a second name (201) starts the next slot and
    // the insert's end closes the last one.
    const auto hasName = [&] {
      return std::any_of(slotEvents.begin(), slotEvents.end(), [](const Event& x) { return x.id == 201; });
    };
    if (pluginEvent && open && e.id == 201 && slotCount < 10 && hasName()) {
      ins.slots[static_cast<size_t>(slotCount++)].events = slotEvents;
      slotEvents.clear();
    }
    if (pluginEvent && open) {
      slotEvents.push_back(e);
      continue;
    }
    if (e.id == 147 && slotCount < 10 && hasName()) ins.slots[static_cast<size_t>(slotCount++)].events = slotEvents;
    ins.events.push_back(e);
    if (e.id == 236) open = true;
    if (e.id == 147) {  // end of this insert
      // Older projects (FL 12) write no 98 Slot.Index after a slot's plugin:
      // the pending plugin events belong to the next slot.
      bool hasState = false;
      for (const Event& pe : slotEvents) hasState |= pe.id == 213 || pe.id == 201;
      if (hasState && slotCount < 10) ins.slots[static_cast<size_t>(slotCount)].events = slotEvents;
      s.inserts.push_back(ins);
      ins = Sections::Insert{};
      slotEvents.clear();
      slotCount = 0;
      open = false;
    }
  }
  if (!ins.events.empty()) s.project.insert(s.project.end(), ins.events.begin(), ins.events.end());
  return s;
}

std::string Context::resolvePath(const std::string& flPath) const {
  std::string p = flPath;
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  const std::string tag = "%FLStudioFactoryData%";
  if (p.rfind(tag, 0) == 0) p = factoryData + p.substr(tag.size());
  const std::string utag = "%FLStudioUserData%";
  if (p.rfind(utag, 0) == 0) {
    const char* home = std::getenv("HOME");
    p = std::string(home ? home : "") + "/Documents/Image-Line" + p.substr(utag.size());
  }
  if (!p.empty() && p[0] != '/' && !projectDir.empty() && !(p.size() > 1 && p[1] == ':')) p = projectDir + "/" + p;
  return p;
}

std::string defaultFactoryData() {
  if (const char* e = std::getenv("GRIDLINE_FL_DATA"); e && *e) return e;
  std::error_code ec;
  for (const char* c : {"/Applications/FL Studio 2026.app/Contents/Resources/FL", "/Applications/FL Studio 2025.app/Contents/Resources/FL",
                        "/Applications/FL Studio 21.app/Contents/Resources/FL"})
    if (fs::exists(c, ec)) return c;
  if (const char* home = std::getenv("HOME")) {
    std::string w = std::string(home) + "/.wine/drive_c/Program Files/Image-Line/FL Studio 2025";
    if (fs::exists(w, ec)) return w;
  }
  return "";
}

}  // namespace gl::flp

namespace gl {

bool importFlp(const std::string& path, Song& out, flp::Report& report, std::string* err, const std::string& factoryData) {
  flp::File file;
  if (!flp::readFile(path, file, err)) return false;
  Song song;  // built from scratch: mixer, channels, patterns, playlist all come from the file
  song.tracks.clear();
  flp::Context c(file, song, report);
  c.sec = flp::split(file);
  c.factoryData = factoryData.empty() ? flp::defaultFactoryData() : factoryData;
  c.projectDir = std::filesystem::path(path).parent_path().string();
  flp::importProject(c);
  flp::importMixer(c);
  flp::importChannels(c);
  flp::importPatterns(c);
  flp::importPlugins(c);
  flp::finishChannels(c);
  flp::importArrangement(c);
  flp::importPatternEvents(c);
  ensureMixer(song);
  out = std::move(song);
  return true;
}

}  // namespace gl
