// Gridline - K-142 (0.7.8): the shape memo of the plugin editors' drawing (see PluginSkinMemo.h).
#include "PluginSkinMemo.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"

namespace gl::skin::memo {

void Key::add(float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof u);
  add(u);
}

namespace {

constexpr int kStateWords = 20;
constexpr int kWords = Key::kMax + kStateWords;
constexpr int kDropAfter = 64;          // frames unused before a kept drawing goes
constexpr size_t kMaxEntries = 8192;
constexpr size_t kMaxVertices = 1u << 20;  // about 20 MB of vertices at most

struct Full {
  uint32_t w[kWords] = {};
  int n = 0;
  bool operator==(const Full& o) const { return n == o.n && std::memcmp(w, o.w, sizeof(uint32_t) * static_cast<size_t>(n)) == 0; }
};

struct Entry {
  Full key;
  int lastSeen = -100;
  bool kept = false;
  std::vector<ImDrawVert> vtx;
  std::vector<ImDrawIdx> idx;  // relative to the first vertex
};

struct State {
  bool on = true, verify = false, printing = false;
  int lastPrint = -1;
  std::unordered_map<uint64_t, Entry> map;
  size_t vertices = 0;
  int lastSweep = 0;
  Stats st;
  // A verify replay: the next Record of this key compares instead of keeping.
  uint64_t verifyHash = 0;
  bool verifyPending = false;
};
void report();
State& S() {
  static State s = [] {
    State x;
    // Dev and tests: GRIDLINE_DRAW_MEMO=off builds every drawing as before; =verify builds the
    // kept ones again, compares, and prints the counts so far once a frame (stdout); =stats only prints.
    if (const char* e = std::getenv("GRIDLINE_DRAW_MEMO")) {
      if (std::strcmp(e, "off") == 0) x.on = false;
      if (std::strcmp(e, "verify") == 0) x.verify = x.printing = true;
      if (std::strcmp(e, "stats") == 0) x.printing = true;
    }
    return x;
  }();
  return s;
}
void report() {
  const State& s = S();
  std::printf("drawmemo: hits %llu misses %llu kept %llu verified %llu mismatches %llu vertices %zu entries %zu\n", static_cast<unsigned long long>(s.st.hits),
               static_cast<unsigned long long>(s.st.misses), static_cast<unsigned long long>(s.st.kept), static_cast<unsigned long long>(s.st.verified),
               static_cast<unsigned long long>(s.st.mismatches), s.vertices, s.map.size());
  std::fflush(stdout);
}

int frameNow() { return ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0; }

void addPtr(Full& f, const void* p) {
  const uint64_t v = reinterpret_cast<uintptr_t>(p);
  f.w[f.n++] = static_cast<uint32_t>(v);
  f.w[f.n++] = static_cast<uint32_t>(v >> 32);
}
void addF(Full& f, float v) { std::memcpy(&f.w[f.n++], &v, sizeof v); }

// The caller's key plus everything of the draw list that shapes the triangles.
Full fullKey(const ImDrawList* dl, const Key& k) {
  Full f;
  std::memcpy(f.w, k.w, sizeof(uint32_t) * static_cast<size_t>(k.n));
  f.n = k.n;
  f.w[f.n++] = static_cast<uint32_t>(k.n);
  f.w[f.n++] = static_cast<uint32_t>(dl->Flags);
  addF(f, dl->_FringeScale);
  const ImDrawListSharedData* d = dl->_Data;
  addPtr(f, d);
  if (d) {
    addF(f, d->TexUvWhitePixel.x), addF(f, d->TexUvWhitePixel.y);
    addPtr(f, d->TexUvLines);
    if (d->TexUvLines)
      for (int i = 1; i <= 2; ++i) addF(f, d->TexUvLines[i].x), addF(f, d->TexUvLines[i].y), addF(f, d->TexUvLines[i].z), addF(f, d->TexUvLines[i].w);
    addF(f, d->CircleTessellationMaxError), addF(f, d->CurveTessellationTol), addF(f, d->ArcFastRadiusCutoff);
  }
  return f;
}

uint64_t hashOf(const Full& f) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < f.n; ++i) {
    h ^= f.w[i];
    h *= 1099511628211ull;
  }
  return h;
}

void sweep(State& s, int now) {
  if (s.printing && now != s.lastPrint) {
    s.lastPrint = now;
    report();
  }
  if (now - s.lastSweep < kDropAfter && s.map.size() < kMaxEntries) return;
  s.lastSweep = now;
  for (auto it = s.map.begin(); it != s.map.end();) {
    if (now - it->second.lastSeen > kDropAfter || now < it->second.lastSeen) {
      s.vertices -= it->second.vtx.size();
      it = s.map.erase(it);
    } else {
      ++it;
    }
  }
  if (s.map.size() >= kMaxEntries) {  // still full: start again
    s.map.clear();
    s.vertices = 0;
  }
}

}  // namespace

uint32_t contextId(const void* bytes, size_t size) {
  struct Slot {
    std::vector<uint8_t> bytes;
    uint32_t id = 0;
    uint64_t used = 0;
  };
  static Slot slots[8];
  static uint32_t nextId = 0;
  static uint64_t tick = 0;
  ++tick;
  Slot* lru = &slots[0];
  for (Slot& s : slots) {
    if (s.id && s.bytes.size() == size && std::memcmp(s.bytes.data(), bytes, size) == 0) {
      s.used = tick;
      return s.id;
    }
    if (s.used < lru->used) lru = &s;
  }
  const auto* b = static_cast<const uint8_t*>(bytes);
  lru->bytes.assign(b, b + size);
  lru->id = ++nextId;  // never given twice: drawings kept under a dropped block never match again
  lru->used = tick;
  return lru->id;
}

