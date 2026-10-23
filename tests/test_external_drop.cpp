// Gridline — J-75: drags from other applications: item kinds, formats, quick
// audio headers, clip layout, drop planning (position / target → action, hint
// text) and the event queue.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "core/ExternalDrop.h"

using namespace gl;
using browse::DropAction;
using browse::DropItem;
using browse::DropTarget;
using browse::Kind;
namespace fs = std::filesystem;

namespace {

xdrop::Item file(const std::string& name) {
  xdrop::Item it;
  it.path = "/x/" + name;
  it.name = name;
  return it;
}

DropItem kindItem(Kind k, const std::string& type = "", PluginKind pk = PluginKind::Generator) {
  DropItem d;
  d.kind = k;
  d.pluginType = type;
  d.pluginKind = pk;
  return d;
}

std::vector<DropItem> kindsOf(const std::vector<xdrop::Item>& items) {
  std::vector<DropItem> out;
  for (const auto& it : items) out.push_back(kindItem(xdrop::kindOf(it)));
  return out;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

void writeLe32(std::ofstream& o, uint32_t v) {
  const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16),
                              static_cast<unsigned char>(v >> 24)};
  o.write(reinterpret_cast<const char*>(b), 4);
}
void writeLe16(std::ofstream& o, uint16_t v) {
  const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
  o.write(reinterpret_cast<const char*>(b), 2);
}

// 16-bit stereo WAV of `frames` frames at `rate`, with a LIST chunk before "data".
void writeWav(const std::string& path, uint32_t rate, uint32_t frames) {
  std::ofstream o(path, std::ios::binary);
  const uint32_t data = frames * 4;
  o.write("RIFF", 4);
  writeLe32(o, 4 + 8 + 16 + 8 + 4 + 8 + data);
  o.write("WAVE", 4);
  o.write("fmt ", 4);
  writeLe32(o, 16);
  writeLe16(o, 1);
  writeLe16(o, 2);
  writeLe32(o, rate);
  writeLe32(o, rate * 4);
  writeLe16(o, 4);
  writeLe16(o, 16);
  o.write("LIST", 4);
  writeLe32(o, 4);
  o.write("INFO", 4);
  o.write("data", 4);
  writeLe32(o, data);
  std::vector<char> zero(data, 0);
  o.write(zero.data(), static_cast<std::streamsize>(zero.size()));
}

// FLAC stream header with a STREAMINFO block (rate 48000, `total` samples).
void writeFlac(const std::string& path, uint64_t total) {
  std::ofstream o(path, std::ios::binary);
  o.write("fLaC", 4);
  const unsigned char hdr[4] = {0x80, 0, 0, 34};  // last block, STREAMINFO, 34 bytes
  o.write(reinterpret_cast<const char*>(hdr), 4);
  unsigned char si[34] = {};
  const uint32_t rate = 48000;
  si[10] = static_cast<unsigned char>(rate >> 12);
  si[11] = static_cast<unsigned char>(rate >> 4);
  si[12] = static_cast<unsigned char>(((rate & 0xF) << 4) | (1 << 1));  // 2 channels
  si[13] = static_cast<unsigned char>((15 << 4) | ((total >> 32) & 0xF));
  si[14] = static_cast<unsigned char>(total >> 24);
  si[15] = static_cast<unsigned char>(total >> 16);
  si[16] = static_cast<unsigned char>(total >> 8);
  si[17] = static_cast<unsigned char>(total);
  o.write(reinterpret_cast<const char*>(si), 34);
}

}  // namespace

