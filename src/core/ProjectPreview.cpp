#include "ProjectPreview.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include "Platform.h"
#include "Project.h"

namespace gl::preview {

namespace fs = std::filesystem;

namespace {

uint32_t clipColour(const Song& s, const Clip& c) {
  switch (c.kind) {
    case ClipKind::Pattern:
      if (const Pattern* p = s.pattern(c.pattern)) return p->color;
      break;
    case ClipKind::Audio:
      if (const Channel* ch = s.channel(c.source)) return ch->color;
      break;
    case ClipKind::Automation:
      if (const AutomationClip* a = s.automation(c.source)) return a->color;
      break;
    default: break;
  }
  return 0x7A8696;
}

void arrangement(const Song& s, Sketch& k) {
  const Tick end = s.lengthTicks();
  if (end <= 0) return;
  std::vector<int> used;
  for (const Clip& c : s.playlist) used.push_back(c.track);
  std::sort(used.begin(), used.end());
  used.erase(std::unique(used.begin(), used.end()), used.end());
  std::map<int, int> rowOf;
  for (size_t i = 0; i < used.size(); ++i) rowOf[used[i]] = std::min(static_cast<int>(i), kMaxRows - 1);
  k.rows = std::min(static_cast<int>(used.size()), kMaxRows);
  std::vector<const Clip*> clips;
  for (const Clip& c : s.playlist) clips.push_back(&c);
  std::stable_sort(clips.begin(), clips.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
  for (const Clip* c : clips) {
    if (k.blocks.size() >= kMaxBlocks) break;
    const Tick len = std::max<Tick>(1, s.clipLength(*c));
    Block b;
    b.row = rowOf[c->track];
    b.x0 = static_cast<float>(static_cast<double>(c->start) / static_cast<double>(end));
    b.x1 = static_cast<float>(static_cast<double>(c->start + len) / static_cast<double>(end));
    b.x0 = std::clamp(b.x0, 0.0f, 1.0f);
    b.x1 = std::clamp(b.x1, b.x0, 1.0f);
    b.rgb = clipColour(s, *c) & 0xFFFFFF;
    b.muted = c->muted;
    k.blocks.push_back(b);
  }
  k.kind = Kind::Arrangement;
  const Tick bar = std::max<Tick>(1, s.barTicks());
  k.bars = static_cast<int>((end + bar - 1) / bar);
  k.seconds = s.tempo > 0 ? static_cast<double>(end) / std::max(1, s.ppq) * 60.0 / s.tempo : 0;
}

void pattern(const Song& s, Sketch& k) {
  for (const Pattern& p : s.patterns) {
    // Steps first: the rows of channels that have lit steps.
    int row = 0;
    int cols = std::min(kMaxStepCols, std::max(1, p.stepsPerBar * std::max(1, p.bars)));
    for (const Channel& ch : s.channels) {
      const auto it = p.steps.find(ch.id);
      if (it == p.steps.end() || row >= kMaxRows) continue;
      bool any = false;
      for (size_t i = 0; i < it->second.size() && static_cast<int>(i) < cols; ++i)
        if (it->second[i] > 0) {
          any = true;
          if (k.blocks.size() < kMaxBlocks)
            k.blocks.push_back({row, static_cast<float>(i) / static_cast<float>(cols), static_cast<float>(i + 1) / static_cast<float>(cols), ch.color & 0xFFFFFF, ch.muted});
        }
      if (any) ++row;
    }
    if (row > 0) {
      k.kind = Kind::Pattern;
      k.rows = row;
      k.cols = cols;
      k.bars = std::max(1, p.bars);
      return;
    }
    // Else notes: rows by pitch over the pattern's content length.
    int lo = 128, hi = -1;
    for (const auto& [cid, notes] : p.notes)
      for (const Note& n : notes) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
    if (hi < 0) continue;
    const Tick len = std::max<Tick>(1, s.patternContentLength(p));
    const int span = hi - lo + 1;
    k.rows = std::min(span, kMaxRows * 2);
    for (const Channel& ch : s.channels) {
      const auto it = p.notes.find(ch.id);
      if (it == p.notes.end()) continue;
      for (const Note& n : it->second) {
        if (k.blocks.size() >= kMaxBlocks) break;
        Block b;
        b.row = std::clamp((hi - n.pitch) * k.rows / span, 0, k.rows - 1);
        b.x0 = std::clamp(static_cast<float>(static_cast<double>(n.start) / static_cast<double>(len)), 0.0f, 1.0f);
        b.x1 = std::clamp(static_cast<float>(static_cast<double>(n.start + n.length) / static_cast<double>(len)), b.x0, 1.0f);
        b.rgb = ch.color & 0xFFFFFF;
        k.blocks.push_back(b);
      }
    }
    k.kind = Kind::Pattern;
    k.cols = 0;
    k.bars = std::max(1, p.bars);
    return;
  }
}

void rack(const Song& s, Sketch& k) {
  for (const Channel& ch : s.channels) {
    if (k.rows >= kMaxRows) break;
    k.blocks.push_back({k.rows++, 0, 0, ch.color & 0xFFFFFF, ch.muted});
  }
  if (k.rows > 0) {
    k.kind = Kind::Rack;
    k.cols = 16;
  }
}

void mixer(const Song& s, Sketch& k) {
  int n = 0;
  for (size_t i = 1; i < s.mixer.size() && n < 16; ++i)  // inserts (0 = master)
    if (!s.mixer[i].name.empty()) k.blocks.push_back({n++, 0, 0, s.mixer[i].color & 0xFFFFFF, s.mixer[i].muted});
  if (n > 0) {
    k.kind = Kind::Mixer;
    k.rows = n;
  }
}

}  // namespace

Sketch make(const Song& s) {
  Sketch k;
  k.title = s.info.title;
  k.tempo = s.tempo;
  k.num = s.timeSigNum;
  k.den = s.timeSigDen;
  k.channels = static_cast<int>(s.channels.size());
  k.patterns = static_cast<int>(s.patterns.size());
  k.clips = static_cast<int>(s.playlist.size());
  arrangement(s, k);
  if (k.kind == Kind::Arrangement && k.clips < 3) {  // one or two clips say little: the pattern says more
    Sketch p = k;
    p.blocks.clear();
    p.kind = Kind::Empty;
    pattern(s, p);
    if (p.kind == Kind::Pattern && p.cols > 0) k = std::move(p);
  }
  if (k.kind == Kind::Empty) pattern(s, k);
  if (k.kind == Kind::Empty) rack(s, k);
  if (k.kind == Kind::Empty) mixer(s, k);
  return k;
}

Json toJson(const Sketch& k) {
  Json j = Json::makeObject();
  j.object["version"] = Json::makeNumber(kVersion);
  j.object["kind"] = Json::makeNumber(static_cast<int>(k.kind));
  j.object["title"] = Json::makeString(k.title);
  j.object["tempo"] = Json::makeNumber(k.tempo);
  j.object["num"] = Json::makeNumber(k.num);
  j.object["den"] = Json::makeNumber(k.den);
  j.object["bars"] = Json::makeNumber(k.bars);
  j.object["seconds"] = Json::makeNumber(k.seconds);
  j.object["channels"] = Json::makeNumber(k.channels);
  j.object["patterns"] = Json::makeNumber(k.patterns);
  j.object["clips"] = Json::makeNumber(k.clips);
  j.object["rows"] = Json::makeNumber(k.rows);
  j.object["cols"] = Json::makeNumber(k.cols);
  j.object["fileSize"] = Json::makeNumber(static_cast<double>(k.fileSize));
  j.object["fileTime"] = Json::makeNumber(static_cast<double>(k.fileTime));
  // Blocks as one flat array: row, x0 * 10000, x1 * 10000, rgb, muted.
  Json b = Json::makeArray();
  for (const Block& x : k.blocks) {
    b.array.push_back(Json::makeNumber(x.row));
    b.array.push_back(Json::makeNumber(std::round(x.x0 * 10000.0f)));
    b.array.push_back(Json::makeNumber(std::round(x.x1 * 10000.0f)));
    b.array.push_back(Json::makeNumber(x.rgb));
    b.array.push_back(Json::makeNumber(x.muted ? 1 : 0));
  }
  j.object["blocks"] = b;
  return j;
}

bool fromJson(const Json& j, Sketch& out) {
  if (!j.is(Json::Type::Object) || static_cast<int>(j.num("version")) != kVersion) return false;
  Sketch k;
  const int kind = static_cast<int>(j.num("kind"));
  if (kind < 0 || kind > static_cast<int>(Kind::Mixer)) return false;
  k.kind = static_cast<Kind>(kind);
  k.title = j.text("title");
  k.tempo = j.num("tempo");
  k.num = static_cast<int>(j.num("num", 4));
  k.den = static_cast<int>(j.num("den", 4));
  k.bars = static_cast<int>(j.num("bars"));
  k.seconds = j.num("seconds");
  k.channels = static_cast<int>(j.num("channels"));
  k.patterns = static_cast<int>(j.num("patterns"));
  k.clips = static_cast<int>(j.num("clips"));
  k.rows = std::clamp(static_cast<int>(j.num("rows")), 0, kMaxRows * 2);  // (Mixer: up to 16 strips)
  k.cols = std::clamp(static_cast<int>(j.num("cols")), 0, kMaxStepCols);
  k.fileSize = static_cast<uint64_t>(std::max(0.0, j.num("fileSize")));
  k.fileTime = static_cast<int64_t>(j.num("fileTime"));
  if (const Json* b = j.find("blocks"); b && b->is(Json::Type::Array)) {
    for (size_t i = 0; i + 5 <= b->array.size() && k.blocks.size() < kMaxBlocks; i += 5) {
      Block x;
      x.row = std::clamp(static_cast<int>(b->array[i].number), 0, std::max(0, k.rows - 1));
      x.x0 = std::clamp(static_cast<float>(b->array[i + 1].number / 10000.0), 0.0f, 1.0f);
      x.x1 = std::clamp(static_cast<float>(b->array[i + 2].number / 10000.0), x.x0, 1.0f);
      x.rgb = static_cast<uint32_t>(b->array[i + 3].number) & 0xFFFFFF;
      x.muted = b->array[i + 4].number != 0;
      k.blocks.push_back(x);
    }
  }
  out = std::move(k);
  return true;
}

std::string pathKey(const std::string& path) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : path) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

std::string cacheFile(const std::string& cacheDir, const std::string& projectPath) {
  return (fs::u8path(cacheDir) / (pathKey(projectPath) + ".json")).u8string();
}

bool fileStamp(const std::string& path, uint64_t& size, int64_t& time) {
  std::error_code ec;
  const fs::path p = fs::u8path(path);
  if (!fs::is_regular_file(p, ec)) return false;
  size = static_cast<uint64_t>(fs::file_size(p, ec));
  if (ec) return false;
  const auto t = fs::last_write_time(p, ec);
  if (ec) return false;
  time = platform::fileTimeSeconds(t);
  return true;
}

bool loadCached(const std::string& cacheDir, const std::string& projectPath, Sketch& out) {
  uint64_t size = 0;
  int64_t time = 0;
  if (!fileStamp(projectPath, size, time)) return false;
  std::ifstream is(fs::u8path(cacheFile(cacheDir, projectPath)), std::ios::binary);
  if (!is) return false;
  std::stringstream ss;
  ss << is.rdbuf();
  Json j;
  Sketch k;
  if (!jsonParse(ss.str(), j) || !fromJson(j, k)) return false;
  if (k.fileSize != size || k.fileTime != time) return false;
  out = std::move(k);
  return true;
}

bool storeCached(const std::string& cacheDir, const std::string& projectPath, Sketch k) {
  if (!fileStamp(projectPath, k.fileSize, k.fileTime)) return false;
  std::error_code ec;
  fs::create_directories(fs::u8path(cacheDir), ec);
  return writeFileAtomic(cacheFile(cacheDir, projectPath), jsonCompact(toJson(k)));
}

}  // namespace gl::preview
