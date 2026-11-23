// Gridline - checkpoint 60 (N59-01): the hand override of automated controls, engine side.
//   - A held parameter plays its held value (the control's own) instead of its automation, on the
//     live builder's engine: control blocks and K-317's long gain blocks alike; released, the curve
//     plays again.
//   - The hold lives in the builder (session state): the song, its saved text and its render are
//     the same with a hold as without one (renders and exports build their own program).
//   - A hold set before the build that first compiles the lane is taken by that build.
//   - Stopped and at rest (K-142, 0.7.8: links that would change nothing are left out), a hold, a
//     release and a new position still reach the parameter.
#include <cmath>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Automation.h"
#include "core/Params.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

// The master's volume automated from 0.2 (normalized) to 0.8 over two bars, from the song's start.
Song automatedSong(ParamId& pid) {
  Song s;
  std::string err;
  if (!loadProject("tests/fixtures/test_song.gridline.json", s, &err)) {
    return s;
  }
  pid = mixerParam(s.masterId(), MixerParam::Volume);
  AutomationClip a;
  a.id = s.allocateId();
  a.owner = pid.owner;
  a.index = pid.index;
  a.points = {{0, 0.2f}, {768, 0.8f}};
  s.automations.push_back(a);
  Clip c;
  c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = a.id, c.start = 0, c.length = 768, c.track = 0;
  s.playlist.push_back(c);
  return s;
}

void run(AudioEngine& e, int frames) {
  std::vector<float> L(256), R(256);
  for (int k = 0; k < frames / 256; ++k) {
    e.process(L.data(), R.data(), 256);
  }
}

bool sameAudio(const AudioBuffer& a, const AudioBuffer& b) {
  bool same = a.frames == b.frames && a.frames > 0;
  for (size_t i = 0; same && i < a.frames; ++i) {
    same = a.at(0, i) == b.at(0, i) && a.at(1, i) == b.at(1, i);
  }
  return same;
}
}  // namespace

int main() {
  ParamId pid;
  Song s = automatedSong(pid);
  expect(!s.channels.empty() && pid.owner != kNoId, "fixture loads");
  if (s.channels.empty()) {
    return finish();
  }
  const auto d = describeParam(s, pid);
  expect(d.has_value(), "the master's volume is described");
  const std::string savedBefore = projectToText(s);
  RenderOptions ro;
  const AudioBuffer before = renderSong(s, ro);

  // ---- the engine plays the held value; released, the curve again
  {
    ProgramBuilder b;
    AudioEngine e(44100);
    b.setSampleRate(44100);
    e.setThreads(1);
    e.setProgram(b.build(s));
    e.setMode(PlayMode::Song);
    e.play();
    run(e, 22050);  // half a second: a quarter of the ramp (120 BPM, 96 ppq: 0.5 s = 96 ticks)
    double v = 0;
    const double curve = d->fromNormalized(0.2 + 0.6 * (e.position() / 768.0));
    expect(b.modulatedValue(pid, v) && std::fabs(v - curve) < 0.02, "the automation plays (" + std::to_string(v) + ")");
    expect(!b.automationHeld(pid), "not held before a touch");
    b.holdAutomation(pid, 0.9);
    expect(b.automationHeld(pid), "held after holdAutomation");
    run(e, 22050);
    expect(b.modulatedValue(pid, v) && std::fabs(v - d->fromNormalized(0.9)) < 1e-4, "held: the hand value plays (" + std::to_string(v) + ")");
    b.holdAutomation(pid, 0.1);  // the control moved again: its new value
    run(e, 4096);
    expect(b.modulatedValue(pid, v) && std::fabs(v - d->fromNormalized(0.1)) < 1e-4, "held: a later hand value plays");
    // A rebuild (any edit) keeps the hold.
    e.setProgram(b.build(s));
    run(e, 4096);
    expect(b.automationHeld(pid) && b.modulatedValue(pid, v) && std::fabs(v - d->fromNormalized(0.1)) < 1e-4, "a rebuild keeps the hold");
    b.releaseAutomation(pid);
    run(e, 4096);
    const double curve2 = d->fromNormalized(std::min(0.8, 0.2 + 0.6 * (e.position() / 768.0)));
    expect(!b.automationHeld(pid) && b.modulatedValue(pid, v) && std::fabs(v - curve2) < 0.02, "released: the automation plays again");
    b.holdAutomation(pid, 0.5);
    b.releaseAllAutomation();
    expect(!b.automationHeld(pid), "releaseAllAutomation ends every hold");
  }

  // ---- K-142 (0.7.8 close): stopped and at rest (a stopped block whose links would change nothing
  // leaves them out), a hold, a release and a move of the position still reach the parameter
  {
    ProgramBuilder b;
    AudioEngine e(44100);
    b.setSampleRate(44100);
    e.setThreads(1);
    e.setProgram(b.build(s));
    e.setMode(PlayMode::Song);
    run(e, 12 * 44100);  // twelve seconds stopped: every tail is over, the graph sleeps
    double v = 0;
    expect(b.modulatedValue(pid, v) && std::fabs(v - d->fromNormalized(0.2)) < 1e-4, "stopped at the start: the curve's first value");
    b.holdAutomation(pid, 0.9);
    run(e, 4096);
    expect(b.modulatedValue(pid, v) && std::fabs(v - d->fromNormalized(0.9)) < 1e-4, "stopped, at rest: a hold plays at once (" + std::to_string(v) + ")");
    run(e, 5 * 44100);
    b.releaseAutomation(pid);
    run(e, 4096);
    expect(b.modulatedValue(pid, v) && std::fabs(v - d->fromNormalized(0.2)) < 1e-4, "stopped, at rest: released, the curve again (" + std::to_string(v) + ")");
    run(e, 5 * 44100);
    e.setPosition(384);
    run(e, 4096);
    expect(b.modulatedValue(pid, v) && std::fabs(v - d->fromNormalized(0.5)) < 0.02, "stopped, at rest: the position moved, the curve there (" + std::to_string(v) + ")");
  }

  // ---- a hold set before the lane's first build is taken by that build
  {
    ProgramBuilder b;
    AudioEngine e(44100);
    b.setSampleRate(44100);
    e.setThreads(1);
    b.holdAutomation(pid, 0.75);
    e.setProgram(b.build(s));
    e.setMode(PlayMode::Song);
    e.play();
    run(e, 8192);
    double v = 0;
    expect(b.modulatedValue(pid, v) && std::fabs(v - d->fromNormalized(0.75)) < 1e-4, "a hold made before the build holds");
  }

  // ---- the song, its saved text and its render ignore the override
  {
    ProgramBuilder live;
    live.holdAutomation(pid, 0.95);
    (void)live.build(s);
    expect(projectToText(s) == savedBefore, "the override is never saved (the song is unchanged)");
    const AudioBuffer after = renderSong(s, ro);
    expect(sameAudio(before, after), "a render follows the automation, held or not");
  }
  return finish();
}
