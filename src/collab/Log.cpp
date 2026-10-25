// Gridline - J-111 collaboration: batches, op log, messages. See Log.h.
#include "Log.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace gl::collab {

std::string siteHex(SiteId s) {
  char buf[20];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(s));
  return buf;
}

SiteId siteFromHex(const std::string& s) {
  if (s.empty() || s.size() > 16) return 0;
  SiteId v = 0;
  for (char c : s) {
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? 10 + c - 'a' : (c >= 'A' && c <= 'F') ? 10 + c - 'A' : -1;
    if (d < 0) return 0;
    v = (v << 4) | static_cast<SiteId>(d);
  }
  return v;
}

Json vvToJson(const VersionVector& v) {
  Json j = Json::makeObject();
  for (const auto& [s, q] : v) j.object[siteHex(s)] = Json::makeNumber(static_cast<double>(q));
  return j;
}

VersionVector vvFromJson(const Json& j) {
  VersionVector v;
  if (!j.is(Json::Type::Object)) return v;
  for (const auto& [k, q] : j.object) {
    const SiteId s = siteFromHex(k);
    if (s && q.is(Json::Type::Number) && q.number >= 0 && q.number < 9e15) v[s] = static_cast<uint64_t>(q.number);
  }
  return v;
}

bool vvCovers(const VersionVector& have, const VersionVector& need, SiteId except) {
  for (const auto& [s, q] : need) {
    if (s == except) continue;
    auto it = have.find(s);
    if ((it == have.end() ? 0 : it->second) < q) return false;
  }
  return true;
}

// ---------------------------------------------------------------- batch ----
Json Batch::toJson() const {
  Json j = Json::makeObject();
  j.object["a"] = Json::makeString(siteHex(site));
  j.object["q"] = Json::makeNumber(static_cast<double>(seq));
  j.object["d"] = vvToJson(deps);
  if (!name.empty()) j.object["n"] = Json::makeString(name);
  Json ops = Json::makeArray();
  for (const auto& o : this->ops) ops.array.push_back(opToJson(o));
  j.object["o"] = std::move(ops);
  if (!sig.empty()) j.object["g"] = Json::makeString(crypto::toB64url(sig));
  return j;
}

std::string Batch::signedText() const {
  Batch b = *this;
  b.sig.clear();
  return "gridline-batch-v1\n" + jsonCompact(b.toJson());
}

bool Batch::fromJson(const Json& j, Batch& out) {
  if (!j.is(Json::Type::Object)) return false;
  out = Batch();
  out.site = siteFromHex(j.text("a"));
  const double q = j.num("q", 0);
  if (!out.site || q < 1 || q > 9e15) return false;
  out.seq = static_cast<uint64_t>(q);
  if (const Json* d = j.find("d")) out.deps = vvFromJson(*d);
  out.name = j.text("n");
  const Json* ops = j.find("o");
  if (!ops || !ops->is(Json::Type::Array) || ops->array.size() > 200000) return false;
  uint64_t lastT = 0;
  for (const auto& oj : ops->array) {
    Op op;
    if (!opFromJson(oj, op)) return false;
    if (op.t <= lastT) return false;  // stamps strictly increase inside a batch
    lastT = op.t;
    out.ops.push_back(std::move(op));
  }
  if (!crypto::fromB64url(j.text("g"), out.sig)) return false;
  return true;
}

std::string Batch::text() const { return jsonCompact(toJson()); }

bool Batch::parse(const std::string& text, Batch& out) {
  Json j;
  if (!jsonParse(text, j)) return false;
  return fromJson(j, out);
}

