// Gridline - K-78: a project's third-party plugin opens in another installed format of the same
// plugin. Ids that belong together, the scored matcher (kinds, vendors, outputs, parameter names,
// no guess between two products), the state wrappings of each format and their round trips, the
// project document (type, state, parameters and automation move; the identity is saved with every
// alias; the saved format wins while it is installed), stand-ins of a plugin installed in another
// format, and the FL import using the same matcher.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Json.h"
#include "core/Params.h"
#include "core/Plist.h"
#include "core/PluginInfo.h"
#include "core/PluginStandIn.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/flp/FlpWrapper.h"
#include "host/Backend.h"
#include "host/CrossFormat.h"
#include "host/HostedPlugins.h"
#include "host/PluginIdentity.h"
#include "host/PluginScan.h"
#include "host/StateConvert.h"

using namespace gl;
using namespace gl::host;

namespace {

std::shared_ptr<const HostedClass> fake(const std::string& type, const std::string& name, const std::string& vendor, bool instrument,
                                        const std::vector<std::string>& params, int outs = 2, int ins = 2) {
  ScanResult r;
  r.format = type.substr(0, type.find(':'));
  r.bundle = "/fake/" + type;
  r.ok = true;
  ScannedClass c;
  c.uid = type.substr(type.find(':') + 1);
  c.name = name;
  c.vendor = vendor;
  c.instrument = instrument;
  c.outputChannels = outs;
  c.inputChannels = instrument ? 0 : ins;
  for (size_t i = 0; i < params.size(); ++i) {
    ScannedParam p;
    p.id = static_cast<uint32_t>(1000 + i * 7);  // ids differ per format: names carry the match
    p.title = params[i];
    p.flags = kParamAutomatable;
    c.params.push_back(p);
  }
  r.classes.push_back(c);
  registerScanResult(r);
  return hostedClass(type);
}

std::shared_ptr<const HostedClass> local(const std::string& type, const std::string& name, const std::string& vendor, bool instrument,
                                         const std::vector<std::string>& params, int outs = 2) {
  // A class description only (not registered): matcher input.
  ScannedClass c;
  c.uid = type.substr(type.find(':') + 1);
  c.name = name;
  c.vendor = vendor;
  c.instrument = instrument;
  c.outputChannels = outs;
  c.inputChannels = instrument ? 0 : 2;
  for (const auto& t : params) {
    ScannedParam p;
    p.title = t;
    p.flags = kParamAutomatable;
    c.params.push_back(p);
  }
  return std::make_shared<const HostedClass>(makeHostedClass(c, "/x", type.substr(0, type.find(':'))));
}

const std::vector<std::string> kFatParams = {"Fatness", "Gain", "Color", "Mix"};

void ids() {
  using namespace ident;
  // Steinberg's VST2-compatible class id: "VST", the id, nine name letters in lower case.
  const std::string u = vst2CompatibleVst3Uid(0x41424344u, "Fat Thing Pro");
  expect(u == "56535441424344666174207468696E67", "VST2-compatible uid layout");
  uint32_t v2 = 0;
  expect(vst2IdOfVst3Uid(u, v2) && v2 == 0x41424344u, "VST2 id read back from the uid");
  expect(!vst2IdOfVst3Uid("ABCDEF019182FAEB544C444E444C5346", v2), "a JUCE uid is not VST2-compatible");
  expect(idRelation("vst2:41424344", "Fat Thing Pro", "vst3:" + u, "Fat Thing Pro") == 100, "vst2 <-> vst3 compatible id: certain");
  expect(idRelation("vst3:" + u, "Fat Thing Pro", "vst2:41424344", "Fat Thing Pro") == 100, "relation is symmetric");
  expect(idRelation("vst2:41424345", "Fat Thing Pro", "vst3:" + u, "Fat Thing Pro") == 0, "another VST2 id: no relation");
  // JUCE: the VST3 class id from the AU manufacturer and plugin codes.
  uint32_t t = 0, s = 0, m = 0;
  expect(parseAuKey("au:aufx/DLSF/TLDN", t, s, m) && s == 0x444C5346u && m == 0x544C444Eu, "AU key parsed");
  expect(parseAuKey("aumu/0x00000001/Abcd", t, s, m) && s == 1u, "AU key with a hex code");
  expect(juceVst3Uid(0x544C444Eu, 0x444C5346u) == "ABCDEF019182FAEB544C444E444C5346", "JUCE VST3 uid layout");
  expect(idRelation("au:aufx/DLSF/TLDN", "", "vst3:ABCDEF019182FAEB544C444E444C5346", "") == 95, "AU <-> JUCE VST3 by codes");
  expect(idRelation("au:aufx/DLSF/TLDN", "", "vst3:ABCDEF019182FAEB544C444E444C5347", "") == 0, "other plugin code: no relation");
  expect(idRelation("vst2:444C5346", "", "au:aufx/DLSF/TLDN", "") == 60, "VST2 id = AU plugin code: a hint only");
  expect(idRelation("clap:com.x.fat", "", "vst3:" + u, "") == 0, "CLAP ids carry nothing");
  // Vendor and plugin codes at the end of the class id, bytes reversed (as a VST3 / AU pair on this Mac has it).
  expect(idRelation("au:aumf/@s13/Aubn", "", "vst3:0B20BA920CE0B1456E62754133317340", "") == 90, "reversed codes at the end of the VST3 id");
  // The VST2-compatible id of a real VST2 / VST3 pair (id and the first nine letters of the name).
  expect(vst2CompatibleVst3Uid(0x4D59504Cu, "Sausage Fattener") == "5653544D59504C736175736167652066", "VST2-compatible id of a real pair");
}

void matcher() {
  using namespace ident;
  Identity saved;
  saved.type = "au:aufx/FATT/Acme";
  saved.name = "Fat Thing";
  saved.vendor = "Acme Audio";
  saved.instrument = 0;
  saved.outputs = 2;
  saved.paramNames = kFatParams;
  std::vector<std::shared_ptr<const HostedClass>> cls = {
      local("vst2:46415454", "Fat Thing", "Acme Audio", false, kFatParams),
      local("vst3:11112222333344445555666677778888", "Fat Thing (VST3)", "Acme Audio Ltd.", false, kFatParams),
      local("clap:com.other.fatthing", "Fat Thing", "Other Co", false, kFatParams),      // another company
      local("vst3:99990000999900009999000099990000", "Fat Thing", "Acme Audio", true, kFatParams),  // an instrument
      local("clap:com.acme.unrelated", "Reverb", "Acme Audio", false, {"Size"}),
  };
  const std::vector<std::string> order = {"vst3", "clap", "au", "lv2", "vst2"};
  auto r = rank(saved, cls, order);
  expect(r.size() == 2, "only the same plugin in other formats is a candidate");
  expect(!r.empty() && r[0].cls->format == "vst3" && r[0].confidence == Confidence::High, "VST3 first (format order), high: name, vendor, parameters");
  expect(r.size() == 2 && r[1].cls->format == "vst2", "VST2 second");
  expect(autoPick(r) == &r[0], "a confident match loads on its own");
  auto r2 = rank(saved, cls, {"vst2", "vst3"});
  expect(!r2.empty() && r2[0].cls->format == "vst2", "the preferred format wins between equal matches");
  {
    Identity tagged = saved;
    tagged.name = "Fat Thing (VST3)";
    auto r3 = rank(tagged, cls, {"vst2", "vst3"});
    expect(!r3.empty() && r3[0].cls->format == "vst3", "a saved name with a format tag goes to that build first");
  }
  // A different parameter set is not the same plugin.
  auto other = local("vst3:AAAABBBBCCCCDDDDEEEEFFFF00001111", "Fat Thing", "Acme Audio", false, {"Room", "Decay", "Tone", "Wet", "Dry"});
  auto r3 = rank(saved, {other}, order);
  expect(r3.empty() || r3[0].confidence <= Confidence::Low, "parameters that differ: never more than low");
  expect(autoPick(r3) == nullptr, "and never loaded on its own");
  // Name only (no vendor known): low, offered, not loaded.
  Identity bare;
  bare.type = "au:aufx/FATT/Acme";
  bare.name = "Fat Thing";
  auto r4 = rank(bare, {local("vst3:12121212121212121212121212121212", "Fat Thing", "", false, kFatParams)}, order);
  expect(r4.size() == 1 && r4[0].confidence == Confidence::Low && autoPick(r4) == nullptr, "name alone: low, a prompt");
  // Two products just as good: no guess.
  Identity noVendor = saved;
  noVendor.vendor.clear();
  noVendor.paramNames.clear();
  auto r5 = rank(noVendor, {local("vst3:12121212121212121212121212121213", "Fat Thing", "Acme", false, {}),
                            local("vst2:12345678", "Fat Thing", "Zeta", false, {})}, order);
  expect(autoPick(r5) == nullptr, "two vendors, nothing to tell them apart: no guess");
  // Ids alone (old projects keep only the type): the JUCE codes of the AU find its VST3.
  Identity onlyType;
  onlyType.type = "au:aufx/DLSF/TLDN";
  auto r6 = rank(onlyType, {local("vst3:ABCDEF019182FAEB544C444E444C5346", "Sausage Thing", "Tailored", false, kFatParams)}, order);
  expect(r6.size() == 1 && r6[0].confidence == Confidence::High, "AU type alone finds the JUCE VST3 by its codes");
  // Aliases recorded where the project was saved.
  Identity withAlias = bare;
  withAlias.aliases = {"vst3:12121212121212121212121212121212"};
  auto r7 = rank(withAlias, {local("vst3:12121212121212121212121212121212", "Fat Thing", "", false, kFatParams)}, order);
  expect(r7.size() == 1 && r7[0].confidence >= Confidence::High, "a saved alias is a sure match");
  // A mono build: different outputs lower the match.
  auto mono = local("vst3:34343434343434343434343434343434", "Fat Thing", "Acme Audio", false, {}, 1);
  Identity np = saved;
  np.paramNames.clear();
  auto r8 = rank(np, {mono}, order);
  expect(r8.size() == 1 && r8[0].confidence == Confidence::Medium, "different outputs: medium at most");
  // Vendor written in front of the name (an Audio Unit's "Vendor: Name" split, VST "Vendor Name").
  auto prefixed = local("vst3:56565656565656565656565656565656", "Acme Fat Thing", "Acme", false, kFatParams);
  auto r9 = rank(saved, {prefixed}, order);
  expect(r9.size() == 1 && r9[0].confidence >= Confidence::Medium, "vendor prefix in the name is ignored");
}

void params() {
  using namespace ident;
  auto c = local("vst3:78787878787878787878787878787878", "X", "Y", false, {"Mix", "Gain", "Mod Rate", "Mod Rate", "Extra"});
  auto m = mapParams({"gain", "MIX", "Mod rate", "Mod-Rate", "Missing"}, 5, *c);
  expect(m.matched == 4 && m.target[0] == 1 && m.target[1] == 0 && m.target[2] == 2 && m.target[3] == 3 && m.target[4] == -1,
         "parameters by name: case, punctuation, repeated names in order");
  auto cut = local("vst2:11223344", "X", "Y", false, {"Intensit", "Mix"});
  auto mc = mapParams({"Intensity", "Mix"}, 2, *cut);
  expect(mc.matched == 2 && mc.target[0] == 0, "a name cut short by the format still matches");
  auto p = mapParams({}, 5, *c);
  expect(p.byPosition && p.matched == 5 && p.target[4] == 4, "no names saved, same count: by position");
  expect(mapParams({}, 4, *c).matched == 0, "no names, different count: nothing");
}

std::string b64(const std::string& s) { return base64Encode(s); }
std::string unb64(const std::string& s) {
  std::string o;
  base64Decode(s, o);
  return o;
}

void states() {
  using namespace xstate;
  const std::string core = std::string("VC2!") + std::string("\x10\0\0\0", 4) + "<STATE fat=\"0.7\"/>";
  // JUCE AU class info -> core.
  const std::string plistXml = auClassInfoXml(0x61756678u, 0x46415454u, 0x41636D65u, "Fat Thing", core);
  std::string back;
  expect(plist::topValue(plistXml, "jucePluginState", back) && back == core, "AU class info carries the JUCE state");
  Core c;
  expect(unwrap("au1:" + b64(plistXml), c) && c.bytes == core && c.framework == "juce", "unwrap AU (JUCE)");
  // -> VST3 (plain uid): the component stream is the state.
  std::string how;
  std::string v3 = rewrap(c, "vst3:11112222333344445555666677778888", "Fat Thing", &how);
  expect(v3.compare(0, 7, "vst3s1:") == 0, "rewrap to VST3");
  Core c2;
  expect(unwrap(v3, c2) && c2.bytes == core, "VST3 round trip keeps the bytes");
  // -> VST3 with a VST2-compatible uid: the fxb bank JUCE writes there.
  const std::string compat = "vst3:" + ident::vst2CompatibleVst3Uid(0x46415454u, "Fat Thing");
  std::string v3b = rewrap(c, compat, "Fat Thing");
  std::string comp;
  {
    const size_t colon = v3b.find(':', 7);
    comp = unb64(v3b.substr(7, colon - 7));
  }
  expect(comp.compare(0, 4, "VstW") == 0 && comp.compare(16, 4, "CcnK") == 0 && comp.compare(24, 4, "FBCh") == 0, "VST2-compatible: VstW + fxb bank");
  std::string inner;
  uint32_t fxId = 0;
  expect(fxChunkPayload(comp, inner, &fxId) && inner == core && fxId == 0x46415454u, "fxb bank chunk read back with its id");
  Core c3;
  expect(unwrap(v3b, c3) && c3.bytes == core && c3.framework == "juce", "unwrap the VST2-compatible state");
  // JUCE VST3 private trailer.
  std::string priv = "PRIVATE-TREE";
  std::string withTrailer = core + priv;
  uint64_t n = priv.size();
  for (int i = 0; i < 8; ++i) withTrailer += static_cast<char>((n >> (8 * i)) & 0xFF);
  withTrailer += "JUCEPrivateData";
  std::string stripped;
  expect(stripJucePrivateData(withTrailer, stripped) && stripped == core, "JUCE private block stripped");
  Core c4;
  expect(unwrap("vst3s1:" + b64(withTrailer) + ":" + b64("ctrl"), c4) && c4.bytes == core && c4.framework == "juce", "unwrap VST3 (JUCE trailer)");
  expect(!stripJucePrivateData("short", stripped), "no trailer: false");
  // -> AU: a class info the unit accepts (type / subtype / manufacturer of the target, version 0).
  std::string au = rewrap(c4, "au:aufx/FATT/Acme", "Fat Thing");
  const std::string auBytes = unb64(au.substr(4));
  std::string nameBack;
  expect(au.compare(0, 4, "au1:") == 0 && plist::topValue(auBytes, "name", nameBack) && nameBack == "Fat Thing", "rewrap to AU");
  Core c5;
  expect(unwrap(au, c5) && c5.bytes == core, "AU round trip keeps the bytes");
  // VST2 chunk <-> CLAP.
  Core c6;
  expect(unwrap("vst2c1:" + b64("raw-chunk"), c6) && c6.bytes == "raw-chunk" && c6.framework.empty(), "unwrap VST2 chunk");
  std::string clap = rewrap(c6, "clap:com.acme.fat", "Fat Thing");
  Core c7;
  expect(clap == "clap1:" + b64("raw-chunk") && unwrap(clap, c7) && c7.bytes == "raw-chunk", "VST2 chunk -> CLAP -> back");
  expect(rewrap(c6, "vst2:46415454", "Fat Thing") == "vst2c1:" + b64("raw-chunk"), "CLAP -> VST2 chunk");
  // A VST2 chunk into a VST2-compatible VST3 goes into the bank.
  std::string v3c = rewrap(c6, compat, "Fat Thing");
  Core c8;
  expect(unwrap(v3c, c8) && c8.bytes == "raw-chunk", "VST2 chunk -> VST2-compatible VST3 bank -> back");
  // VST2 parameter lists only go to VST2; LV2 takes nothing.
  std::string pl(4, '\0');
  float f = 0.25f;
  pl.append(reinterpret_cast<const char*>(&f), 4);
  Core c9;
  expect(unwrap("vst2p1:" + b64(pl), c9) && c9.paramList && c9.values.size() == 1 && c9.values[0] == 0.25f, "VST2 parameter list");
  expect(rewrap(c9, "vst3:11112222333344445555666677778888", "x").empty(), "a parameter list is no state for VST3");
  expect(rewrap(c6, "lv2:urn:x", "x").empty(), "LV2 takes no foreign state");
  // A VST2-based Audio Unit keeps its VST2 program (an fxp) under "vstdata".
  const std::string prog = fxProgramChunk(0x4D59504Cu, 1, 3, "No name", "vst2-chunk");
  std::string pc;
  uint32_t pid = 0;
  int pn = 0;
  expect(fxChunkPayload(prog, pc, &pid, &pn) && pc == "vst2-chunk" && pid == 0x4D59504Cu && pn == 3, "fxp program chunk round trip");
  Core cv;
  expect(unwrap("au1:" + b64(auVstClassInfoXml(0x61756678u, 0x444C5346u, 0x544C444Eu, prog)), cv) && cv.bytes == "vst2-chunk" && cv.vst2Id == 0x4D59504Cu,
         "unwrap a VST2-based AU (vstdata)");
  const std::string v2from = rewrap(cv, "vst2:4D59504C", "x");
  expect(v2from == "vst2c1:" + b64("vst2-chunk"), "VST2-based AU -> VST2 chunk");
  const std::string sfCompat = "vst3:" + ident::vst2CompatibleVst3Uid(0x4D59504Cu, "Sausage Fattener");
  Core cv3;
  expect(unwrap(rewrap(cv, sfCompat, "x"), cv3) && cv3.bytes == "vst2-chunk" && cv3.vst2Id == 0x4D59504Cu, "VST2-based AU -> VST2-compatible VST3 bank");
  const std::string backAu = rewrap(cv3, "au:aufx/DLSF/TLDN", "x", nullptr, 3);
  std::string vd;
  expect(plist::topValue(unb64(backAu.substr(4)), "vstdata", vd) && fxChunkPayload(vd, pc, &pid) && pc == "vst2-chunk" && pid == 0x4D59504Cu,
         "VST3 bank -> VST2-based AU (vstdata)");
  // An AU class info without a portable state does not move.
  const std::string plain = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>data</key><data>AAAA</data></dict></plist>";
  Core c10;
  expect(!unwrap("au1:" + b64(plain), c10), "AU without a portable state: nothing to move");
  expect(!unwrap("garbage", c10) && !unwrap("vst3s1:%%%", c10), "bad data: false");
}

Song songWith(const PluginState& plug, uint32_t autoIndex) {
  Song s;
  ensureMixer(s);
  Channel c;
  c.id = s.nextId++;
  c.name = "Fat";
  c.plugin = plug;
  s.channels.push_back(c);
  AutomationClip a;
  a.id = s.nextId++;
  a.owner = c.id;
  a.index = kChannelPluginBase + autoIndex;
  AutoPoint p;
  p.value = 0.3f;
  a.points = {p};
  s.automations.push_back(a);
  return s;
}

void project() {
  // Saved on a computer with the AU (and its VST3): the identity keeps both.
  const std::string kAu = "au:aumu/FATI/Acme";
  const std::string kV3 = "vst3:ABCDEF019182FAEB41636D6546415449";  // JUCE uid of the same codes
  const std::string kV2 = "vst2:46415449";
  auto au = fake(kAu, "Fat Instrument", "Acme Audio", true, {"Mix", "Gain", "Color", "Drive"});
  auto v3 = fake(kV3, "Fat Instrument", "Acme Audio", true, {"Drive", "Color", "Gain", "Mix", "Bypass Fx"});
  expect(au && v3, "fake classes registered");
  const std::string core = "VC2!\x05\0\0\0<S/>";
  PluginState st = defaultPluginState(kAu);
  st.params = {0.1f, 0.2f, 0.3f, 0.4f};
  st.data = "au1:" + base64Encode(xstate::auClassInfoXml(0x61756D75u, 0x46415449u, 0x41636D65u, "Fat Instrument", core));
  Song song = songWith(st, 2);  // automation on "Color"
  const Json saved = songToJson(song);
  const Json* ident = saved.object.at("channels").array[0].object.at("plugin").find("ident");
  expect(ident && ident->text("name") == "Fat Instrument" && ident->text("vendor") == "Acme Audio", "identity saved: name, vendor");
  bool hasAlias = false;
  if (ident && ident->find("aliases"))
    for (const auto& a : ident->find("aliases")->array) hasAlias = hasAlias || a.str == kV3;
  expect(hasAlias, "identity saved: the VST3 of the same plugin as an alias");
  expect(ident && ident->find("paramNames") && ident->find("paramNames")->array.size() == 4, "identity saved: parameter names");
  // Installed: nothing changes.
  Song same;
  expect(songFromJson(saved, same) && same.channels[0].plugin.type == kAu && same.channels[0].plugin.data == st.data, "saved format installed: kept");
  // The other computer has no AU.
  standin::setHiddenPlugins({"format:au"});
  Song opened;
  expect(songFromJson(saved, opened), "opens without the AU");
  const PluginState& o = opened.channels[0].plugin;
  expect(o.type == kV3, "opened as the VST3");
  expect(o.params.size() == 5 && std::fabs(o.params[3] - 0.1f) < 1e-6f && std::fabs(o.params[0] - 0.4f) < 1e-6f && std::fabs(o.params[1] - 0.3f) < 1e-6f,
         "parameters carried by name");
  xstate::Core c;
  expect(xstate::unwrap(o.data, c) && c.bytes == core && o.data.compare(0, 7, "vst3s1:") == 0, "the state moved into the VST3 wrapping");
  xformat::LoadNote n;
  expect(xformat::loadNote(o, n) && n.via == "state" && n.from == kAu && n.to == kV3 && n.verify.size() == 4, "load note: state, four to verify");
  expect(opened.automations.size() == 1 && opened.automations[0].index == kChannelPluginBase + 1, "automation follows Color to its new index");
  const auto lines = xformat::summary(opened);
  expect(lines.size() == 1 && lines[0].find("VST3 version") != std::string::npos && lines[0].find("AU version") != std::string::npos,
         "load report: opened the VST3 version instead of the AU");
  // Saved again there: VST3 with the AU as an alias, no load note.
  const Json again = songToJson(opened);
  const Json& pj = again.object.at("channels").array[0].object.at("plugin");
  bool auAlias = false;
  if (const Json* id2 = pj.find("ident"); id2 && id2->find("aliases"))
    for (const auto& a : id2->find("aliases")->array) auAlias = auAlias || a.str == kAu;
  expect(pj.text("type") == kV3 && auAlias && !pj.find("ident")->find("load"), "re-saved: VST3, the AU kept as an alias, no load note");
  // Back on a computer with only the AU: it opens as the AU again.
  standin::setHiddenPlugins({"format:vst3"});
  Song back;
  expect(songFromJson(again, back) && back.channels[0].plugin.type == kAu, "and back to the AU where only the AU is");
  xstate::Core cb;
  expect(xstate::unwrap(back.channels[0].plugin.data, cb) && cb.bytes == core, "the state survives the round trip");
  expect(back.automations[0].index == kChannelPluginBase + 2, "automation back on Color");
  // Nothing of it installed: stays missing (placeholder), state untouched.
  standin::setHiddenPlugins({"format:au", "format:vst3"});
  Song none;
  expect(songFromJson(saved, none) && none.channels[0].plugin.type == kAu && none.channels[0].plugin.data == st.data, "none installed: kept as saved");
  // Saved there, the record stays as it came.
  const Json noneSaved = songToJson(none);
  const Json* id3 = noneSaved.object.at("channels").array[0].object.at("plugin").find("ident");
  expect(id3 && id3->text("name") == "Fat Instrument" && id3->find("aliases") && !id3->find("load"), "missing plugin keeps its identity when saved");
  standin::setHiddenPlugins({});
  // An old project (no identity record) with a VST2 that is now only a VST3: names unknown, ids decide.
  const std::string kOld2 = "vst2:46415449";
  (void)kV2;
  PluginState old;
  old.type = kOld2;
  old.params = {0.5f};
  old.data = "vst2c1:" + base64Encode("chunk");
  Song os = songWith(old, 0);
  Json oj = songToJson(os);
  oj.object["channels"].array[0].object["plugin"].object.erase("ident");
  Song oo;
  expect(songFromJson(oj, oo), "old project opens");
  xformat::LoadNote on;
  const bool loaded = xformat::loadNote(oo.channels[0].plugin, on);
  expect(oo.channels[0].plugin.type == kOld2 && loaded && on.via == "missing" && !on.suggest.empty(),
         "old project: a VST2 id that is only a hint stays missing, the AU offered");
}

void flImport() {
  const std::string kV3 = "vst3:ABCDEF019182FAEB41636D6546415449";
  flp::WrapperInfo w;
  w.format = flp::WrapperInfo::Au;
  w.instrument = true;
  w.name = "Fat Instrument";
  w.vendor = "Acme Audio";
  w.auKey = "aumu/FATX/Acme";  // not installed here
  w.data = "au1:" + base64Encode(xstate::auClassInfoXml(0x61756D75u, 0x46415458u, 0x41636D65u, "Fat Instrument", "VC2!\x02\0\0\0ab"));
  standin::setHiddenPlugins({"format:au"});
  const flp::WrapperMatch m = flp::matchWrapper(w);
  expect(m.type == kV3 && !m.stateKept && m.movedData.compare(0, 7, "vst3s1:") == 0, "FL import: the AU opens as the VST3 with its state moved");
  w.vendor = "Someone Else";
  expect(flp::matchWrapper(w).type.empty(), "FL import: another vendor's plugin of that name is not it");
  standin::setHiddenPlugins({});
}

void standIns() {
  // A stand-in saved where Serum-like plugin was missing; here its VST3 is installed: the record moves.
  const std::string kAu = "au:aumu/FATI/Acme", kV3 = "vst3:ABCDEF019182FAEB41636D6546415449";
  Json project = Json::makeObject();
  Json ch = Json::makeObject();
  ch.object["id"] = Json::makeNumber(5);
  Json plug = Json::makeObject();
  plug.object["type"] = Json::makeString("gl.plasma");
  Json orig = Json::makeObject();
  orig.object["type"] = Json::makeString(kAu);
  orig.object["params"] = Json::makeArray();
  plug.object["original"] = orig;
  ch.object["plugin"] = plug;
  project.object["channels"] = Json::makeArray();
  project.object["channels"].array.push_back(ch);
  standin::setHiddenPlugins({"format:au"});
  xformat::resolveProjectJson(project);
  const Json& o = project.object["channels"].array[0].object["plugin"].object["original"];
  expect(o.text("type") == kV3, "a stand-in's original moves to the installed format (then restored)");
  standin::setHiddenPlugins({});
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("crossformat");
  ids();
  matcher();
  params();
  states();
  project();
  flImport();
  standIns();
  return finish();
}