bool replay(ImDrawList* dl, const Key& key) {
  State& s = S();
  if (!s.on || key.full() || !dl) return false;
  if (s.printing && frameNow() != s.lastPrint) {
    s.lastPrint = frameNow();
    report();
  }
  const Full f = fullKey(dl, key);
  const uint64_t h = hashOf(f);
  auto it = s.map.find(h);
  if (it == s.map.end() || !it->second.kept || !(it->second.key == f)) {
    ++s.st.misses;
    return false;
  }
  Entry& e = it->second;
  e.lastSeen = frameNow();
  ++s.st.hits;
  if (s.verify) {  // build it again; the Record compares
    s.verifyHash = h;
    s.verifyPending = true;
    return false;
  }
  const int nv = static_cast<int>(e.vtx.size()), ni = static_cast<int>(e.idx.size());
  const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
  dl->PrimReserve(ni, nv);
  std::memcpy(dl->_VtxWritePtr, e.vtx.data(), sizeof(ImDrawVert) * static_cast<size_t>(nv));
  for (int i = 0; i < ni; ++i) dl->_IdxWritePtr[i] = static_cast<ImDrawIdx>(e.idx[static_cast<size_t>(i)] + base);
  dl->_VtxWritePtr += nv;
  dl->_IdxWritePtr += ni;
  dl->_VtxCurrentIdx += static_cast<unsigned>(nv);
  return true;
}

Record::Record(ImDrawList* dl, const Key& key)
    : dl_(dl), key_(key), cmd0_(dl ? dl->CmdBuffer.Size : 0), vtx0_(dl ? dl->VtxBuffer.Size : 0), idx0_(dl ? dl->IdxBuffer.Size : 0),
      cur0_(dl ? dl->_VtxCurrentIdx : 0), keep_(dl && S().on && !key.full()) {}

Record::~Record() {
  State& s = S();
  if (!keep_) return;
  const bool verifying = s.verifyPending;
  s.verifyPending = false;
  const int now = frameNow();
  sweep(s, now);
  const Full f = fullKey(dl_, key_);
  const uint64_t h = hashOf(f);
  // Kept only when the drawing went into the one draw command it started in, with no vertex
  // offset jump (its indices then simply follow the vertices).
  const int nv = dl_->VtxBuffer.Size - vtx0_, ni = dl_->IdxBuffer.Size - idx0_;
  const bool plain = dl_->CmdBuffer.Size == cmd0_ && dl_->_VtxCurrentIdx - cur0_ == static_cast<unsigned>(nv) && nv >= 0 && ni >= 0;
  auto it = s.map.find(h);
  if (verifying && h == s.verifyHash && it != s.map.end() && it->second.key == f) {
    Entry& e = it->second;
    bool same = plain && e.vtx.size() == static_cast<size_t>(nv) && e.idx.size() == static_cast<size_t>(ni) &&
                std::memcmp(e.vtx.data(), dl_->VtxBuffer.Data + vtx0_, sizeof(ImDrawVert) * static_cast<size_t>(nv)) == 0;
    for (int i = 0; same && i < ni; ++i)
      if (static_cast<ImDrawIdx>(dl_->IdxBuffer.Data[idx0_ + i] - cur0_) != e.idx[static_cast<size_t>(i)]) same = false;
    ++s.st.verified;
    if (!same) ++s.st.mismatches;
    return;
  }
  if (it == s.map.end()) {
    Entry e;
    e.key = f;
    e.lastSeen = now;
    s.map.emplace(h, std::move(e));
    return;
  }
  Entry& e = it->second;
  if (!(e.key == f)) {  // another drawing with the same hash: this one takes the place
    s.vertices -= e.vtx.size();
    e = Entry{};
    e.key = f;
    e.lastSeen = now;
    return;
  }
  const bool seenBefore = e.lastSeen == now - 1 || e.lastSeen == now;
  e.lastSeen = now;
  if (!seenBefore || !plain || e.kept || s.vertices + static_cast<size_t>(nv) > kMaxVertices) return;
  e.vtx.assign(dl_->VtxBuffer.Data + vtx0_, dl_->VtxBuffer.Data + vtx0_ + nv);
  e.idx.resize(static_cast<size_t>(ni));
  for (int i = 0; i < ni; ++i) e.idx[static_cast<size_t>(i)] = static_cast<ImDrawIdx>(dl_->IdxBuffer.Data[idx0_ + i] - cur0_);
  e.kept = true;
  s.vertices += e.vtx.size();
  ++s.st.kept;
}

void setEnabled(bool on) {
  S().on = on;
  if (!on) clear();
}
bool enabled() { return S().on; }
void setVerify(bool on) { S().verify = on; }
Stats stats() {
  Stats st = S().st;
  st.entries = S().map.size();
  return st;
}
void resetStats() { S().st = Stats{}; }
void clear() {
  S().map.clear();
  S().vertices = 0;
}

}  // namespace gl::skin::memo
