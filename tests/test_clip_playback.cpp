// Gridline - K-112b3: a pattern clip's playback settings (core/ClipPlayback.h).
//   - transpose, velocity offset, delay and playback quantise change the played note only;
//   - a clip with none set plays as written and saves nothing new (old projects stay byte-equal);
//   - the settings round-trip through the project file and are clamped when out of range.
#include <cmath>
#include <string>
#include "Check.h"
#include "core/ClipPlayback.h"
#include "core/Json.h"
#include "core/Song.h"
#include "core/Project.h"

using namespace gl;

int main() {
  Clip c;
  c.start = 960;
  {
    Tick on = 1000, off = 1050;
    float onF = 0, offF = 0, pitch = 60, vel = 0.5f;
    clipplay::apply(c, c.start, on, off, onF, offF, pitch, vel);
    expect(!clipplay::active(c) && on == 1000 && off == 1050 && pitch == 60 && vel == 0.5f, "nothing set: the note plays as written");
  }
  c.playTranspose = 5, c.playVelocity = -20, c.playDelay = 12, c.playQuantize = 96;
  {
    Tick on = 1000, off = 1050;
    float onF = 0.25f, offF = 0, pitch = 60, vel = 0.5f;
    clipplay::apply(c, c.start, on, off, onF, offF, pitch, vel);
    // 1000.25 is 40.25 ticks into the clip: snapped to 0 (grid 96), plus the delay of 12.
    expect(on == 960 + 12 && onF == 0.0f, "quantise snaps to the clip's grid, then the delay moves the note");
    expect(std::fabs(static_cast<double>(off - on) + offF - 49.75) < 1e-3, "the note keeps its length");
    expect(pitch == 65, "transpose adds semitones");
    expect(std::fabs(vel - (0.5f - 20.0f / 127.0f)) < 1e-5f, "the velocity offset is in MIDI steps");
  }
  {
    Clip d;
    d.playDelay = -500;
    Tick on = 100, off = 200;
    float onF = 0, offF = 0, pitch = 127, vel = 1.0f;
    d.playTranspose = 12, d.playVelocity = 40;
    clipplay::apply(d, 0, on, off, onF, offF, pitch, vel);
    expect(on == 0 && off == 100, "a negative delay never starts a note before the song");
    expect(pitch == 127 && vel == 1.0f, "pitch and velocity stay in range");
  }
  {
    auto roundTrip = [](const Clip& in, Clip& out, std::string& text) {
      Song s;
      Pattern p;
      p.id = 100;
      p.name = "P";
      s.patterns.push_back(p);
      Clip k = in;
      k.id = 101, k.pattern = 100, k.kind = ClipKind::Pattern, k.track = 0;
      s.playlist.push_back(k);
      s.nextId = 200;
      const Json j = songToJson(s);
      text = jsonCompact(j);
      Song back;
      if (!songFromJson(j, back) || back.playlist.empty()) return false;
      out = back.playlist[0];
      return true;
    };
    Clip plain, outPlain, outSet;
    std::string t1, t2;
    expect(roundTrip(plain, outPlain, t1) && t1.find("\"playT") == std::string::npos && t1.find("\"playQ") == std::string::npos, "a clip without playback settings writes none");
    expect(roundTrip(c, outSet, t2) && outSet.playTranspose == 5 && outSet.playVelocity == -20 && outSet.playDelay == 12 &&
               outSet.playQuantize == 96,
           "the settings round-trip");
    Clip wild;
    wild.playTranspose = 400, wild.playVelocity = -900, wild.playQuantize = -5;
    Clip outWild;
    expect(roundTrip(wild, outWild, t1) && outWild.playTranspose == 48 && outWild.playVelocity == -127 && outWild.playQuantize == 0,
           "out-of-range values are clamped");
  }
  expect(clipplay::tag(c).find("+5 st") != std::string::npos, "the title tag names the transpose");
  return finish();
}
