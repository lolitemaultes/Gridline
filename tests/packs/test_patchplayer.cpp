// Gridline: the Patch Player instrument. Catalogue entry, state text, the patch chosen by the
// editor, the Patch and Bank parameters and MIDI program change / bank select, the nearest
// patch for one the file lacks, a missing file, one undo step per patch change, reverb and
// chorus sends (silent at zero, the dry sound unchanged), families and patch summaries.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/Document.h"
#include "core/PluginParams.h"
#include "core/ProjectInterop.h"
#include "patchplayer_rig.h"

using namespace gl;
using namespace gl::packs::samplers;
using namespace pptest;
using namespace sf2t;

namespace {

// Three melodic patches (programs 0, 1, 40) and a drum kit, each a DC level of its own so the
// output tells which one plays.
std::string makeFont() {
  Bank b;
  b.name = "Levels";
  for (int i = 0; i < 4; ++i) b.samples.push_back(sf2t::Sample{"L" + std::to_string(i), dc(4410, static_cast<int16_t>(4096 * (i + 1))), {}, {}, 44100, 60, 0, 0, 4410, 1, 0});
  for (int i = 0; i < 4; ++i)
    b.insts.push_back(Inst{"I" + std::to_string(i), {sf2t::Zone{}.g(AttackVolEnv, -12000).g(ReleaseVolEnv, -12000).g(SampleModes, 1).g(SampleId, i)}});
  b.presets = {Preset{"One", 0, 0, {sf2t::Zone{}.g(Instrument, 0)}}, Preset{"Two", 1, 0, {sf2t::Zone{}.g(Reverb, 250).g(Chorus, 100).g(Instrument, 1)}},
               Preset{"Strings", 40, 0, {sf2t::Zone{}.g(Instrument, 2)}}, Preset{"Kit", 0, 128, {sf2t::Zone{}.g(Instrument, 3)}}};
  return writeBank(b, "levels.sf2");
}

float levelOf(Rig& r, int key = 60) {
  r.gen->noteOn(99, key, 1.f, 0);
  std::vector<float> L, R;
  r.run(L, R, 2000);
  r.gen->noteOff(99, 0);
  r.run(L, R, 500);
  return L[1500];
}

// The bank the app tests open (gridline_test_patchplayer --write <path>): named patches across the
// families and two drum kits, each a few key zones of a looped tone, with the patch's own reverb.
void writeShowFont(const std::string& path) {
  Bank b;
  b.name = "Gridline test bank";
  b.samples.push_back(sf2t::Sample{"Tone low", sine(4410, 40, 12000), {}, {}, 44100, 48, 0, 441, 4410, 1, 0});
  b.samples.push_back(sf2t::Sample{"Tone mid", sine(4410, 60, 12000), {}, {}, 44100, 60, 0, 441, 4410, 1, 0});
  b.samples.push_back(sf2t::Sample{"Tone high", sine(4410, 80, 12000), {}, {}, 44100, 72, 0, 441, 4410, 1, 0});
  Inst keys{"Keys", {sf2t::Zone{}.g(ReleaseVolEnv, -1200).g(SampleModes, 1),
                     sf2t::Zone{}.range(KeyRange, 0, 52).g(SampleId, 0), sf2t::Zone{}.range(KeyRange, 53, 66).g(SampleId, 1),
                     sf2t::Zone{}.range(KeyRange, 67, 127).g(SampleId, 2)}};
  Inst kit{"Kit", {sf2t::Zone{}.range(KeyRange, 35, 36).g(SampleId, 0), sf2t::Zone{}.range(KeyRange, 38, 40).g(SampleId, 1),
                   sf2t::Zone{}.range(KeyRange, 42, 46).g(SampleId, 2).g(Exclusive, 1)}};
  b.insts = {keys, kit};
  struct P {
    const char* name;
    int prog, bank, inst, reverb;
  };
  const P ps[] = {{"Grand Piano", 0, 0, 0, 200},  {"Bright Piano", 1, 0, 0, 200},  {"Celesta", 8, 0, 0, 300},      {"Drawbar Organ", 16, 0, 0, 150},
                  {"Nylon Guitar", 24, 0, 0, 120}, {"Finger Bass", 33, 0, 0, 0},     {"Warm Strings", 48, 0, 0, 350}, {"Slow Strings", 49, 0, 0, 400},
                  {"Choir Aahs", 52, 0, 0, 400},   {"Trumpet", 56, 0, 0, 200},       {"Alto Sax", 65, 0, 0, 150},     {"Flute", 73, 0, 0, 250},
                  {"Square Lead", 80, 0, 0, 100},  {"Halo Pad", 94, 0, 0, 500},      {"Rain", 96, 0, 0, 500},         {"Steel Drums", 114, 0, 0, 150},
                  {"Grand Piano 2", 0, 1, 0, 200}, {"Standard Kit", 0, 128, 1, 120}, {"Brush Kit", 40, 128, 1, 200}};
  for (const P& p : ps) b.presets.push_back(Preset{p.name, static_cast<uint16_t>(p.prog), static_cast<uint16_t>(p.bank), {sf2t::Zone{}.g(Reverb, p.reverb).g(Chorus, 100).g(Instrument, p.inst)}});
  writeFile(path, b.build());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[1]) == "--write") {
    writeShowFont(argv[2]);
    return 0;
  }
  gl::test::hermeticTestEnv("patchplayer");
  const PluginInfo* info = findPlugin(kPatchPlayerType);
  expect(info && info->params.size() == PpCount && info->kind == PluginKind::Generator && info->usesSample, "patch player in the catalogue");
  expect(info && info->name == "Patch Player" && info->presets.size() >= 2 && info->presets.size() <= 6 && info->editorWidth > 0,
         "name, presets, editor width");
  expect(info && info->params[PpPatch].choices.size() == 129 && info->params[PpBank].choices.size() == 130, "patch and bank are stepped choices");
  {
    PluginState s;
    s.type = kPatchPlayerType;
    s.params = {0.f};
    normalizePluginState(s);
    expect(s.params.size() == PpCount && std::fabs(s.params[PpBendRange] - 2.f) < 1e-6f, "normalize pads a short state");
  }
  // ------------------------------------------------------------ state text
  {
    PatchRef r{"/a b/font.sf2", 128, 5, "Big Kit"};
    PatchRef p = parsePatchRef(serializePatchRef(r));
    expect(p.path == r.path && p.bank == 128 && p.program == 5 && p.name == "Big Kit", "state round trip");
    expect(parsePatchRef("/only/a/path.sf2").path == "/only/a/path.sf2" && parsePatchRef("/x.sf2").program == 0, "bare path state");
    PluginState st = defaultPluginState(kPatchPlayerType);
    st.data = serializePatchRef(r);
    std::vector<std::string> refs;
    forEachStateFileRef(st, [&](const std::string& path) { refs.push_back(path); });
    expect(refs.size() == 1 && refs[0] == r.path, "the file is a project file reference (found, relinked, shared)");
  }
  const std::string font = makeFont();
  // ------------------------------------------------------------ patch choice
  {
    Rig r(font, 0, 1);
    expect(std::fabs(r.st.params[PpReverb] - 0.25f) < 1e-6f && std::fabs(r.st.params[PpChorus] - 0.1f) < 1e-6f, "sends start at the patch's own amounts");
    expect(std::fabs(levelOf(r) - 0.25f) < 1e-3f, "the chosen patch plays");
    r.set(PpPatch, 41);  // program 40
    expect(std::fabs(levelOf(r) - 0.375f) < 1e-3f, "the Patch parameter chooses a program");
    r.set(PpPatch, 0);
    expect(std::fabs(levelOf(r) - 0.25f) < 1e-3f, "Patch back on Chosen plays the saved patch");
    // MIDI program change: the next note, not the held one.
    r.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 1000);
    r.gen->midi(0xC0, 40, 0, 0);
    r.gen->noteOn(2, 64, 1.f, 0);
    r.run(L, R, 1000);
    expect(std::fabs(L[1500] - (0.25f + 0.375f)) < 1e-3f, "program change: the held note keeps its patch, the next note takes the new one");
    r.gen->noteOff(1, 0), r.gen->noteOff(2, 0);
    r.run(L, R, 500);
    r.gen->midi(0xB0, 0, 127, 0);  // bank select: the drum bank
    r.gen->midi(0xC0, 0, 0, 0);
    expect(std::fabs(levelOf(r) - 0.5f) < 1e-3f, "bank select and program change reach the drum kit");
    r.gen->midi(0xB0, 0, 0, 0);
    r.gen->midi(0xC0, 33, 0, 0);  // the file has no program 33: the nearest in its family (none), then nearest at all
    const float lv = levelOf(r);
    expect(lv > 0.1f, "a program the file lacks plays the nearest one");
    Rig k(font, 128, 9);  // a kit the file lacks: kit 0
    expect(std::fabs(levelOf(k) - 0.5f) < 1e-3f, "a missing drum kit plays kit 0");
  }
  // ------------------------------------------------------------ missing file
  {
    Rig r(tmpPath("not there.sf2"), 0, 3);
    r.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 2000);
    expect(peak(L) == 0.f && finite(L), "a missing file is silent, no crash");
    PluginState st = defaultPluginState(kPatchPlayerType);
    setPatch(st, tmpPath("not there.sf2"), 0, 3, "Remembered");
    expect(parsePatchRef(st.data).name == "Remembered", "the patch name stays for a missing file");
  }
  // ------------------------------------------------------------ sends
  {
    Rig dry(font, 0, 1), wet(font, 0, 1);
    dry.set(PpReverb, 0), dry.set(PpChorus, 0);
    wet.set(PpReverb, 0.5f), wet.set(PpChorus, 0.25f);
    std::vector<float> Ld, Rd, Lw, Rw, rev0, cho0, rev1, cho1;
    dry.gen->noteOn(1, 60, 1.f, 0);
    wet.gen->noteOn(1, 60, 1.f, 0);
    dry.run(Ld, Rd, 3000, 256, &rev0, &cho0);
    wet.run(Lw, Rw, 3000, 256, &rev1, &cho1);
    expect(dry.gen->auxOutputCount() == 2, "two extra outputs: reverb and chorus sends");
    expect(peak(rev0) == 0.f && peak(cho0) == 0.f, "sends at zero are silent");
    double same = 0;
    for (size_t i = 0; i < Ld.size(); ++i) same = std::max(same, static_cast<double>(std::fabs(Ld[i] - Lw[i])));
    expect(same == 0, "the dry sound does not change with the sends");
    expect(std::fabs(rev1[2000] - 0.5f * Lw[2000]) < 1e-5f && std::fabs(cho1[2000] - 0.25f * Lw[2000]) < 1e-5f, "sends carry the voice at their levels");
    wet.gen->midi(0xB0, 91, 127, 0);  // the reverb controller adds 20 %
    std::vector<float> L2, R2, rev2;
    wet.run(L2, R2, 512, 256, &rev2);
    expect(std::fabs(rev2[400] - 0.7f * L2[400]) < 1e-3f, "reverb controller adds to the send");
  }
  // ------------------------------------------------------------ one undo step per patch change
  {
    Document doc;
    doc.editSong("mixer", [](Song& s) { ensureMixer(s, 4); });  // as every song in the app has
    ObjectId id = doc.addChannel("Strings", kPatchPlayerType);
    doc.editSong("choose patch", [&](Song& s) {
      if (Channel* c = s.channel(id)) setPatch(c->plugin, font, 0, 40, "Strings");
    });
    const Channel* c = doc.song().channel(id);
    expect(c && parsePatchRef(c->plugin.data).program == 40, "patch chosen");
    doc.editSong("choose patch", [&](Song& s) {
      if (Channel* ch = s.channel(id)) setPatch(ch->plugin, font, 0, 1, "Two");
    });
    doc.undo();
    c = doc.song().channel(id);
    expect(c && parsePatchRef(c->plugin.data).program == 40, "undo takes back one patch change");
    // Sends to mixer tracks with Gridline's reverb and chorus, as one step.
    const size_t tracks = doc.song().mixer.size();
    doc.editSong("send reverb and chorus to mixer tracks", [&](Song& s) { routePatchSends(s, id); });
    auto [rv, ch] = patchSendTracks(doc.song(), id);
    const MixerTrack* rt = doc.song().mixerTrack(rv);
    const MixerTrack* ct = doc.song().mixerTrack(ch);
    expect(rt && ct && rv != ch && rt->slots[0].plugin.type == "gl.reverb" && ct->slots[0].plugin.type == "gl.chorus",
           "sends routed to a reverb track and a chorus track");
    expect(rt && rt->name == "Strings reverb" && rt->slots[0].plugin.params[pp::RvbDry] == 0.f && rt->slots[0].plugin.params[pp::RvbWet] == 1.f,
           "the reverb track is named and fully wet");
    doc.editSong("send again", [&](Song& s) { routePatchSends(s, id); });
    auto [rv2, ch2] = patchSendTracks(doc.song(), id);
    expect(rv2 != rv && ch2 != ch && doc.song().mixer.size() >= tracks, "routing again takes other unused tracks (the first ones are in use)");
    doc.undo();
    doc.undo();
    expect(patchSendTracks(doc.song(), id).first == kNoId && doc.song().mixer.size() == tracks, "undo takes the routing back");
    Song plain;
    expect(!routePatchSends(plain, 12345), "routing needs a Patch Player channel");
  }
  // ------------------------------------------------------------ families and summaries
  {
    expect(std::string(patchFamily(0, 0)) == "Piano" && std::string(patchFamily(0, 48)) == "Ensemble" && std::string(patchFamily(128, 0)) == "Drum kits",
           "families");
    auto f = loadSf2(font);
    expect(f != nullptr, "font loads");
    if (f) {
      PatchInfo pi = patchInfo(*f, 1);
      expect(pi.zones == 1 && pi.samples == 1 && pi.loops && std::fabs(pi.reverb - 0.25f) < 1e-6f && pi.memory == 4410 * 2, "patch summary");
    }
  }
  return finish();
}
