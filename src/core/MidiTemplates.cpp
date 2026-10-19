#include "MidiTemplates.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "Json.h"

namespace gl {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const char* kActionKeys[] = {"",           "play",        "stop",        "record", "songmode",
                             "metronome",  "countin",     "steprecord",  "looprecord",
                             "nextpattern", "prevpattern", "undo",       "redo",   "taptempo"};

RemoteAction actionFromKey(const std::string& k) {
  for (int i = 1; i < static_cast<int>(RemoteAction::Count); ++i)
    if (k == kActionKeys[i]) return static_cast<RemoteAction>(i);
  return RemoteAction::None;
}

const char* encoderKey(RemoteEncoder e) {
  switch (e) {
    case RemoteEncoder::TwosComplement: return "twos";
    case RemoteEncoder::BinaryOffset: return "offset";
    case RemoteEncoder::SignBit: return "signbit";
    default: return "absolute";
  }
}
RemoteEncoder encoderFromKey(const std::string& k) {
  if (k == "twos" || k == "twoscomplement") return RemoteEncoder::TwosComplement;
  if (k == "offset" || k == "binaryoffset") return RemoteEncoder::BinaryOffset;
  if (k == "signbit" || k == "sign") return RemoteEncoder::SignBit;
  return RemoteEncoder::Absolute;
}
const char* takeoverKey(RemoteTakeover t) {
  return t == RemoteTakeover::Pickup ? "pickup" : t == RemoteTakeover::Scale ? "scale" : "jump";
}

int channelOf(const Json& j) {
  int c = jsonInt<int>(j.num("channel", 0));
  return c >= 1 && c <= 16 ? c - 1 : -1;
}

bool controlFrom(const Json& j, TemplateControl& c) {
  if (!j.is(Json::Type::Object)) return false;
  c.channel = channelOf(j);
  if (const Json* v = j.find("cc"); v && v->is(Json::Type::Number)) {
    c.source = RemoteSource::CC;
    c.number = std::clamp(jsonInt<int>(v->number), 0, 127);
    return true;
  }
  if (const Json* v = j.find("note"); v && v->is(Json::Type::Number)) {
    c.source = RemoteSource::Note;
    c.number = std::clamp(jsonInt<int>(v->number), 0, 127);
    return true;
  }
  return false;
}

// "knobs": {"cc": [..], "notes": [..], "channel": n} or a list of controls.
std::vector<TemplateControl> controlList(const Json& j, RemoteEncoder* enc) {
  std::vector<TemplateControl> out;
  if (j.is(Json::Type::Array)) {
    for (const auto& e : j.array) {
      TemplateControl c;
      if (controlFrom(e, c)) out.push_back(c);
    }
    return out;
  }
  if (!j.is(Json::Type::Object)) return out;
  int ch = channelOf(j);
  if (enc) *enc = encoderFromKey(lower(j.text("encoder", "absolute")));
  for (const char* key : {"cc", "notes"})
    if (const Json* a = j.find(key); a && a->is(Json::Type::Array))
      for (const auto& v : a->array)
        if (v.is(Json::Type::Number)) {
          TemplateControl c;
          c.source = key[0] == 'c' ? RemoteSource::CC : RemoteSource::Note;
          c.number = std::clamp(jsonInt<int>(v.number), 0, 127);
          c.channel = ch;
          out.push_back(c);
        }
  return out;
}

Json controlJson(const TemplateControl& c) {
  Json j = Json::makeObject();
  j.object[c.source == RemoteSource::Note ? "note" : "cc"] = Json::makeNumber(c.number);
  if (c.channel >= 0) j.object["channel"] = Json::makeNumber(c.channel + 1);
  return j;
}

bool controlMatches(const TemplateControl& c, const MidiMessage& m) {
  if (c.channel >= 0 && c.channel != m.channel()) return false;
  if (c.source == RemoteSource::CC) return m.kind() == 0xB0 && m.d1 == c.number;
  if (c.source == RemoteSource::Note) return (m.kind() == 0x90 || m.kind() == 0x80) && m.d1 == c.number;
  return false;
}

}  // namespace

