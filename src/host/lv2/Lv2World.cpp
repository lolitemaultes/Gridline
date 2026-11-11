#include "Lv2World.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include "../Backend.h"
#include "../PluginFolders.h"
#include "../PluginScan.h"
#include "../../core/Platform.h"

namespace fs = std::filesystem;

namespace gl::host::lv2 {

namespace {

std::string L(const char* local) { return std::string(kLv2) + local; }
std::string A(const char* local) { return std::string(kAtom) + local; }
const std::string kSeeAlso = std::string(kRdfs) + "seeAlso";
const std::string kLabel = std::string(kRdfs) + "label";
const std::string kPprops = "http://lv2plug.in/ns/ext/port-props#";
const std::string kUnits = "http://lv2plug.in/ns/extensions/units#";
const std::string kDoap = "http://usefulinc.com/ns/doap#";
const std::string kFoaf = "http://xmlns.com/foaf/0.1/";
const std::string kMidiEvent = "http://lv2plug.in/ns/ext/midi#MidiEvent";
const std::string kTimePosition = "http://lv2plug.in/ns/ext/time#Position";
const std::string kMinimumSize = "http://lv2plug.in/ns/ext/resize-port#minimumSize";
const std::string kGroup = "http://lv2plug.in/ns/ext/port-groups#group";

std::string manifestOf(const std::string& bundle) { return (fs::path(bundle) / "manifest.ttl").string(); }

// The bundle path without a trailing separator.
std::string trimmed(std::string bundle) {
  while (bundle.size() > 1 && (bundle.back() == '/' || bundle.back() == '\\')) bundle.pop_back();
  return bundle;
}

// `path` is a file of `bundle` (seeAlso may point anywhere in principle).
bool inBundle(const std::string& path, const std::string& bundle) {
  return path.size() > bundle.size() && path.compare(0, bundle.size(), bundle) == 0 &&
         (path[bundle.size()] == '/' || path[bundle.size()] == '\\');
}

void includeFile(Model& m, const std::string& path, std::string* err) {
  std::string e;
  auto part = sharedFile(path, &e);
  if (part) m.include(std::move(part));
  if (!e.empty() && err && err->empty()) *err = e;
}

// K-180: adds to `m` what `bundle` says about plugin `uri`: its manifest, and the files the manifest
// points to (rdfs:seeAlso) for the plugin, its interfaces and the presets that apply to it, until
// nothing new turns up. Each file is parsed once and shared (sharedFile): a bundle of two hundred
// plugins was read whole for every one of them, a full copy each (gigabytes). `everything`: every
// file the manifest points to, as before (a bundle that describes its plugin some other way).
void includeFor(Model& m, const std::string& bundle, const std::string& uri, bool everything, std::string* err) {
  includeFile(m, manifestOf(bundle), err);
  const std::string uiProp = std::string(kUi) + "ui";
  for (int round = 0; round < 16; ++round) {
    std::vector<std::string> about = {uri};
    for (const Node* n : m.objects(uri, uiProp))
      if (n->isIri()) about.push_back(n->value);
    for (std::string& p : m.subjects(L("appliesTo"), uri)) about.push_back(std::move(p));
    std::vector<std::string> files;
    auto want = [&](const Node& o) {
      std::string path;
      if (!o.isIri() || !uriToPath(o.value, path) || !inBundle(path, bundle) || m.loaded(path)) return;
      if (std::find(files.begin(), files.end(), path) == files.end()) files.push_back(path);
    };
    if (everything) {
      m.forEach([&](const Triple& t) {
        if (t.p == kSeeAlso) want(t.o);
      });
    } else {
      for (const std::string& s : about)
        for (const Node* o : m.objects(s, kSeeAlso)) want(*o);
    }
    if (files.empty()) break;
    for (const auto& f : files) includeFile(m, f, err);
  }
}

std::string unitSymbol(const Model& m, const Node* u) {
  if (!u) return {};
  if (u->isBlank()) return m.text(u->key(), kUnits + "symbol");
  if (!u->isIri()) return {};
  const std::string& v = u->value;
  if (v.compare(0, kUnits.size(), kUnits) != 0) return m.text(v, kUnits + "symbol");
  static const std::map<std::string, std::string> known = {
      {"bar", "bars"},   {"beat", "beats"}, {"bpm", "BPM"},     {"cent", "cents"},      {"cm", "cm"},   {"coef", ""},
      {"db", "dB"},      {"degree", "deg"}, {"frame", "frames"}, {"hz", "Hz"},          {"inch", "in"}, {"khz", "kHz"},
      {"km", "km"},      {"m", "m"},        {"mhz", "MHz"},     {"midiNote", "note"},   {"mile", "mi"}, {"min", "min"},
      {"mm", "mm"},      {"ms", "ms"},      {"oct", "oct"},     {"pc", "%"},            {"s", "s"},     {"semitone12TET", "st"}};
  auto it = known.find(v.substr(kUnits.size()));
  return it != known.end() ? it->second : m.text(v, kUnits + "symbol");
}

Port readPort(const Model& m, const std::string& p) {
  Port port;
  port.index = static_cast<uint32_t>(std::max(0.0, m.object(p, L("index")) ? m.object(p, L("index"))->number() : 0));
  port.symbol = m.text(p, L("symbol"));
  port.name = m.text(p, L("name"));
  if (port.name.empty()) port.name = port.symbol;
  bool in = false, out = false;
  for (const Node* t : m.objects(p, kRdfType)) {
    if (!t->isIri()) continue;
    const std::string& v = t->value;
    if (v == L("InputPort")) in = true;
    else if (v == L("OutputPort")) out = true;
    else if (v == L("AudioPort")) port.type = Port::Audio;
    else if (v == L("ControlPort")) port.type = Port::Control;
    else if (v == L("CVPort")) port.type = Port::CV;
    else if (v == A("AtomPort")) port.type = Port::Atom;
  }
  port.input = in || !out;
  if (const Node* n = m.object(p, L("minimum")); n && n->isLiteral()) port.hasMin = true, port.min = static_cast<float>(n->number());
  if (const Node* n = m.object(p, L("maximum")); n && n->isLiteral()) port.hasMax = true, port.max = static_cast<float>(n->number());
  if (const Node* n = m.object(p, L("default")); n && n->isLiteral()) port.hasDef = true, port.def = static_cast<float>(n->number());
  for (const Node* pp : m.objects(p, L("portProperty"))) {
    if (!pp->isIri()) continue;
    const std::string& v = pp->value;
    if (v == L("toggled")) port.toggled = true;
    else if (v == L("integer")) port.integer = true;
    else if (v == L("enumeration")) port.enumeration = true;
    else if (v == L("sampleRate")) port.sampleRate = true;
    else if (v == L("connectionOptional")) port.optional = true;
    else if (v == L("reportsLatency")) port.reportsLatency = true;
    else if (v == L("isSideChain")) port.sideChain = true;
    else if (v == kPprops + "logarithmic") port.logarithmic = true;
    else if (v == kPprops + "notOnGUI") port.notOnGui = true;
    else if (v == kPprops + "notAutomatic") port.notAutomatic = true;
  }
  if (const Node* d = m.object(p, L("designation")); d && d->isIri()) port.designation = d->value;
  if (port.designation == L("control")) port.controlDesignated = true;
  if (port.designation == L("latency")) port.reportsLatency = true;
  for (const Node* sp : m.objects(p, L("scalePoint"))) {
    if (!sp->isResource()) continue;
    const Node* v = m.object(sp->key(), "http://www.w3.org/1999/02/22-rdf-syntax-ns#value");
    if (!v) continue;
    port.points.push_back({static_cast<float>(v->number()), m.text(sp->key(), kLabel)});
  }
  std::sort(port.points.begin(), port.points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  port.unit = unitSymbol(m, m.object(p, kUnits + "unit"));
  for (const Node* s : m.objects(p, A("supports"))) {
    if (!s->isIri()) continue;
    if (s->value == kMidiEvent) port.midi = true;
    if (s->value == kTimePosition) port.timePosition = true;
  }
  if (const Node* n = m.object(p, kMinimumSize)) port.minimumSize = static_cast<uint32_t>(std::max(0.0, n->number()));
  if (const Node* g = m.object(p, kGroup); g && g->isIri()) port.group = g->value;
  return port;
}

std::vector<std::string> iris(const Model& m, const std::string& s, const std::string& p) {
  std::vector<std::string> out;
  for (const Node* n : m.objects(s, p))
    if (n->isIri()) out.push_back(n->value);
  return out;
}

// Descriptions in use, by URI. K-180: held only while something uses them (an instance), so the
// files a scan reads for one plugin are freed before the next; a description is quick to make again.
std::mutex gCacheLock;
std::map<std::string, std::weak_ptr<const PluginDesc>>& cache() {
  static auto* c = new std::map<std::string, std::weak_ptr<const PluginDesc>>;  // never destroyed
  return *c;
}

// K-180: which bundles' manifests mention an IRI (a plugin of another bundle they add presets or
// interfaces to). Built once for the search folders, not once a plugin: describe() read every
// bundle's manifest again for each plugin. Made again when the folders change or when bundles are
// looked for again (pluginBundles).
struct BundleIndex {
  std::vector<std::string> folders;
  bool built = false;
  std::vector<std::string> bundles, canonical;          // every bundle of the folders
  std::map<std::string, std::vector<size_t>> mentions;  // IRI -> bundles (indices)
  std::map<std::string, std::vector<size_t>> declares;  // plugin URI -> bundles declaring it (copies of it)
};
std::mutex gIndexLock;
BundleIndex& bundleIndex() {
  static auto* x = new BundleIndex;  // never destroyed
  return *x;
}
std::atomic<int> gIndexBuilds{0};

std::string canonicalPath(const std::string& p) {
  std::error_code ec;
  fs::path c = fs::weakly_canonical(p, ec);
  return trimmed(ec ? p : c.string());
}

void buildIndex(BundleIndex& x, const std::vector<std::string>& folders) {
  x = BundleIndex{};
  x.folders = folders;
  x.built = true;
  ++gIndexBuilds;
  const std::string type = kRdfType, binary = L("binary");
  for (const auto& b : allBundles(folders)) {
    Model man;
    if (!man.loadFile(manifestOf(b))) continue;
    const size_t k = x.bundles.size();
    x.bundles.push_back(trimmed(b));
    x.canonical.push_back(canonicalPath(b));
    auto note = [&](const std::string& iri) {
      if (iri.rfind("file:", 0) == 0) return;  // the bundle's own files
      auto& v = x.mentions[iri];
      if (v.empty() || v.back() != k) v.push_back(k);
    };
    const std::string plugin = L("Plugin");
    for (const auto& t : man.triples()) {
      if (t.s.isIri() && t.p == type && t.o.isIri() && t.o.value == plugin) x.declares[t.s.value].push_back(k);
      if (t.s.isIri()) note(t.s.value);
      if (t.o.isIri() && t.p != type && t.p != kSeeAlso && t.p != binary) note(t.o.value);
    }
  }
}

// The bundles other than `self` whose manifests mention `uri`. A bundle that declares the plugin
// itself is another copy of it (installed twice, in two folders), not an addition: left out.
std::vector<std::string> bundlesMentioning(const std::string& uri, const std::string& self) {
  const std::vector<std::string> folders = host::searchFolders("lv2");  // K-13: the LV2 search folders the user set
  const std::string selfCanonical = canonicalPath(self);
  std::lock_guard<std::mutex> g(gIndexLock);
  BundleIndex& x = bundleIndex();
  if (!x.built || x.folders != folders) buildIndex(x, folders);
  std::vector<std::string> out;
  auto it = x.mentions.find(uri);
  if (it == x.mentions.end()) return out;
  const auto copies = x.declares.find(uri);
  for (size_t k : it->second) {
    if (x.canonical[k] == selfCanonical || x.bundles[k] == self) continue;
    if (copies != x.declares.end() && std::find(copies->second.begin(), copies->second.end(), k) != copies->second.end()) continue;
    out.push_back(x.bundles[k]);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- discovery

std::vector<std::string> defaultPaths() {
  std::vector<std::string> v;
  const char* home = gl::platform::homeEnv();
#if defined(__APPLE__)
  if (home) v.push_back(std::string(home) + "/Library/Audio/Plug-Ins/LV2");
  v.push_back("/Library/Audio/Plug-Ins/LV2");
  if (home) v.push_back(std::string(home) + "/.lv2");
  v.push_back("/usr/local/lib/lv2");
#elif defined(_WIN32)
  // The LV2 specification's Windows locations: %APPDATA%\LV2 and %COMMONPROGRAMFILES%\LV2.
  (void)home;
  for (const std::string& p : {host::envFolder("APPDATA", "LV2"), host::envFolder("CommonProgramFiles", "LV2")})
    if (!p.empty()) v.push_back(p);
#else
  if (home) v.push_back(std::string(home) + "/.lv2");
  v.push_back("/usr/local/lib/lv2");
  v.push_back("/usr/lib/lv2");
  v.push_back("/usr/pkg/lib/lv2");
#endif
  addEnvPaths(v, "LV2_PATH");
  return v;
}

std::vector<std::string> allBundles(const std::vector<std::string>& folders) {
  std::vector<std::string> out;
  for (auto& b : findEntries(folders, ".lv2", true, false)) {
    std::error_code ec;
    if (fs::is_regular_file(manifestOf(b), ec)) out.push_back(b);
  }
  return out;
}

std::vector<std::string> pluginUris(const std::string& bundle, std::string* err) {
  auto m = sharedFile(manifestOf(trimmed(bundle)), err);
  return m ? m->subjects(kRdfType, L("Plugin")) : std::vector<std::string>{};
}

void forgetBundleIndex() {
  std::lock_guard<std::mutex> g(gIndexLock);
  bundleIndex().built = false;
}

int bundleIndexBuilds() { return gIndexBuilds.load(); }

std::vector<std::string> pluginBundles(const std::vector<std::string>& folders) {
  forgetBundleIndex();  // bundles may have come or gone: the next description looks again
  std::vector<std::string> out;
  for (auto& b : allBundles(folders))
    if (!pluginUris(b).empty()) out.push_back(b);
  return out;
}

int64_t bundleMtime(const std::string& bundle) {
  int64_t best = pathMtime(bundle);
  std::error_code ec;
  for (fs::directory_iterator it(bundle, ec), end; !ec && it != end; it.increment(ec)) {
    auto t = fs::last_write_time(it->path(), ec);
    if (ec) {
      ec.clear();
      continue;
    }
    best = std::max<int64_t>(best, platform::fileTimeSeconds(t));
  }
  return best;
}

// ---------------------------------------------------------------- description

std::shared_ptr<const PluginDesc> describe(const std::string& bundleIn, const std::string& uri, std::string* err) {
  const std::string bundle = trimmed(bundleIn);
  {
    std::lock_guard<std::mutex> g(gCacheLock);
    auto it = cache().find(uri);
    if (it != cache().end())
      if (auto d = it->second.lock(); d && d->bundle == bundle) return d;
  }
  auto model = std::make_shared<Model>();
  std::string loadErr;
  includeFor(*model, bundle, uri, false, &loadErr);
  // A bundle that describes its plugin in files the manifest ties to something else: all of them.
  if (model->has(uri, kRdfType, L("Plugin")) && model->objects(uri, L("port")).empty()) includeFor(*model, bundle, uri, true, &loadErr);
  if (!model->has(uri, kRdfType, L("Plugin"))) {
    if (err) *err = loadErr.empty() ? "plugin " + uri + " not described in " + bundle : loadErr;
    return nullptr;
  }
  // Other bundles that add to this plugin (UIs, presets).
  for (const auto& b : bundlesMentioning(uri, bundle)) includeFor(*model, b, uri, false, nullptr);
  const Model& m = *model;
  auto d = std::make_shared<PluginDesc>();
  d->uri = uri;
  d->bundle = bundle;
  d->model = model;
  d->name = m.text(uri, kDoap + "name");
  if (d->name.empty()) d->name = m.text(uri, kLabel);
  if (d->name.empty()) d->name = uri;
  for (const char* who : {"maintainer", "developer"}) {
    if (!d->author.empty()) break;
    if (const Node* n = m.object(uri, kDoap + who); n && n->isResource()) d->author = m.text(n->key(), kFoaf + "name");
  }
  if (d->author.empty())
    if (const Node* proj = m.object(uri, L("project")); proj && proj->isResource()) {
      if (const Node* n = m.object(proj->key(), kDoap + "maintainer"); n && n->isResource()) d->author = m.text(n->key(), kFoaf + "name");
      if (d->author.empty()) d->author = m.text(proj->key(), kDoap + "name");
    }
  const Node* minor = m.object(uri, L("minorVersion"));
  const Node* micro = m.object(uri, L("microVersion"));
  if (minor || micro)
    d->version = "0." + std::to_string(minor ? static_cast<int>(minor->number()) : 0) + "." + std::to_string(micro ? static_cast<int>(micro->number()) : 0);
  if (const Node* bin = m.object(uri, L("binary")); bin && bin->isIri()) uriToPath(bin->value, d->binary);
  if (d->binary.empty()) {
    if (err) *err = "no lv2:binary for " + uri;
    return nullptr;
  }
  d->types = iris(m, uri, kRdfType);
  d->instrumentClass = std::find(d->types.begin(), d->types.end(), L("InstrumentPlugin")) != d->types.end();
  d->required = iris(m, uri, L("requiredFeature"));
  d->optional = iris(m, uri, L("optionalFeature"));
  d->extensionData = iris(m, uri, L("extensionData"));
  d->defaultState = m.object(uri, std::string(kState) + "state") != nullptr;
  for (const Node* p : m.objects(uri, L("port")))
    if (p->isResource()) d->ports.push_back(readPort(m, p->key()));
  std::sort(d->ports.begin(), d->ports.end(), [](const Port& a, const Port& b) { return a.index < b.index; });
  for (size_t k = 0; k < d->ports.size(); ++k)
    if (d->ports[k].index != k) {
      if (err) *err = "port indices of " + uri + " are not 0.." + std::to_string(d->ports.size() - 1);
      return nullptr;
    }
  // UIs.
  for (const std::string& u : iris(m, uri, std::string(kUi) + "ui")) {
    Ui ui;
    ui.uri = u;
    for (const std::string& t : iris(m, u, kRdfType)) {
      if (t == std::string(kUi) + "CocoaUI") ui.kind = Ui::Cocoa, ui.typeUri = t;
      else if (t == std::string(kUi) + "X11UI") ui.kind = Ui::X11, ui.typeUri = t;
      else if (t == std::string(kUi) + "WindowsUI") ui.kind = Ui::Windows, ui.typeUri = t;
      else if ((t == kKxExternalUi || t == kOldExternalUi) && ui.kind == Ui::Other) ui.kind = Ui::External, ui.typeUri = t;
    }
    if (const Node* bin = m.object(u, std::string(kUi) + "binary"); bin && bin->isIri()) uriToPath(bin->value, ui.binary);
    if (ui.binary.empty())
      if (const Node* bin = m.object(u, L("binary")); bin && bin->isIri()) uriToPath(bin->value, ui.binary);
    if (ui.binary.empty()) continue;
    ui.bundle = fs::path(ui.binary).parent_path().string() + "/";
    ui.required = iris(m, u, L("requiredFeature"));
    ui.optional = iris(m, u, L("optionalFeature"));
    d->uis.push_back(std::move(ui));
  }
  // Presets.
  for (const std::string& p : m.subjects(L("appliesTo"), uri)) {
    if (!m.has(p, kRdfType, std::string(kPset) + "Preset")) continue;
    PresetInfo pi;
    pi.uri = p;
    pi.label = m.text(p, kLabel);
    if (pi.label.empty()) {
      pi.label = p;
      size_t cut = pi.label.find_last_of("#/");
      if (cut != std::string::npos && cut + 1 < pi.label.size()) pi.label = pi.label.substr(cut + 1);
    }
    d->presets.push_back(std::move(pi));
  }
  std::stable_sort(d->presets.begin(), d->presets.end(), [](const PresetInfo& a, const PresetInfo& b) { return a.label < b.label; });
  std::lock_guard<std::mutex> g(gCacheLock);
  for (auto it = cache().begin(); it != cache().end();) it = it->second.expired() ? cache().erase(it) : std::next(it);
  cache()[uri] = d;
  return d;
}

// ---------------------------------------------------------------- state data

bool readState(const Model& m, const std::string& subject, StateData& out) {
  out = StateData{};
  bool any = false;
  for (const Node* p : m.objects(subject, L("port"))) {
    if (!p->isResource()) continue;
    const std::string sym = m.text(p->key(), L("symbol"));
    const Node* v = m.object(p->key(), std::string(kPset) + "value");
    if (sym.empty() || !v) continue;
    out.ports.push_back({sym, static_cast<float>(v->number())});
    any = true;
  }
  const Node* st = m.object(subject, std::string(kState) + "state");
  if (st && st->isResource()) {
    const std::string x = kXsd;
    for (const Triple* t : m.about(st->key())) {
      StateValue sv;
      sv.key = t->p;
      const Node& o = t->o;
      if (o.isIri()) {
        std::string path;
        if (uriToPath(o.value, path)) {
          sv.type = A("Path");
          sv.data = path + '\0';
        } else {
          sv.type = A("URID");
          sv.data = o.value;
        }
      } else if (o.isLiteral()) {
        const std::string& dt = o.datatype;
        auto put = [&sv](const void* p, size_t n) { sv.data.assign(static_cast<const char*>(p), n); };
        if (dt.empty() || dt == x + "string") {
          sv.type = A("String");
          sv.data = o.value + '\0';
        } else if (dt == x + "int" || dt == x + "integer") {
          int32_t v = static_cast<int32_t>(o.number());
          sv.type = A("Int");
          put(&v, sizeof v);
        } else if (dt == x + "long") {
          int64_t v = static_cast<int64_t>(o.number());
          sv.type = A("Long");
          put(&v, sizeof v);
        } else if (dt == x + "float" || dt == x + "decimal") {
          float v = static_cast<float>(o.number());
          sv.type = A("Float");
          put(&v, sizeof v);
        } else if (dt == x + "double") {
          double v = o.number();
          sv.type = A("Double");
          put(&v, sizeof v);
        } else if (dt == x + "boolean") {
          int32_t v = o.truth() ? 1 : 0;
          sv.type = A("Bool");
          put(&v, sizeof v);
        } else {
          // base64 body with the atom type as datatype (or xsd:base64Binary: a chunk)
          sv.type = dt == x + "base64Binary" ? A("Chunk") : dt;
          if (!base64Decode(o.value, sv.data)) continue;
        }
      } else {
        continue;  // nested objects are not supported
      }
      out.props.push_back(std::move(sv));
      any = true;
    }
  }
  return any;
}

}  // namespace gl::host::lv2
