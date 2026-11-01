// Gridline: J-213, replacing a channel's instrument.
//  * a channel with an automatic name (empty, the instrument's name, numbered or not, one of its
//    factory preset names, its sample's name) takes the new instrument's name;
//  * a name the user typed stays;
//  * the new instrument starts from its default state (no parameters of the old one or its preset);
//  * one undo step restores both the old instrument (with its state) and the old name; redo again;
//  * replacing with the same instrument keeps the name; a sample's name stays while the new
//    instrument keeps that sample;
//  * effect slots: a replaced effect starts from its default state, undo brings the old one back.
#include <string>
#include "Check.h"
#include "core/Document.h"
#include "core/ExternalDrop.h"
#include "core/PluginInfo.h"

using namespace gl;

namespace {

const std::string& nameOf(const std::string& type) {
  static const std::string none;
  const PluginInfo* p = findPlugin(type);
  return p ? p->name : none;
}

ObjectId addNamed(Document& d, const std::string& type, const std::string& name) {
  const ObjectId id = d.addChannel(name, type);
  return id;
}

// The last factory preset of the channel's instrument, applied as the header loads one.
bool loadLastPreset(Document& d, ObjectId id) {
  const Channel* c = d.song().channel(id);
  const PluginInfo* info = c ? findPlugin(c->plugin.type) : nullptr;
  if (!info || info->presets.empty()) return false;
  const PluginState ps = presetState(*info, info->presets.size() - 1);
  return d.editChannel(id, "load preset", [&](Channel& x) {
    x.plugin = ps;
    x.name = info->presets.back().name;  // a preset dropped on the row names the channel after it
  });
}

}  // namespace