// ------------------------------------------------------------------ crc ----
uint32_t crc32(const void* data, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

namespace {

std::FILE* fopenPath(const std::string& p, const char* mode) {
#if defined(_WIN32)
  const std::wstring m(mode, mode + std::strlen(mode));
  return _wfopen(fs::path(p).c_str(), m.c_str());
#else
  return std::fopen(p.c_str(), mode);
#endif
}

void putU32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint32_t getU32(const unsigned char* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

void syncFile(std::FILE* f) {
  if (!f) return;
  std::fflush(f);
#if defined(_WIN32)
  _commit(_fileno(f));
#else
  fsync(fileno(f));
#endif
}

void syncDir(const std::string& dir) {
#if !defined(_WIN32)
  const int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd >= 0) {
    fsync(fd);
    ::close(fd);
  }
#else
  (void)dir;
#endif
}

}  // namespace

bool readFileText(const std::string& path, std::string& out, size_t maxBytes) {
  std::error_code ec;
  const auto size = fs::file_size(fs::path(path), ec);
  if (ec || size > maxBytes) return false;
  std::ifstream is(fs::path(path), std::ios::binary);
  if (!is) return false;
  std::stringstream ss;
  ss << is.rdbuf();
  out = ss.str();
  return true;
}

bool writeFileDurable(const std::string& path, const std::string& text, std::string* err) {
  const std::string tmp = path + ".tmp";
  std::FILE* f = fopenPath(tmp.c_str(), "wb");
  if (!f) {
    if (err) *err = "cannot write " + tmp;
    return false;
  }
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  syncFile(f);
  std::fclose(f);
  std::error_code ec;
  if (!ok) {
    fs::remove(fs::path(tmp), ec);
    if (err) *err = "cannot write " + tmp;
    return false;
  }
  fs::rename(fs::path(tmp), fs::path(path), ec);
  if (ec) {
    fs::remove(fs::path(tmp), ec);
    if (err) *err = "cannot replace " + path;
    return false;
  }
  syncDir(fs::path(path).parent_path().string());
  return true;
}

// ---------------------------------------------------------------- oplog ----
OpLog::~OpLog() { close(); }

std::string OpLog::logPath(int gen) const { return (fs::path(dir_) / ("oplog." + std::to_string(gen) + ".bin")).string(); }

void OpLog::close() {
  if (f_) {
    syncFile(f_);
    std::fclose(f_);
    f_ = nullptr;
  }
}

bool OpLog::openLog(int gen, bool truncateAt, uint64_t at) {
  close();
  const std::string p = logPath(gen);
  std::error_code ec;
  if (truncateAt) fs::resize_file(fs::path(p), at, ec);
  f_ = fopenPath(p.c_str(), "ab");
  gen_ = gen;
  return f_ != nullptr;
}

bool OpLog::open(const std::string& dir, Json& snapshot, std::vector<std::string>& records, std::string* err) {
  close();
  dir_ = dir;
  records.clear();
  snapshot = Json();
  truncated_ = 0;
  std::error_code ec;
  fs::create_directories(fs::path(dir), ec);
  int gen = 0;
  std::string text;
  const std::string snapPath = (fs::path(dir) / "snapshot.json").string();
  if (fs::exists(fs::path(snapPath), ec)) {
    if (!readFileText(snapPath, text) || !jsonParse(text, snapshot) || !snapshot.is(Json::Type::Object)) {
      if (err) *err = "the collaboration snapshot is damaged";
      return false;
    }
    gen = static_cast<int>(snapshot.num("gen", 0));
  }
  // Logs of other generations are leftovers of an interrupted compaction.
  for (const auto& e : fs::directory_iterator(fs::path(dir), ec)) {
    const std::string n = e.path().filename().string();
    if (n.rfind("oplog.", 0) == 0 && n != "oplog." + std::to_string(gen) + ".bin") fs::remove(e.path(), ec);
    if (n == "snapshot.json.tmp") fs::remove(e.path(), ec);
  }
  std::string data;
  const std::string lp = logPath(gen);
  uint64_t good = 0;
  bytes_ = 0;
  records_ = 0;
  if (fs::exists(fs::path(lp), ec) && readFileText(lp, data, 4ull << 30)) {
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    size_t at = 0;
    while (at + 8 <= data.size()) {
      const uint32_t len = getU32(p + at), crc = getU32(p + at + 4);
      if (len > data.size() - at - 8) break;
      if (crc32(p + at + 8, len) != crc) break;
      records.emplace_back(data, at + 8, len);
      at += 8 + len;
    }
    good = at;
    truncated_ = static_cast<int64_t>(data.size() - at);
    records_ = records.size();
    bytes_ = good;
  }
  if (!openLog(gen, truncated_ > 0, good)) {
    if (err) *err = "cannot open the collaboration log in " + dir;
    return false;
  }
  return true;
}

bool OpLog::append(const std::string& payload) {
  if (!f_) return false;
  std::string rec;
  rec.reserve(payload.size() + 8);
  putU32(rec, static_cast<uint32_t>(payload.size()));
  putU32(rec, crc32(payload.data(), payload.size()));
  rec += payload;
  if (std::fwrite(rec.data(), 1, rec.size(), f_) != rec.size()) return false;
  std::fflush(f_);
  bytes_ += rec.size();
  ++records_;
  dirty_ = true;
  return true;
}

void OpLog::sync() {
  if (!dirty_ || !f_) return;
  syncFile(f_);
  dirty_ = false;
}

bool OpLog::compact(Json snapshot, const std::vector<std::string>& keep, std::string* err) {
  if (dir_.empty()) return false;
  const int next = gen_ + 1;
  // 1. the next log with the kept records, synced
  const std::string np = logPath(next);
  std::FILE* nf = fopenPath(np.c_str(), "wb");
  if (!nf) {
    if (err) *err = "cannot write " + np;
    return false;
  }
  uint64_t nbytes = 0;
  for (const auto& r : keep) {
    std::string rec;
    putU32(rec, static_cast<uint32_t>(r.size()));
    putU32(rec, crc32(r.data(), r.size()));
    rec += r;
    if (std::fwrite(rec.data(), 1, rec.size(), nf) != rec.size()) {
      std::fclose(nf);
      std::error_code ec;
      fs::remove(fs::path(np), ec);
      if (err) *err = "cannot write " + np;
      return false;
    }
    nbytes += rec.size();
  }
  syncFile(nf);
  std::fclose(nf);
  // 2. the snapshot naming it (atomic rename) - from here on the new state is the valid one
  snapshot.object["gen"] = Json::makeNumber(next);
  if (!writeFileDurable((fs::path(dir_) / "snapshot.json").string(), jsonCompact(snapshot), err)) {
    std::error_code ec;
    fs::remove(fs::path(np), ec);
    return false;
  }
  // 3. the old log goes
  const int old = gen_;
  close();
  std::error_code ec;
  fs::remove(fs::path(logPath(old)), ec);
  f_ = fopenPath(np.c_str(), "ab");
  gen_ = next;
  bytes_ = nbytes;
  records_ = keep.size();
  dirty_ = false;
  return f_ != nullptr;
}

// -------------------------------------------------------------- message ----
std::string Message::encode() const {
  const std::string h = jsonCompact(head);
  std::string out;
  out.reserve(4 + h.size() + bin.size());
  putU32(out, static_cast<uint32_t>(h.size()));
  out += h;
  out += bin;
  return out;
}

bool Message::decode(const std::string& wire, Message& out) {
  if (wire.size() < 4) return false;
  const uint32_t n = getU32(reinterpret_cast<const unsigned char*>(wire.data()));
  if (n > wire.size() - 4) return false;
  if (!jsonParse(wire.substr(4, n), out.head) || !out.head.is(Json::Type::Object)) return false;
  out.bin = wire.substr(4 + n);
  return true;
}

}  // namespace gl::collab
