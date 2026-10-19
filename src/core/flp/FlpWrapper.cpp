// Gridline — FLP import: "Fruity Wrapper" instances → hosted plugins.
#include "FlpWrapper.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include "../../host/HostedPlugins.h"
#include "FlpGen2.h"

namespace gl::flp {

using gen2::Blob;

namespace {

std::string text(Blob b) { return std::string(reinterpret_cast<const char*>(b.p), b.size()); }

// {u32 id, u64 size, bytes} records.
struct Record {
  uint32_t id = 0;
  Blob data;
};
std::vector<Record> records(Blob b, size_t from) {
  std::vector<Record> out;
  size_t p = from;
  while (b.has(p, 12)) {
    uint32_t id = b.u32(p);
    uint64_t n = static_cast<uint64_t>(b.u32(p + 4)) | (static_cast<uint64_t>(b.u32(p + 8)) << 32);
    if (n > b.size() - p - 12) break;
    out.push_back({id, b.sub(p + 12, static_cast<size_t>(n))});
    p += 12 + static_cast<size_t>(n);
  }
  return out;
}

std::string hexUid(Blob g) {
  // COM GUID order: Data1 (u32 LE), Data2, Data3 (u16 LE), 8 bytes as stored.
  static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  std::string s;
  char b[3];
  for (int i : order) {
    std::snprintf(b, sizeof b, "%02X", g.u8(static_cast<size_t>(i)));
    s += b;
  }
  return s;
}

std::string vst2State(Blob s) {
  if (s.size() < 13 || s.i32(0) != -9) return {};
  const int32_t flag = s.i32(5);
  const uint32_t size = s.u32(9);
  const size_t start = flag == -2 ? 21 : 13;
  if (size > 0) {
    if (!s.has(start, size)) return {};
    return "vst2c1:" + host::base64Encode(text(s.sub(start, size)));
  }
  // Parameter list: u32 count, f32 values.
  const uint32_t n = s.u32(start);
  if (n == 0 || n > 100000 || !s.has(start + 4, static_cast<size_t>(n) * 4)) return {};
  std::string bytes(4, '\xFF');  // program: keep the plugin's
  bytes += text(s.sub(start + 4, static_cast<size_t>(n) * 4));
  return "vst2p1:" + host::base64Encode(bytes);
}

std::string vst3State(Blob s, std::vector<uint32_t>& paramIds) {
  if (s.size() < 4 || s.u32(0) != 1) return {};
  std::string comp, ctrl;
  for (const Record& r : records(s, 4)) {
    if (r.id == 3) comp = text(r.data);
    else if (r.id == 2) ctrl = text(r.data);
    else if (r.id == 4) {
      const uint32_t n = r.data.u32(0);
      for (uint32_t i = 0; i < n && r.data.has(4 + 4 * static_cast<size_t>(i), 4); ++i) paramIds.push_back(r.data.u32(4 + 4 * static_cast<size_t>(i)));
    }
  }
  if (comp.empty()) return {};
  return host::encodeState(comp, ctrl);
}

std::string auState(Blob s) {
  if (s.size() < 12) return {};
  const uint32_t n = s.u32(4);
  if (!s.has(12, n) || n == 0) return {};
  return "au1:" + host::base64Encode(text(s.sub(12, n)));
}

std::string fold(const std::string& s) {
  std::string o;
  for (char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

}  // namespace

std::string WrapperInfo::savedType() const {
  switch (format) {
    case Vst2: return host::vst2Type(vst2Id);
    case Vst3: return "vst3:" + vst3Uid;
    case Au: return "au:" + auKey;
    default: return {};
  }
}

const char* WrapperInfo::formatName() const {
  switch (format) {
    case Vst2: return "VST2";
    case Vst3: return "VST3";
    case Au: return "AU";
    default: return "?";
  }
}

bool parseWrapper(const uint8_t* p, size_t n, WrapperInfo& w) {
  w = WrapperInfo{};
  Blob b(p, n);
  if (b.size() < 16) return false;
  Blob state;
  bool haveInfo = false;
  Blob guid;
  for (const Record& r : records(b, 4)) {
    switch (r.id) {
      case 50: {
        const uint32_t kind = r.data.u32(0);
        haveInfo = true;
        if (kind == 0 || kind == 4) w.format = WrapperInfo::Vst2;
        else if (kind == 7 || kind == 8) w.format = WrapperInfo::Vst3;
        else if (kind == 9 || kind == 10) w.format = WrapperInfo::Au;
        w.instrument = kind == 4 || kind == 8 || kind == 10;
        break;
      }
      case 51: w.vst2Id = r.data.u32(0); break;
      case 52: guid = r.data; break;
      case 53: state = r.data; break;
      case 54: w.name = text(r.data); break;
      case 55: w.path = text(r.data); break;
      case 56: w.vendor = text(r.data); break;
      default: break;
    }
  }
  if (!haveInfo || w.format == WrapperInfo::None) return false;
  if (w.format == WrapperInfo::Vst3) {
    if (guid.size() < 16) return false;
    w.vst3Uid = hexUid(guid);
    w.data = vst3State(state, w.paramIds);
  } else if (w.format == WrapperInfo::Au) {
    if (guid.size() < 12) return false;
    w.auKey = host::auKey(guid.u32(4), guid.u32(8), guid.u32(0));
    w.data = auState(state);
  } else {
    w.data = vst2State(state);
  }
  if (w.name.empty()) {  // from the path: the file name without extension
    std::string f = w.path;
    for (char& c : f)
      if (c == '\\') c = '/';
    f = f.substr(f.rfind('/') == std::string::npos ? 0 : f.rfind('/') + 1);
    w.name = f.substr(0, f.rfind('.'));
  }
  return true;
}

WrapperMatch matchWrapper(const WrapperInfo& w) {
  WrapperMatch m;
  const std::string saved = w.savedType();
  if (!saved.empty() && host::hostedClass(saved)) {
    m.type = saved;
    m.byId = true;
    m.stateKept = true;
    return m;
  }
  const char* own = w.format == WrapperInfo::Vst2 ? "vst2" : w.format == WrapperInfo::Vst3 ? "vst3" : "au";
  const std::string name = fold(w.name), vendor = fold(w.vendor);
  if (name.empty()) return m;
  std::shared_ptr<const host::HostedClass> sameFormat, other;
  for (const auto& c : host::hostedClasses()) {
    if (c->instrument != w.instrument || fold(c->name) != name) continue;
    if (!vendor.empty() && !c->vendor.empty() && fold(c->vendor).find(vendor) == std::string::npos && vendor.find(fold(c->vendor)) == std::string::npos)
      continue;
    if (c->format == own) {
      if (!sameFormat) sameFormat = c;
    } else if (!other || (c->format == "vst3" && other->format != "vst3")) {
      other = c;  // prefer VST3, then whatever is installed
    }
  }
  if (sameFormat) {
    m.type = sameFormat->type;
    m.stateKept = true;  // same format, different id (version): the state usually still loads
  } else if (other) {
    m.type = other->type;
  }
  return m;
}

bool isImageLineVendor(const std::string& vendor) {
  std::string v;
  for (unsigned char ch : vendor)
    if (std::isalnum(ch)) v += static_cast<char>(std::tolower(ch));
  return v.empty() || v.find("imageline") != std::string::npos;
}

namespace {

bool decodeWrapper(Context& c, const std::vector<Event>& evs, PluginMapping& m, const PluginDecoder& flWrapped) {
  const Event* st = pluginState(evs);
  WrapperInfo w;
  if (!st || !parseWrapper(st->data, st->size, w)) {  // no plugin identity: the FL-plugin decoder reports it
    if (flWrapped) return flWrapped(c, evs, m);
    c.report.counts["hosted plugins: empty wrappers"]++;
    return false;
  }
  const std::string label = w.name + " (" + w.formatName() + ")";
  const WrapperMatch mt = matchWrapper(w);
  if (!mt.type.empty()) {
    m.glType = mt.type;
    m.state = defaultPluginState(mt.type);
    if (mt.stateKept) m.state.data = w.data;
    m.exact = mt.stateKept && !w.data.empty();
    c.report.counts["hosted plugins: installed"]++;
    if (!mt.stateKept) c.report.warn(label + ": only installed in another format; its saved settings could not be carried over");
    // Automation of the wrapper's parameters (FL index → plugin parameter).
    if (auto cls = host::hostedClass(mt.type); cls && mt.stateKept) {
      std::vector<uint32_t> ids = w.paramIds;
      const bool byIndex = w.format != WrapperInfo::Vst3;  // VST2 / AU: FL index = parameter id
      auto index = [cls, ids, byIndex](int fl) -> int {
        if (fl < 0) return -1;
        uint32_t id = byIndex ? static_cast<uint32_t>(fl) : (static_cast<size_t>(fl) < ids.size() ? ids[static_cast<size_t>(fl)] : 0xFFFFFFFFu);
        for (size_t k = 0; k < cls->params.size(); ++k)
          if (cls->params[k].id == id) return static_cast<int>(k);
        return -1;
      };
      m.param = index;
      m.value = [cls, index](int fl, double v) {
        int k = index(fl);
        return k < 0 ? v : cls->fromNormalized(static_cast<size_t>(k), v);
      };
    }
    return true;
  }
  // FL's own plugins loaded through the wrapper (VST builds of FL effects in
  // older projects): the FL plugin decoders map them (FlpFx2Mod.cpp). Only
  // Image-Line's own plugins become built-ins: a third-party plugin is never
  // replaced by a Gridline equivalent, even when its name matches.
  if (flWrapped && isImageLineVendor(w.vendor) && findDecoder(w.name) && findDecoder(w.name) != findDecoder("Fruity Wrapper")) {
    PluginMapping fm;
    fm.flName = m.flName;
    if (flWrapped(c, evs, fm) && !fm.glType.empty()) {
      m = fm;
      c.report.counts["hosted plugins: FL plugins wrapped as VST \xE2\x86\x92 built-in"]++;
      return true;
    }
  }
  // Not installed: keep it as the type it was saved with, with its state, so
  // it loads once the plugin is installed and scanned.
  m.glType = w.savedType();
  m.state.type = m.glType;
  m.state.data = w.data;
  m.exact = false;
  c.report.counts["hosted plugins: missing (kept with their state)"]++;
  c.report.unmapped[label + " - not installed (kept with its state)"]++;
  return true;
}

}  // namespace

void registerWrapperDecoder(std::map<std::string, PluginDecoder>& d) {
  // FL's own plugins inside the wrapper keep the decoder registered before
  // (FlpFx2Mod.cpp); third-party plugins are handled here.
  PluginDecoder fl;
  if (auto it = d.find("Fruity Wrapper"); it != d.end()) fl = it->second;
  d["Fruity Wrapper"] = [fl](Context& c, const std::vector<Event>& evs, PluginMapping& m) { return decodeWrapper(c, evs, m, fl); };
}

}  // namespace gl::flp