const char* templateActionKey(RemoteAction a) {
  int i = static_cast<int>(a);
  return i > 0 && i < static_cast<int>(RemoteAction::Count) ? kActionKeys[i] : "";
}

bool parseControllerTemplate(const std::string& text, ControllerTemplate& t, std::string* err) {
  Json j;
  size_t pos = 0;
  if (!jsonParse(text, j, &pos) || !j.is(Json::Type::Object)) {
    if (err) *err = "not a JSON object (offset " + std::to_string(pos) + ")";
    return false;
  }
  t = ControllerTemplate{};
  t.name = j.text("name");
  if (t.name.empty()) {
    if (err) *err = "missing \"name\"";
    return false;
  }
  t.id = j.text("id");
  if (const Json* m = j.find("match"); m && m->is(Json::Type::Array))
    for (const auto& v : m->array)
      if (v.is(Json::Type::String) && !v.str.empty()) t.match.push_back(v.str);
  std::string to = lower(j.text("takeover", "jump"));
  t.takeover = to == "pickup" ? RemoteTakeover::Pickup : to == "scale" ? RemoteTakeover::Scale : RemoteTakeover::Jump;
  if (const Json* v = j.find("mmc")) t.mmc = v->is(Json::Type::Bool) ? v->boolean : v->number != 0;
  if (const Json* tr = j.find("transport"); tr && tr->is(Json::Type::Object))
    for (const auto& [k, v] : tr->object) {
      RemoteAction a = actionFromKey(lower(k));
      TemplateControl c;
      if (a != RemoteAction::None && controlFrom(v, c)) t.transport.push_back({a, c});
    }
  if (const Json* k = j.find("knobs")) t.knobs = controlList(*k, &t.knobEncoder);
  if (const Json* f = j.find("faders")) t.faders = controlList(*f, nullptr);
  return true;
}

std::string controllerTemplateJson(const ControllerTemplate& t) {
  Json j = Json::makeObject();
  j.object["name"] = Json::makeString(t.name);
  if (!t.id.empty()) j.object["id"] = Json::makeString(t.id);
  Json m = Json::makeArray();
  for (const auto& s : t.match) m.array.push_back(Json::makeString(s));
  j.object["match"] = std::move(m);
  j.object["takeover"] = Json::makeString(takeoverKey(t.takeover));
  if (t.mmc) {
    Json b;
    b.type = Json::Type::Bool;
    b.boolean = true;
    j.object["mmc"] = b;
  }
  Json tr = Json::makeObject();
  for (const auto& [a, c] : t.transport) tr.object[templateActionKey(a)] = controlJson(c);
  j.object["transport"] = std::move(tr);
  auto list = [](const std::vector<TemplateControl>& v, RemoteEncoder* enc) {
    Json o = Json::makeObject();
    bool same = !v.empty();
    for (const auto& c : v) same = same && c.source == v[0].source && c.channel == v[0].channel;
    if (!same) {
      Json a = Json::makeArray();
      for (const auto& c : v) a.array.push_back(controlJson(c));
      return a;
    }
    Json nums = Json::makeArray();
    for (const auto& c : v) nums.array.push_back(Json::makeNumber(c.number));
    o.object[v[0].source == RemoteSource::Note ? "notes" : "cc"] = std::move(nums);
    if (v[0].channel >= 0) o.object["channel"] = Json::makeNumber(v[0].channel + 1);
    if (enc) o.object["encoder"] = Json::makeString(encoderKey(*enc));
    return o;
  };
  RemoteEncoder enc = t.knobEncoder;
  if (!t.knobs.empty()) j.object["knobs"] = list(t.knobs, &enc);
  if (!t.faders.empty()) j.object["faders"] = list(t.faders, nullptr);
  return jsonStringify(j) + "\n";
}