int main() {
  const std::string synth = "gl.synth", fm = "gl.fm", sampler = "gl.sampler";
  expect(findPlugin(synth) && findPlugin(fm) && findPlugin(sampler), "the test instruments exist");

  // ---- numbered names
  expect(xdrop::numberedName("Sampler", "Sampler") && xdrop::numberedName("Sampler #2", "Sampler") &&
             xdrop::numberedName("Sampler #12", "Sampler"),
         "numbered: the name and the name #N");
  expect(!xdrop::numberedName("Sampler #", "Sampler") && !xdrop::numberedName("Sampler #2b", "Sampler") &&
             !xdrop::numberedName("Sampler 2", "Sampler") && !xdrop::numberedName("Samplers", "Sampler") && !xdrop::numberedName("x", ""),
         "numbered: other names are not");

  // ---- the default name follows the instrument; undo restores both in one step
  {
    Document d(makeEmptySong());
    const ObjectId id = addNamed(d, synth, nameOf(synth));
    expect(channelNameIsAutomatic(*d.song().channel(id)), "the instrument's name is automatic");
    const size_t u0 = d.history().position();
    expect(d.setChannelPlugin(id, fm), "replace: done");
    const Channel* c = d.song().channel(id);
    expect(c->plugin.type == fm && c->name == nameOf(fm), "replace: the channel takes the new instrument's name");
    expect(c->plugin == defaultPluginState(fm), "replace: the new instrument starts from its default state");
    expect(d.history().position() == u0 + 1, "replace: one undo step");
    expect(d.undo(), "undo");
    c = d.song().channel(id);
    expect(c->plugin.type == synth && c->name == nameOf(synth), "undo: the old instrument and its name come back together");
    expect(d.redo(), "redo");
    c = d.song().channel(id);
    expect(c->plugin.type == fm && c->name == nameOf(fm), "redo: the new instrument and its name again");
  }
  // ---- numbered (cloned) names, empty names
  {
    Document d(makeEmptySong());
    const ObjectId id = addNamed(d, synth, nameOf(synth) + " #2");
    d.setChannelPlugin(id, fm);
    expect(d.song().channel(id)->name == nameOf(fm), "a numbered instrument name follows the instrument");
    const ObjectId e = addNamed(d, synth, "");
    d.setChannelPlugin(e, fm);
    expect(d.song().channel(e)->name == nameOf(fm), "an empty name takes the instrument's name");
    const ObjectId cl = d.cloneChannel(addNamed(d, synth, nameOf(synth)));
    expect(d.song().channel(cl)->name == nameOf(synth) + " #2", "a clone is numbered");
    d.setChannelPlugin(cl, fm);
    expect(d.song().channel(cl)->name == nameOf(fm), "a replaced clone takes the new instrument's name");
  }
  // ---- a typed name stays (and undo keeps it)
  {
    Document d(makeEmptySong());
    const ObjectId id = addNamed(d, synth, nameOf(synth));
    d.renameChannel(id, "My bass");
    expect(!channelNameIsAutomatic(*d.song().channel(id)), "a typed name is not automatic");
    d.setChannelPlugin(id, fm);
    expect(d.song().channel(id)->plugin.type == fm && d.song().channel(id)->name == "My bass", "replace: a typed name stays");
    d.undo();
    expect(d.song().channel(id)->plugin.type == synth && d.song().channel(id)->name == "My bass", "undo: the typed name is still there");
    const ObjectId n = addNamed(d, synth, nameOf(synth) + " #2 lead");
    d.setChannelPlugin(n, fm);
    expect(d.song().channel(n)->name == nameOf(synth) + " #2 lead", "a name that only starts like the instrument's stays");
  }
  // ---- a factory preset's name follows the instrument; the preset's state goes too
  {
    Document d(makeEmptySong());
    const std::string kick = "gl.kick";  // factory presets of its own
    const ObjectId id = addNamed(d, kick, nameOf(kick));
    if (loadLastPreset(d, id)) {
      const PluginInfo* si = findPlugin(kick);
      expect(d.song().channel(id)->name == si->presets.back().name, "preset: the channel is named after it");
      expect(channelNameIsAutomatic(*d.song().channel(id)), "a factory preset's name is automatic");
      d.setChannelPlugin(id, fm);
      const Channel* c = d.song().channel(id);
      expect(c->name == nameOf(fm), "preset: replacing renames the channel to the new instrument");
      expect(c->plugin == defaultPluginState(fm), "preset: the new instrument starts from its default, not the old preset");
      d.undo();
      c = d.song().channel(id);
      expect(c->plugin == presetState(*si, si->presets.size() - 1) && c->name == si->presets.back().name, "undo: the old preset's state and name come back");
    } else {
      expect(false, "the kick has factory presets");
    }
  }
  // ---- same instrument: the name stays; a sample's name stays while the sample stays
  {
    Document d(makeEmptySong());
    const ObjectId id = addNamed(d, synth, nameOf(synth));
    d.setChannelPlugin(id, synth);
    expect(d.song().channel(id)->name == nameOf(synth), "same instrument: the name stays");
    const ObjectId s = addNamed(d, sampler, "Kick 01");
    d.editChannel(s, "", [](Channel& c) { c.plugin.data = "/packs/Drums/Kick 01.wav"; });
    expect(channelNameIsAutomatic(*d.song().channel(s)), "a sample's name is automatic");
    d.setChannelPlugin(s, fm);
    expect(d.song().channel(s)->name == nameOf(fm), "sample name: the instrument without the sample renames the channel");
    d.undo();
    const PluginInfo* si = findPlugin(synth);
    if (si && si->usesSample) {
      d.setChannelPlugin(s, synth);
      expect(d.song().channel(s)->name == "Kick 01" && d.song().channel(s)->plugin.data == "/packs/Drums/Kick 01.wav",
             "sample name: an instrument that keeps the sample keeps its name");
    }
  }
  // ---- effect slots: the replaced effect starts over, undo brings the old one back
  {
    Document d(makeEmptySong());
    const ObjectId mt = d.song().mixer.size() > 1 ? d.song().mixer[1].id : d.song().masterId();
    expect(d.setSlotPlugin(mt, 0, "gl.eq"), "slot: an effect");
    const PluginInfo* eq = findPlugin("gl.eq");
    if (eq && !eq->presets.empty()) {
      const PluginState ps = presetState(*eq, 0);
      d.editMixerTrack(mt, "load preset", [&](MixerTrack& t) { t.slots[0].plugin = ps; });
    }
    const PluginState before = d.song().mixerTrack(mt)->slots[0].plugin;
    expect(d.setSlotPlugin(mt, 0, "gl.delay"), "slot: replaced");
    expect(d.song().mixerTrack(mt)->slots[0].plugin == defaultPluginState("gl.delay"), "slot: the new effect starts from its default");
    d.undo();
    expect(d.song().mixerTrack(mt)->slots[0].plugin == before, "slot: undo restores the old effect and its state");
  }
  return finish();
}