int main() {
  // ---- item kinds ----
  {
    expect(xdrop::kindOf(file("Kick.wav")) == Kind::Audio, "wav item is audio");
    expect(xdrop::kindOf(file("Riff.mid")) == Kind::Midi, "mid item is MIDI");
    expect(xdrop::kindOf(file("song.flp")) == Kind::Flp, "flp item");
    expect(xdrop::kindOf(file("x.gridline.json")) == Kind::Project, "gridline project item");
    expect(xdrop::kindOf(file("notes.txt")) == Kind::Other, "text item is unsupported");
    xdrop::Item d = file("Drums");
    d.dir = true;
    expect(xdrop::kindOf(d) == Kind::Folder, "folder item");
    xdrop::Item p;
    p.name = "Dropped file";
    p.pending = true;
    p.guessed = true;
    expect(xdrop::kindOf(p) == Kind::Audio, "promise of unknown type counts as audio");
    p.guessed = false;
    expect(xdrop::kindOf(p) == Kind::Other, "promise without a guess stays unknown");
    xdrop::Item pm;
    pm.name = "Dropped file.mid";
    pm.pending = true;
    expect(xdrop::kindOf(pm) == Kind::Midi, "promise typed as MIDI");
    expect(xdrop::displayName(file("a b.wav")) == "a b.wav", "display name");
  }

  // ---- formats ----
  {
    expect(xdrop::extensionForUti("com.microsoft.waveform-audio") == ".wav", "uti wav");
    expect(xdrop::extensionForUti("public.mp3") == ".mp3", "uti mp3");
    expect(xdrop::extensionForUti("org.xiph.flac") == ".flac", "uti flac");
    expect(xdrop::extensionForUti("public.midi-audio") == ".mid", "uti midi");
    expect(xdrop::extensionForUti("PUBLIC.AIFF-AUDIO") == ".aiff", "uti case-insensitive");
    expect(xdrop::extensionForUti("com.example.unknown").empty(), "unknown uti");
    expect(xdrop::isWebUrl("https://x.com/a.wav") && xdrop::isWebUrl("HTTP://x") && !xdrop::isWebUrl("file:///a.wav") && !xdrop::isWebUrl("ftp://x"),
           "web links");
    expect(xdrop::pathFromFileUrl("file:///Users/me/My%20Kick.wav") == "/Users/me/My Kick.wav", "file url decoded");
    expect(xdrop::pathFromFileUrl("file://localhost/tmp/a.wav") == "/tmp/a.wav", "file url with host");
    expect(xdrop::pathFromFileUrl("https://x/a.wav").empty(), "web link is not a file url");
    expect(xdrop::fileNameForUrl("https://cdn.x.com/packs/Kick%2001.wav?sig=abc#t") == "Kick 01.wav", "link file name");
    expect(xdrop::fileNameForUrl("https://x.com/") == "Download", "link without a file name");
    bool audioListed = false;
    for (const auto& u : xdrop::audioUtis()) audioListed = audioListed || u == "com.microsoft.waveform-audio";
    expect(audioListed, "raw wav data type registered");
  }

  // ---- quick audio headers, unique paths ----
  const fs::path tmp = fs::temp_directory_path() / "gridline-test-external-drop";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  {
    writeWav((tmp / "a.wav").string(), 44100, 88200);
    const double s = xdrop::audioHeaderSeconds((tmp / "a.wav").string());
    expect(s > 1.999 && s < 2.001, "wav header length (chunk before data skipped)");
    writeFlac((tmp / "b.flac").string(), 48000 * 3);
    const double f = xdrop::audioHeaderSeconds((tmp / "b.flac").string());
    expect(f > 2.999 && f < 3.001, "flac STREAMINFO length");
    std::ofstream((tmp / "c.wav").string()) << "not audio";
    expect(xdrop::audioHeaderSeconds((tmp / "c.wav").string()) < 0, "garbage is unknown");
    expect(xdrop::audioHeaderSeconds((tmp / "missing.wav").string()) < 0, "missing file is unknown");

    const std::string u1 = xdrop::uniquePath((tmp / "Dropped").string(), "Kick.wav");
    expect(fs::path(u1).filename() == "Kick.wav" && fs::is_directory(tmp / "Dropped"), "unique path: first free name, folder made");
    std::ofstream(u1) << "x";
    const std::string u2 = xdrop::uniquePath((tmp / "Dropped").string(), "Kick.wav");
    expect(fs::path(u2).filename() == "Kick (2).wav", "unique path: numbered when taken");
    expect(fs::path(xdrop::uniquePath((tmp / "Dropped").string(), "a/b:c.wav")).filename() == "a_b_c.wav", "unique path: separators replaced");
  }

  // ---- clip layout ----
  {
    auto st = xdrop::layoutClips({100, 200, 300}, 3, 960, false, 500);
    expect(st.size() == 3 && st[0].track == 3 && st[1].track == 4 && st[2].track == 5 && st[2].tick == 960, "stacked on consecutive tracks");
    auto sq = xdrop::layoutClips({100, 200, 300}, 3, 960, true, 500);
    expect(sq[0].tick == 960 && sq[1].tick == 1060 && sq[2].tick == 1260 && sq[2].track == 3, "sequential on one track");
    auto cl = xdrop::layoutClips({1, 1, 1}, 8, 0, false, 10);
    expect(cl[1].track == 9 && cl[2].track == 9, "stack clamps to the last track");
  }

  // ---- planning: position / target → action and hint ----
  const Tick bar = 384;
  {
    std::vector<xdrop::Item> items = {file("Kick.wav"), file("Loop.wav"), file("Pad.wav")};
    xdrop::Context c;
    c.target = DropTarget::Playlist;
    c.track = 3;  // track 4
    c.tick = 8 * bar;  // bar 9
    c.barTicks = bar;
    c.lengths = {96, 1536, 0};
    c.fallbackLength = bar;
    xdrop::Plan p = xdrop::plan(items, kindsOf(items), c, {});
    expect(p.accepted() && p.steps.size() == 3, "playlist takes three samples");
    bool clips = true;
    for (const auto& s : p.steps) clips = clips && s.action == DropAction::AudioClip;
    expect(clips, "samples on the playlist become audio clips");
    expect(p.steps[0].track == 3 && p.steps[1].track == 4 && p.steps[2].track == 5, "stacked from the track under the pointer (FL)");
    expect(p.steps[2].length == bar, "unknown length drawn as one bar");
    expect(p.label == "Drop to add 3 audio clips on tracks 4-6 at bar 9", "hint: stacked clips");
    xdrop::Mods shift;
    shift.shift = true;
    xdrop::Plan q = xdrop::plan(items, kindsOf(items), c, shift);
    expect(q.steps[1].tick == 8 * bar + 96 && q.steps[2].tick == 8 * bar + 96 + 1536 && q.steps[2].track == 3, "Shift: one after another");
    expect(q.label == "Drop to add 3 audio clips one after another on track 4 from bar 9", "hint: sequential clips");
    std::vector<xdrop::Item> one = {file("Kick.wav")};
    c.lengths = {96};
    expect(xdrop::plan(one, kindsOf(one), c, {}).label == "Drop to add an audio clip on track 4 at bar 9", "hint: one clip");
  }
  {
    std::vector<xdrop::Item> items = {file("Kick.wav"), file("Snare.wav")};
    xdrop::Context c;
    c.target = DropTarget::Rack;
    xdrop::Plan p = xdrop::plan(items, kindsOf(items), c, {});
    expect(p.steps.size() == 2 && p.steps[0].action == DropAction::NewSamplerChannel && p.label == "Drop to add 2 sampler channels",
           "rack free area: new sampler channels");
    c.targetType = "gl.sampler";
    c.targetUsesSample = true;
    c.onChannel = true;
    c.targetName = "Kick";
    p = xdrop::plan(items, kindsOf(items), c, {});
    expect(p.steps[0].action == DropAction::ReplaceSample && p.steps[1].action == DropAction::NewSamplerChannel, "channel row: replace, rest new");
    expect(contains(p.label, "replace the sample of Kick") && contains(p.label, "add a sampler channel"), "hint: replace + new");
    xdrop::Mods shift;
    shift.shift = true;
    p = xdrop::plan(items, kindsOf(items), c, shift);
    expect(p.steps[0].action == DropAction::NewSamplerChannel && p.steps[1].action == DropAction::NewSamplerChannel, "Shift on a row: new channels");
    c.targetType = "gl.synth";
    c.targetUsesSample = false;
    c.targetName = "Lead";
    p = xdrop::plan(items, kindsOf(items), c, {});
    expect(p.steps[0].action == DropAction::ReplaceInstrument && p.steps[1].action == DropAction::NewSamplerChannel,
           "J-97 row of a synth: becomes a sampler with the first file, the rest new channels");
    expect(contains(p.label, "replace Lead with a sampler playing Kick.wav"), "J-97 hint: replace with a sampler");
    p = xdrop::plan(items, kindsOf(items), c, shift);
    expect(p.steps[0].action == DropAction::NewSamplerChannel, "J-97 Shift on a synth row: new channels");
  }
  {  // J-97: instrument presets and MIDI on a channel row; inserting between rows
    std::vector<xdrop::Item> items = {file("Bass.fst"), file("Pad.fst")};
    std::vector<DropItem> kinds = {kindItem(Kind::Preset, "gl.synth"), kindItem(Kind::Preset, "gl.synth")};
    xdrop::Context c;
    c.target = DropTarget::Rack;
    c.targetType = "gl.sampler";
    c.targetUsesSample = true;
    c.onChannel = true;
    c.targetName = "Kick";
    xdrop::Plan p = xdrop::plan(items, kinds, c, {});
    expect(p.steps.size() == 2 && p.steps[0].action == DropAction::ReplaceFromPreset && p.steps[1].action == DropAction::NewChannelFromPreset,
           "J-97 row: the first preset replaces the instrument, the rest new channels");
    expect(contains(p.label, "replace Kick with the preset Bass.fst"), "J-97 hint: replace from preset");
    std::vector<xdrop::Item> midi = {file("Riff.mid")};
    p = xdrop::plan(midi, kindsOf(midi), c, {});
    expect(p.steps.size() == 1 && p.steps[0].action == DropAction::ImportMidiNotes && contains(p.label, "into Kick"), "J-97 MIDI on a row: its notes");
    xdrop::Context gap;
    gap.target = DropTarget::Rack;
    gap.insertHere = true;
    std::vector<xdrop::Item> audio = {file("Kick.wav")};
    p = xdrop::plan(audio, kindsOf(audio), gap, {});
    expect(p.steps[0].action == DropAction::NewSamplerChannel && p.label == "Drop to insert a sampler channel here", "J-97 between rows: insert");
    expect(browse::dropAction(kindItem(Kind::Preset, "gl.delay", PluginKind::Effect), DropTarget::Rack, "gl.sampler", true) == DropAction::None,
           "J-97 effect preset on a row: refused");
  }
  {
    std::vector<xdrop::Item> items = {file("Kick.wav"), file("song.flp"), file("Pad.wav")};
    xdrop::Context c;
    c.target = DropTarget::MixerSlot;
    xdrop::Plan p = xdrop::plan(items, kindsOf(items), c, {});
    expect(p.steps.size() == 1 && p.steps[0].action == DropAction::OpenProject && p.steps[0].item == 1 && p.skipped.size() == 2,
           "a project opens wherever it is dropped");
    expect(p.label == "Drop to open the project song.flp (2 other files ignored)", "hint: project");
  }
  {
    xdrop::Item folder = file("Drums");
    folder.dir = true;
    std::vector<xdrop::Item> items = {folder};
    xdrop::Context c;
    c.target = DropTarget::Browser;
    xdrop::Plan p = xdrop::plan(items, kindsOf(items), c, {});
    expect(p.accepted() && p.steps[0].action == DropAction::AddBrowserFolder && p.label == "Drop to add the folder Drums to the browser",
           "browser: folder");
    c.target = DropTarget::Playlist;
    p = xdrop::plan(items, kindsOf(items), c, {});
    expect(!p.accepted() && p.label == "Drums cannot be dropped here", "playlist refuses folders with a reason");
    std::vector<xdrop::Item> audio = {file("Kick.wav")};
    c.target = DropTarget::Browser;
    expect(!xdrop::plan(audio, kindsOf(audio), c, {}).accepted(), "browser: samples are not taken");
  }
  {
    std::vector<xdrop::Item> items = {file("notes.txt"), file("photo.jpg")};
    xdrop::Context c;
    c.target = DropTarget::Playlist;
    xdrop::Plan p = xdrop::plan(items, kindsOf(items), c, {});
    expect(!p.accepted() && p.unsupported.size() == 2 && p.label == "Gridline cannot open 2 files", "unsupported files");
    items.push_back(file("Kick.wav"));
    p = xdrop::plan(items, kindsOf(items), c, {});
    expect(p.accepted() && p.unsupported.size() == 2 && contains(p.label, "(2 files left out)"), "mixed: unsupported files left out");
  }
  {
    std::vector<xdrop::Item> items = {file("Riff.mid")};
    xdrop::Context c;
    c.target = DropTarget::Playlist;
    c.track = 1;
    c.tick = 4 * bar;
    c.barTicks = bar;
    xdrop::Plan p = xdrop::plan(items, kindsOf(items), c, {});
    expect(p.steps[0].action == DropAction::MidiPatternClip && p.steps[0].track == 1 && p.steps[0].tick == 4 * bar, "MIDI on the playlist: pattern clip");
    expect(p.label == "Drop to import Riff.mid as a pattern clip on track 2 at bar 5", "hint: MIDI pattern clip");
    c.target = DropTarget::PianoRoll;
    expect(xdrop::plan(items, kindsOf(items), c, {}).steps[0].action == DropAction::ImportMidiNotes, "MIDI on the piano roll: notes");
    c.target = DropTarget::Rack;
    expect(xdrop::plan(items, kindsOf(items), c, {}).steps[0].action == DropAction::ImportMidiChannels, "MIDI on the rack: pattern");
    std::vector<xdrop::Item> mixed = {file("Riff.mid"), file("Kick.wav")};
    c.target = DropTarget::Playlist;
    c.lengths = {0, 192};
    xdrop::Plan m = xdrop::plan(mixed, kindsOf(mixed), c, {});
    expect(m.steps.size() == 2 && m.steps[0].track == 1 && m.steps[1].track == 2, "MIDI and audio clips stack together");
  }
  {
    std::vector<xdrop::Item> items = {file("Delay.fst"), file("Lead.fst")};
    std::vector<DropItem> kinds = {kindItem(Kind::Preset, "gl.delay", PluginKind::Effect), kindItem(Kind::Preset, "gl.synth", PluginKind::Generator)};
    xdrop::Context c;
    c.target = DropTarget::MixerSlot;
    xdrop::Plan p = xdrop::plan(items, kinds, c, {});
    expect(p.steps.size() == 1 && p.steps[0].action == DropAction::InsertEffectPreset && p.skipped.size() == 1, "mixer: effect preset");
    expect(p.label == "Drop to insert the effect preset Delay.fst (1 file left out)", "hint: effect preset");
    c.target = DropTarget::Rack;
    p = xdrop::plan(items, kinds, c, {});
    expect(p.steps.size() == 1 && p.steps[0].action == DropAction::NewChannelFromPreset, "rack: generator preset");
  }
  {
    std::vector<xdrop::Item> items = {file("A.wav"), file("B.wav")};
    xdrop::Context c;
    c.target = DropTarget::PluginWindow;
    c.targetType = "gl.slicer";
    c.targetUsesSample = true;
    c.targetName = "Slicer";
    xdrop::Plan p = xdrop::plan(items, kindsOf(items), c, {});
    expect(p.steps.size() == 1 && p.steps[0].action == DropAction::LoadSample && p.skipped.size() == 1, "sample plugin: loads the first sample");
    expect(p.label == "Drop to load A.wav into Slicer (1 file left out)", "hint: load sample");
    c.targetUsesSample = false;
    expect(!xdrop::plan(items, kindsOf(items), c, {}).accepted(), "plugin without samples refuses audio");
  }
  {
    xdrop::Item promise;
    promise.name = "Dropped file";
    promise.pending = true;
    promise.guessed = true;
    std::vector<xdrop::Item> items = {promise};
    xdrop::Context c;
    c.target = DropTarget::Playlist;
    expect(xdrop::plan(items, kindsOf(items), c, {}).steps[0].action == DropAction::AudioClip, "promised file of unknown type: audio clip");
  }
  {  // the shared table (browser drags use it too)
    expect(browse::dropAction(kindItem(Kind::Audio), DropTarget::Rack, "gl.sampler", true) == DropAction::ReplaceSample, "table: row of a sampler");
    expect(browse::dropAction(kindItem(Kind::Audio), DropTarget::Rack) == DropAction::NewSamplerChannel, "table: rack area");
    expect(browse::dropAction(kindItem(Kind::Midi), DropTarget::Playlist) == DropAction::MidiPatternClip, "table: MIDI on the playlist");
    expect(browse::dropAction(kindItem(Kind::Folder), DropTarget::Browser) == DropAction::AddBrowserFolder, "table: folder on the browser");
    expect(browse::dropAction(kindItem(Kind::Folder), DropTarget::Workspace) == DropAction::AddBrowserFolder, "table: folder anywhere else");
    expect(std::string(browse::dropActionLabel(DropAction::ReplaceSample)) == "Replace sample", "table label");
  }

  // ---- event queue (completions come from other threads) ----
  {
    xdrop::take();
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
      ts.emplace_back([t] {
        for (int i = 0; i < 250; ++i) {
          xdrop::Event e;
          e.type = xdrop::EventType::Arrived;
          e.index = static_cast<size_t>(t * 1000 + i);
          xdrop::post(e);
        }
      });
    for (auto& t : ts) t.join();
    expect(xdrop::take().size() == 1000 && xdrop::take().empty(), "events from several threads, taken once");
    const uint64_t a = xdrop::newToken(), b = xdrop::newToken();
    expect(b > a, "tokens increase");
    xdrop::setDropFolder("/tmp/x/Dropped");
    expect(xdrop::dropFolder() == "/tmp/x/Dropped", "drop folder");
    xdrop::setCancelled(true);
    xdrop::setHoverAccepted(true);
    expect(xdrop::cancelled() && xdrop::hoverAccepted(), "flags for the platform layer");
    xdrop::setCancelled(false);
    xdrop::setHoverAccepted(false);
  }

  fs::remove_all(tmp);
  return finish();
}