ControllerTemplate genericTemplate() {
  ControllerTemplate t;
  t.id = "generic";
  t.name = "Generic controller";
  t.match = {"*"};
  t.mmc = true;
  // Common factory defaults of many keyboards' "generic / DAW-less" modes.
  const std::pair<RemoteAction, int> tr[] = {{RemoteAction::PlayPause, 117}, {RemoteAction::Stop, 116},
                                             {RemoteAction::Record, 118},    {RemoteAction::LoopRecord, 113}};
  for (auto [a, cc] : tr) t.transport.push_back({a, {RemoteSource::CC, cc, -1}});
  for (int i = 0; i < 8; ++i) t.knobs.push_back({RemoteSource::CC, 21 + i, -1});
  return t;
}

std::vector<ControllerTemplate> loadControllerTemplates(const std::vector<std::string>& dirs, std::vector<std::string>* errors) {
  std::vector<ControllerTemplate> out = {genericTemplate()};
  for (const auto& d : dirs) {
    std::error_code ec;
    if (d.empty() || !fs::is_directory(d, ec)) continue;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(d, ec))
      if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      std::ifstream in(f);
      std::stringstream ss;
      ss << in.rdbuf();
      ControllerTemplate t;
      std::string err;
      if (!parseControllerTemplate(ss.str(), t, &err)) {
        if (errors) errors->push_back(f.filename().string() + ": " + err);
        continue;
      }
      if (t.id.empty()) t.id = f.stem().string();
      t.source = f.string();
      auto it = std::find_if(out.begin(), out.end(), [&](const ControllerTemplate& x) { return x.id == t.id; });
      if (it != out.end())
        *it = t;
      else
        out.push_back(t);
    }
  }
  return out;
}

const ControllerTemplate* matchControllerTemplate(const std::vector<ControllerTemplate>& all, const std::string& device) {
  const std::string dev = lower(device);
  const ControllerTemplate* best = nullptr;
  size_t bestLen = 0;
  const ControllerTemplate* any = nullptr;
  for (const auto& t : all)
    for (const auto& m : t.match) {
      if (m == "*") {
        if (!any) any = &t;
        continue;
      }
      std::string lm = lower(m);
      if (dev.find(lm) != std::string::npos && lm.size() > bestLen) {
        best = &t;
        bestLen = lm.size();
      }
    }
  return best ? best : any;
}

const ControllerTemplate* findControllerTemplate(const std::vector<ControllerTemplate>& all, const std::string& id) {
  for (const auto& t : all)
    if (t.id == id) return &t;
  return nullptr;
}

TemplateHit templateLookup(const ControllerTemplate& t, const MidiMessage& m) {
  TemplateHit h;
  for (const auto& [a, c] : t.transport)
    if (controlMatches(c, m)) {
      h.kind = TemplateHit::Action;
      h.action = a;
      return h;
    }
  for (size_t i = 0; i < t.knobs.size(); ++i)
    if (controlMatches(t.knobs[i], m)) {
      h.kind = TemplateHit::Knob;
      h.index = static_cast<int>(i);
      return h;
    }
  for (size_t i = 0; i < t.faders.size(); ++i)
    if (controlMatches(t.faders[i], m)) {
      h.kind = TemplateHit::Fader;
      h.index = static_cast<int>(i);
      return h;
    }
  return h;
}

RemoteAction parseMmc(const std::vector<unsigned char>& x) {
  // F0 7F <device> 06 <command> F7
  if (x.size() < 6 || x[0] != 0xF0 || x[1] != 0x7F || x[3] != 0x06) return RemoteAction::None;
  switch (x[4]) {
    case 0x01: return RemoteAction::Stop;
    case 0x02:  // play
    case 0x03:  // deferred play
      return RemoteAction::PlayPause;
    case 0x06: return RemoteAction::Record;  // record strobe
    case 0x09: return RemoteAction::Stop;    // pause
    default: return RemoteAction::None;
  }
}

}  // namespace gl
