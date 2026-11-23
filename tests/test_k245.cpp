// K-245 / K-246: macro knobs for every plugin. Every channel and every mixer track has eight
// macros; any parameter (built-in or installed plugin, strip) links to one with its own range and
// curve; macros are parameters (automation, the automation lists); saved, undone, synced.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/LayerRack.h"
#include "core/Macros.h"
#include "core/Params.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"

using namespace gl;

namespace {

constexpr double kSr = 48000;

struct Rig {
  Song s = makeEmptySong();
  ObjectId lead = kNoId, hosted = kNoId, track = kNoId;
  explicit Rig(bool withHosted = true) {
    PluginInfo hi;  // an installed plugin (only its description: its parameters are link targets like any other)
    hi.type = "vst3:k245-test";
    hi.name = "Test Host Synth";
    hi.kind = PluginKind::Generator;
    hi.params = {ParamDesc{"Cutoff", "", 0, 1, 0.5, ParamKind::Continuous, ParamScale::Linear, {}},
                 ParamDesc{"Drive", "", 0, 1, 0.0, ParamKind::Continuous, ParamScale::Linear, {}}};
    registerDynamicPlugin(hi);
    lead = add("Lead", "gl.synth");
    hosted = add("Host", withHosted ? "vst3:k245-test" : "gl.synth");
    MixerTrack& m = s.mixer[1];
    m.slots[0].plugin = defaultPluginState("gl.filter");
    m.slots[1].plugin = defaultPluginState("gl.delay");
    track = m.id;
    s.channel(lead)->mixerTrack = m.id;
  }
  ObjectId add(const std::string& name, const std::string& type) {
    Channel c;
    c.id = s.allocateId();
    c.name = name;
    c.plugin = defaultPluginState(type);
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    return c.id;
  }
};

// The engine's live values after one block of the song.
std::shared_ptr<const Program> run(const Song& s, AudioEngine& eng, ProgramBuilder& pb, int blocks = 2) {
  eng.setProgram(pb.build(s));
  std::vector<float> l(512), r(512);
  for (int b = 0; b < blocks; ++b) eng.process(l.data(), r.data(), 512);
  return eng.program();
}

bool near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

void testModel() {
  Rig r;
  Document d(r.s);
  const Song& s = d.song();
  // Every channel and mixer track has macros, as parameters and as link sources.
  expect(describeParam(s, layerMacroParam(r.lead, 0)).has_value() && describeParam(s, layerMacroParam(r.track, 7)).has_value(),
         "a plain channel's and a mixer track's macros are parameters");
  expect(!describeParam(s, layerMacroParam(r.lead, 8)).has_value(), "only eight macros");
  expect(isControllerSource(s, r.lead, kLayerMacroSlot) && isControllerSource(s, r.track, kLayerMacroSlot) &&
             !isControllerSource(s, kSongId, kLayerMacroSlot),
         "channels and mixer tracks are macro sources, the song is not");
  expect(macro::ownerFor(s, channelPluginParam(r.lead, 3)) == r.lead && macro::ownerFor(s, slotParam(r.track, 1, 0)) == r.track &&
             macro::ownerFor(s, songParam(SongParam::Tempo)) == kNoId,
         "a parameter's macros are its own wrapper's (the channel, the effect's mixer track)");
  // The default range starts at the control's current value (Osc 1 level, a plain knob), so linking leaves the sound as it was.
  const ParamId cutoff = channelPluginParam(r.lead, pp::SynOsc1Level);
  const auto cd = describeParam(s, cutoff);
  const double before = *getParam(s, cutoff);
  const size_t undoBefore = d.history().position();
  expect(macro::link(d, cutoff, r.lead, 0), "link a built-in parameter to a macro");
  expect(d.history().position() == undoBefore + 1, "linking is one undo step");
  const ModLink* l = findModLink(s, cutoff.owner, cutoff.index);
  expect(l && l->sourceSlot == kLayerMacroSlot && l->sourceOwner == r.lead && l->sourceOutput == 0 && near(l->min, cd->toNormalized(before)) &&
             l->max == 1.0f,
         "the link starts at the control's value and runs to the top");
  expect(macro::firstFree(s, r.lead) == 1, "the next free macro");
  expect(macro::link(d, channelPluginParam(r.hosted, 1), r.hosted, 0) && macro::link(d, channelPluginParam(r.hosted, 0), r.hosted, 0),
         "link an installed plugin's parameters");
  expect(macro::links(s, r.hosted, 0).size() == 2, "one macro drives several controls");
  expect(macro::link(d, slotParam(r.track, 0, 0), r.track, 2) && macro::link(d, slotParam(r.track, 1, 1), r.track, 2), "link effect parameters to the track's macro");
  // Relinking moves the control to the other macro and keeps its range.
  d.editModLink(cutoff, "range", [](ModLink& x) { x.min = 0.1f, x.max = 0.9f, x.curve = ModCurve::Log; });
  expect(macro::link(d, cutoff, r.lead, 4), "relink to another macro");
  l = findModLink(s, cutoff.owner, cutoff.index);
  expect(l && l->sourceOutput == 4 && l->min == 0.1f && l->max == 0.9f && l->curve == ModCurve::Log, "relinking keeps the range and curve");
  expect(macro::links(s, r.lead, 0).empty() && macro::linkedMacro(s, cutoff, nullptr, nullptr), "one macro per control");
  // Loops are refused: a macro driving itself, directly or through another macro.
  expect(!macro::link(d, layerMacroParam(r.lead, 4), r.lead, 4), "a macro cannot drive itself");
  expect(macro::link(d, layerMacroParam(r.hosted, 3), r.lead, 1), "a macro may drive another channel's macro");
  expect(macro::wouldLoop(s, r.hosted, 3, layerMacroParam(r.lead, 1)) && !macro::link(d, layerMacroParam(r.lead, 1), r.hosted, 3),
         "a loop through two macros is refused");
  // Rename, unlink, clear.
  expect(macro::rename(d, r.track, 2, "Space") && macro::name(s, r.track, 2) == "Space" &&
             paramDisplayName(s, layerMacroParam(r.track, 2)).find("Space") != std::string::npos,
         "rename a mixer track's macro");
  expect(macro::unlink(d, channelPluginParam(r.hosted, 1)) && macro::links(s, r.hosted, 0).size() == 1, "unlink one control");
  expect(macro::clear(d, r.hosted, 0) && macro::links(s, r.hosted, 0).empty(), "unlink every control of a macro");
  d.undo();
  expect(macro::links(s, r.hosted, 0).size() == 1, "clearing is one undo step");
}

void testEngine() {
  Rig r;
  Document d(r.s);
  const ParamId cutoff = channelPluginParam(r.lead, pp::SynOsc1Level), hostCut = channelPluginParam(r.hosted, 0);
  const ParamId fxCut = slotParam(r.track, 0, 0), fxMix = slotMixParam(r.track, 1);
  expect(macro::link(d, cutoff, r.lead, 0) && macro::link(d, hostCut, r.hosted, 0) && macro::link(d, fxCut, r.track, 0) &&
             macro::link(d, fxMix, r.track, 0),
         "links on a channel, an installed plugin and a mixer track");
  d.editModLink(cutoff, "range", [](ModLink& x) { x.min = 0.2f, x.max = 0.6f; });
  d.editModLink(fxCut, "range", [](ModLink& x) { x.min = 0.9f, x.max = 0.1f; });  // inverted
  d.editModLink(fxMix, "range", [](ModLink& x) { x.min = 0, x.max = 1, x.curve = ModCurve::Inverted; });
  d.editModLink(hostCut, "range", [](ModLink& x) { x.min = 0, x.max = 1; });
  d.setParam(layerMacroParam(r.lead, 0), 0.5, false);
  d.setParam(layerMacroParam(r.hosted, 0), 1.0, false);
  d.setParam(layerMacroParam(r.track, 0), 0.25, false);
  ProgramBuilder pb;
  AudioEngine eng(kSr);
  auto p = run(d.song(), eng, pb);
  const Song& s = d.song();
  const int li = s.channelIndex(r.lead), hi = s.channelIndex(r.hosted), mi = s.mixerIndex(r.track);
  const ParamDesc sd = *describeParam(s, cutoff), fd = *describeParam(s, fxCut);
  expect(near(p->channels[static_cast<size_t>(li)].live.params[pp::SynOsc1Level], sd.fromNormalized(0.4)), "a channel macro at 50% sets its target to the middle of its range");
  expect(near(p->channels[static_cast<size_t>(hi)].live.params[0], 1.0), "a macro reaches an installed plugin's parameter");
  expect(near(p->mixer[static_cast<size_t>(mi)].slots[0].live.params[0], fd.fromNormalized(0.9 + 0.25 * (0.1 - 0.9))),
         "a mixer track macro reaches an effect, an inverted range moves it the other way");
  expect(near(p->mixer[static_cast<size_t>(mi)].slots[1].mix, 0.75), "each link has its own curve");
  double shown = 0;
  expect(pb.modulatedValue(cutoff, shown) && near(shown, sd.fromNormalized(0.4)), "the knob shows the macro-driven value");
  // A macro driving another channel's macro: the chain follows.
  expect(macro::link(d, layerMacroParam(r.hosted, 0), r.lead, 1), "a macro drives a macro");
  d.editModLink(layerMacroParam(r.hosted, 0), "range", [](ModLink& x) { x.min = 0, x.max = 1; });
  d.setParam(layerMacroParam(r.lead, 1), 0.3, false);
  p = run(d.song(), eng, pb, 4);
  expect(near(p->channels[static_cast<size_t>(hi)].live.params[0], 0.3), "macro to macro to control");
}

void testAutomation() {
  Rig r;
  Document d(r.s);
  const ParamId fxCut = slotParam(r.track, 0, 0);
  expect(macro::link(d, fxCut, r.track, 5), "link an effect to a track macro");
  d.editModLink(fxCut, "range", [](ModLink& x) { x.min = 0, x.max = 1; });
  // K-246: an assigned macro is in the automation lists, an unassigned one is not.
  const std::vector<ParamId> all = listParams(d.song());
  auto has = [&](ParamId p) { return std::find(all.begin(), all.end(), p) != all.end(); };
  expect(has(layerMacroParam(r.track, 5)) && !has(layerMacroParam(r.track, 4)) && !has(layerMacroParam(r.lead, 0)),
         "assigned macros are listed as automatable parameters, unassigned ones are not");
  macro::rename(d, r.lead, 2, "Tone");
  {
    const std::vector<ParamId> a2 = listParams(d.song());
    expect(std::find(a2.begin(), a2.end(), layerMacroParam(r.lead, 2)) != a2.end(), "a named macro is listed too");
    const std::vector<ParamId> as = macro::assignedMacros(d.song());
    expect(as.size() == 2 && as[0] == layerMacroParam(r.lead, 2) && as[1] == layerMacroParam(r.track, 5), "every assigned macro of the song, channels first");
  }
  // An automation clip on the track's macro moves the effect while playing.
  Song s = d.song();
  AutomationClip a;
  a.id = s.allocateId();
  a.owner = r.track;
  a.index = layerMacroParam(r.track, 5).index;
  a.points = {{0, 0.8f, 0, AutoShape::Hold}, {768, 0.8f, 0, AutoShape::Hold}};
  s.automations.push_back(a);
  Clip c;
  c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = a.id, c.start = 0, c.length = 768;
  s.playlist.push_back(c);
  ProgramBuilder pb;
  AudioEngine eng(kSr);
  eng.setMode(PlayMode::Song);
  eng.setProgram(pb.build(s));
  eng.play();
  std::vector<float> l(512), rr(512);
  for (int b = 0; b < 4; ++b) eng.process(l.data(), rr.data(), 512);
  const auto p = eng.program();
  const ParamDesc fd = *describeParam(s, fxCut);
  expect(near(p->mixer[static_cast<size_t>(s.mixerIndex(r.track))].slots[0].live.params[0], fd.fromNormalized(0.8)),
         "automating a track macro moves what it drives");
  eng.stop();
}

void testSaveLoad() {
  Rig r;
  Document d(r.s);
  macro::link(d, channelPluginParam(r.lead, pp::SynOsc1Level), r.lead, 3);
  macro::link(d, slotParam(r.track, 1, 0), r.track, 6);
  macro::rename(d, r.track, 6, "Echo");
  d.setParam(layerMacroParam(r.track, 6), 0.7, false);
  d.setParam(layerMacroParam(r.lead, 3), 0.2, false);
  Song back;
  expect(songFromJson(songToJson(d.song()), back), "save and load");
  expect(back.mixerTrack(r.track) && back.mixerTrack(r.track)->macros[6].name == "Echo" && back.mixerTrack(r.track)->macros[6].value == 0.7f &&
             back.channel(r.lead)->macros[3].value == 0.2f && back.modLinks.size() == 2,
         "macros and their links are saved on channels and mixer tracks");
  // Projects without macros save as before (no macros key).
  Rig plain;
  const Json j = songToJson(plain.s);
  bool any = false;
  for (const char* list : {"channels", "mixer"})
    if (const Json* l = j.find(list); l && l->is(Json::Type::Array))
      for (const Json& e : l->array) any = any || e.find("macros") != nullptr;
  expect(!any, "a project without macros writes no macros key");
  // A mixer track element (the collaboration path) carries its macros.
  MixerTrack m = *d.song().mixerTrack(r.track);
  MixerTrack mb;
  mixerTrackFromJson(mixerTrackToJson(m), mb);
  expect(mb.macros == m.macros, "a mixer track element keeps its macros");
  // Removing the channel removes its macro links.
  d.removeChannel(r.lead);
  expect(d.song().modLinks.size() == 1, "removing a channel removes its macro links");
}

void testLayerStillWorks() {
  Rig r;
  Document d(r.s);
  d.editChannel(r.lead, "layer", [&](Channel& c) { c.layer = {r.hosted}; });
  expect(layer::mapMacro(d, r.lead, 0, channelPluginParam(r.hosted, 0)), "a layer's macro still maps its children");
  expect(!layer::mapMacro(d, r.lead, 0, layerMacroParam(r.lead, 1)), "a layer's macro cannot drive its own macros");
  const auto outs = listControllerOutputs(d.song());
  int layerOuts = 0;
  for (const auto& o : outs) layerOuts += o.slot == kLayerMacroSlot;
  expect(layerOuts == kLayerMacros, "the controller list keeps only the layer's macros (other macros are linked from their own menu)");
}

void testCollab() {
  using namespace sim;
  Clock clock;
  Net net(245);
  Peer a, b;
  a.label = "Alex";
  b.label = "Sam";
  a.id = crypto::Identity::generate();
  b.id = crypto::Identity::generate();
  Rig r(false);  // (an installed plugin's sync is its own feature: the collaboration test keeps to built-ins)
  a.doc.reset(r.s);
  a.folder = tmpDir("k245_a");
  b.folder = tmpDir("k245_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  expect(b.s && b.s->ready(), "the collaborator joins");
  macro::link(a.doc, slotParam(r.track, 0, 0), r.track, 1);
  macro::rename(a.doc, r.track, 1, "Sweep");
  settle(net, {&a, &b}, clock);  // (one person's edits to a track, then the other's: a track is merged as a whole)
  b.doc.setParam(layerMacroParam(r.track, 1), 0.6, false);
  macro::link(b.doc, channelPluginParam(r.lead, pp::SynOsc1Level), r.lead, 0);
  settle(net, {&a, &b}, clock);
  const MixerTrack* ta = a.doc.song().mixerTrack(r.track);
  const MixerTrack* tb = b.doc.song().mixerTrack(r.track);
  expect(ta && tb && tb->macros[1].name == "Sweep" && ta->macros[1].value == 0.6f && a.doc.song().modLinks.size() == 2 &&
             b.doc.song().modLinks == a.doc.song().modLinks,
         "macro links, names and values reach the collaborator both ways");
  expect(songText(a) == songText(b), "both sides end with the same project");
}

}  // namespace

int main() {
  testModel();
  testEngine();
  testAutomation();
  testSaveLoad();
  testLayerStillWorks();
  testCollab();
  return finish();
}
