// Gridline - k078-songlinks (upgraded in 0.7.9 for live song-parameter internal links):
// internalLinkApplies (core/Params.h) is checked against the program builder itself: for every
// parameter of the test song (including tempo, swing, master pitch and groove accents), a link
// from a channel macro compiles into Program::links exactly when internalLinkApplies says so.
// A project saved with an internal link on a song setting loads, saves back unchanged, and
// compiles live into Program::links.
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/Song.h"
#include "engine/Program.h"

using namespace gl;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

ModLink macroLink(ObjectId sourceChannel, ParamId target) {
  ModLink m;
  m.sourceOwner = sourceChannel;
  m.sourceSlot = kLayerMacroSlot;
  m.sourceOutput = 0;
  m.owner = target.owner;
  m.index = target.index;
  return m;
}

void predicateTests(const Song& song) {
  for (uint32_t i = 0; i < static_cast<uint32_t>(SongParam::Count); ++i)
    expect(internalLinkApplies(song, {kSongId, i}), "song setting " + paramDisplayName(song, {kSongId, i}) + ": live internal link supported");
  expect(internalLinkApplies(song, songParam(SongParam::Swing)), "swing: live internal link supported");
  expect(internalLinkApplies(song, songParam(SongParam::GrooveAccent)), "groove accents: live internal link supported");
  expect(internalLinkApplies(song, songParam(SongParam::Tempo)), "tempo: live internal link supported");
  expect(internalLinkApplies(song, songParam(SongParam::MasterPitch)), "master pitch: live internal link supported");
  expect(!internalLinkApplies(song, {kSongId, static_cast<uint32_t>(SongParam::Count)}), "an unknown song parameter: no internal link");
  expect(!internalLinkApplies(song, {kNoId, 0}), "no owner: no internal link");
  expect(internalLinkApplies(song, channelParam(song.channels[0].id, ChannelParam::Volume)), "a channel's volume takes an internal link");
  expect(internalLinkApplies(song, mixerParam(song.mixer[1].id, MixerParam::Volume)), "a mixer track's volume takes an internal link");
}

// Every parameter: the builder compiles the link exactly when internalLinkApplies says so.
void engineAgrees(const Song& base) {
  const ObjectId src = base.channels[0].id;
  ProgramBuilder pb;
  int checked = 0, mismatches = 0, refused = 0;
  for (const ParamId& p : listParams(base)) {
    if (p == layerMacroParam(src, 0)) continue;  // the source itself
    Song s = base;
    s.modLinks = {macroLink(src, p)};
    auto prog = pb.build(s);
    const bool compiled = prog && !prog->links.empty();
    const bool says = internalLinkApplies(s, p);
    ++checked;
    if (!says) ++refused;
    if (compiled != says) {
      ++mismatches;
      expect(false, "engine and dialog disagree on " + paramDisplayName(s, p) + (compiled ? " (the engine applies it)" : " (the engine ignores it)"));
    }
  }
  expect(checked > 50, "the test song has plenty of parameters to check (" + std::to_string(checked) + ")");
  expect(mismatches == 0, "the engine applies an internal link exactly where the dialog offers one");
  expect(refused == 0, "every listed parameter including song settings accepts live internal links (" + std::to_string(refused) + " refused)");
}

// A project holding an internal link on swing loads with it, keeps it, and compiles it live.
void oldLinkLoads(const Song& base) {
  Song s = base;
  s.modLinks = {macroLink(s.channels[0].id, songParam(SongParam::Swing))};
  const std::string text = projectToText(s);
  Song back;
  std::string err;
  expect(loadProjectText(text, "", back, &err), "a project with an internal link on swing loads (" + err + ")");
  const ModLink* l = findModLink(back, kSongId, static_cast<uint32_t>(SongParam::Swing));
  expect(l != nullptr, "its swing link is still there, for the dialog to show and edit");
  expect(l && l->sourceOwner == s.channels[0].id && l->sourceSlot == kLayerMacroSlot, "the link keeps its source");
  expect(projectToText(back) == text, "it saves back unchanged");
  ProgramBuilder pb;
  auto prog = pb.build(back);
  expect(prog && prog->links.size() == 1 && prog->swingLinked && prog->links[0].kind == ProgLink::SongSwing,
         "the engine now compiles the swing link live");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k078_songlinks");
  Song song;
  std::string err;
  if (!loadProject("tests/fixtures/test_song.gridline.json", song, &err)) {
    expect(false, "the test song loads: " + err);
    return finish();
  }
  expect(!song.channels.empty() && song.mixer.size() > 1, "the test song has channels and mixer tracks");
  if (song.channels.empty() || song.mixer.size() < 2) return finish();
  predicateTests(song);
  engineAgrees(song);
  oldLinkLoads(song);
  return finish();
}
