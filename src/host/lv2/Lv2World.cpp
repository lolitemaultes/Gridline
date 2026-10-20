#include "Lv2World.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include "../Backend.h"
#include "../PluginScan.h"

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

// Loads a bundle's manifest and every file it points to with rdfs:seeAlso.
void loadBundle(Model& m, const std::string& bundle, std::string* err) {
  const std::string man = manifestOf(bundle);
  if (m.loaded(man)) return;
  std::string e;
  if (!m.loadFile(man, &e) && err && err->empty()) *err = e;
  std::vector<std::string> files;
  for (const auto& t : m.triples()) {
    if (t.p != kSeeAlso || !t.o.isIri()) continue;
    std::string path;
    if (!uriToPath(t.o.value, path)) continue;
    // Only files of this bundle (seeAlso may point anywhere in principle).
    if (path.compare(0, bundle.size(), bundle) != 0) continue;
    if (std::find(files.begin(), files.end(), path) == files.end()) files.push_back(path);
  }
  for (const auto& f : files) {
    std::string fe;
    if (!m.loadFile(f, &fe) && err && err->empty()) *err = fe;
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

std::mutex gCacheLock;
std::map<std::string, std::shared_ptr<const PluginDesc>>& cache() {
  static auto* c = new std::map<std::string, std::shared_ptr<const PluginDesc>>;  // never destroyed
  return *c;
}

}  // namespace

// ---------------------------------------------------------------- discovery

std::vector<std::string> defaultPaths() {
  std::vector<std::string> v;
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home) v.push_back(std::string(home) + "/Library/Audio/Plug-Ins/LV2");
  v.push_back("/Library/Audio/Plug-Ins/LV2");
  if (home) v.push_back(std::string(home) + "/.lv2");
  v.push_back("/usr/local/lib/lv2");
#else
  if (home) v.push_back(std::string(home) + "/.lv2");
  v.push_back("/usr/local/lib/lv2");
  v.push_back("/usr/lib/lv2");
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
  Model m;
  m.loadFile(manifestOf(bundle), err);
  return m.subjects(kRdfType, L("Plugin"));
}

std::vector<std::string> pluginBundles(const std::vector<std::string>& folders) {
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
    best = std::max<int64_t>(best, std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
  }
  return best;
}

// ---------------------------------------------------------------- description

std::shared_ptr<const PluginDesc> describe(const std::string& bundleIn, const std::string& uri, std::string* err) {
  std::string bundle = bundleIn;
  while (bundle.size() > 1 && bundle.back() == '/') bundle.pop_back();
  {
    std::lock_guard<std::mutex> g(gCacheLock);
    auto it = cache().find(uri);
    if (it != cache().end() && it->second->bundle == bundle) return it->second;
  }
  auto model = std::make_shared<Model>();
  std::string loadErr;
  loadBundle(*model, bundle, &loadErr);
  if (!model->has(uri, kRdfType, L("Plugin"))) {
    if (err) *err = loadErr.empty() ? "plugin " + uri + " not described in " + bundle : loadErr;
    return nullptr;
  }
  // Other bundles that add to this plugin (UIs, presets).
  for (const auto& b : allBundles(defaultPaths())) {
    std::error_code ec;
    if (fs::equivalent(b, bundle, ec)) continue;
    Model man;
    if (!man.loadFile(manifestOf(b))) continue;
    bool mentions = false;
    for (const auto& t : man.triples())
      if ((t.s.isIri() && t.s.value == uri) || (t.o.isIri() && t.o.value == uri)) {
        mentions = true;
        break;
      }
    if (mentions) loadBundle(*model, b, nullptr);
  }
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
