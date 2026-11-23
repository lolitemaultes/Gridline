// Gridline: the "Audio clip" window's plan. See ClipWindow.h.
#include "ClipWindow.h"
#include <algorithm>
#include <cmath>

namespace gl::clipwin {

Scope scopeOf(int tab) {
  switch (tab) {
    case TabClip:
    case TabTime: return Scope::Clip;
    case TabSample:
    case TabPlayback:
    case TabEnvelope:
    case TabFilter:
    case TabSampleFx: return Scope::Sample;
    case TabChannel:
    case TabNoteFx:
    case TabMidi: return Scope::Channel;
    default: return Scope::Tools;
  }
}

const char* tabName(int tab) {
  static const char* const kNames[TabCount] = {"Clip",    "Warp",         "Sample",          "Playback",   "Envelope", "Filter",
                                               "Sample FX", "Channel", "Note effects", "MIDI and macros", "Edit audio", "Analysis"};
  return tab >= 0 && tab < TabCount ? kNames[tab] : "";
}

const std::vector<Group>& groups() {
  static const std::vector<Group> g = {{TabClip, 2}, {TabSample, 5}, {TabChannel, 3}, {TabEdit, 2}};
  return g;
}

bool hasModules(int tab) { return tab >= 0 && tab < TabEdit; }

bool tabShown(int tab, bool hasClip) { return tab >= 0 && tab < TabCount && (hasClip || scopeOf(tab) != Scope::Clip); }

int shownTab(int tab, bool hasClip) { return tabShown(tab, hasClip) ? tab : TabPlayback; }

bool ownsType(const std::string& type) { return type == "gl.sampler"; }

int entryTab(Entry e) {
  switch (e) {
    case Entry::DoubleClick:
    case Entry::EditClip: return TabClip;
    case Entry::ChannelSettings:
    case Entry::DoubleClickSetting: return TabPlayback;
  }
  return TabClip;
}

std::vector<FlowRow> flowRows(const std::vector<float>& minW, float avail, float gap, std::vector<float>* widths, const std::vector<float>* grow) {
  std::vector<FlowRow> rows;
  if (widths) widths->assign(minW.size(), 0.0f);
  size_t k = 0;
  while (k < minW.size()) {
    size_t e = k;
    float used = 0;
    while (e < minW.size() && (e == k || used + gap + minW[e] <= avail)) used += (e == k ? 0 : gap) + minW[e], ++e;
    rows.push_back({k, e - k});
    if (widths) {
      // The row stretched to the width (never narrower than its least widths); the last card takes
      // what the rounding left so the row ends at the width exactly.
      float shares = 0;
      if (grow)
        for (size_t j = k; j < e; ++j) shares += std::max(0.0f, j < grow->size() ? (*grow)[j] : 1.0f);
      auto share = [&](size_t j) {
        if (shares <= 0) return 1.0f / static_cast<float>(e - k);
        return std::max(0.0f, j < grow->size() ? (*grow)[j] : 1.0f) / shares;
      };
      const float spare = std::max(0.0f, avail - used);
      float x = 0;
      for (size_t j = k; j < e; ++j) {
        float w = std::floor(minW[j] + spare * share(j));
        if (j + 1 == e && used <= avail) w = std::max(minW[j], avail - x);
        (*widths)[j] = w;
        x += w + gap;
      }
    }
    k = e;
  }
  return rows;
}

const std::vector<Item>& inventory() {
  // key, the old control, where it was, its home now (a module), the tab its module is on, when it is needed.
  static const std::vector<Item> v = {
      // header
      {"colour", "Colour swatch", WasBoth, "Header", -1, NeedAlways},
      {"name", "Clip name / channel name", WasBoth, "Header", -1, NeedAlways},
      {"info", "Warp badge, file info, shared by N clips", WasClipEditor, "Header", -1, NeedAlways},
      {"selcount", "And N more selected", WasClipEditor, "Header", -1, NeedClip},
      {"route", "Mixer track route", WasChannelSettings, "Header: the mixer track box, as plugin windows have it (Clips: the head card)", -1, NeedAlways},
      {"routefree", "Route to a free mixer track", WasChannelSettings, "Header: the button after the mixer track box (Clips: the head card)", -1, NeedAlways},
      {"vol", "Strip volume", WasChannelSettings, "Output (pinned at the right of every tab)", -1, NeedAlways},
      {"pan", "Strip pan", WasChannelSettings, "Output (pinned at the right of every tab)", -1, NeedAlways},
      {"pitch", "Strip pitch", WasChannelSettings, "Output (pinned at the right of every tab)", -1, NeedAlways},
      {"loupe", "Open in Loupe / Edit in Loupe", WasBoth, "Output: Open in Loupe", -1, NeedAlways},
      {"menu", "Instrument menu", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.presets", "Presets", WasChannelSettings, "Header menu > Presets", -1, NeedAlways},
      {"menu.load", "Load preset from file", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.save", "Save preset to file", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.copy", "Copy state", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.paste", "Paste state", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.random", "Randomize", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.reset", "Reset to default", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.replace", "Instrument list (replace the instrument)", WasChannelSettings, "Header menu > Replace instrument", -1, NeedAlways},
      {"menu.rename", "Channel name (edit)", WasChannelSettings, "Header menu > Rename channel", -1, NeedAlways},
      {"menu.clone", "Clone channel", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.colour", "Colour (menu)", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.delete", "Delete channel", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.pianoroll", "Piano roll", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.rack", "Show in the channel rack", WasChannelSettings, "Header menu", -1, NeedAlways},
      {"menu.unique", "Make this clip's sample unique", WasClipEditor, "Header menu and the Sample module's menu", -1, NeedClip},
      {"menu.audiotools", "Audio tools", WasClipEditor, "Header menu > Audio tools", -1, NeedClip},
      // tool strip, waveform, overview
      {"tools", "Tools: pointer, warp, slip, fade, slice, pencil, scrub, zoom", WasClipEditor, "Tools row", -1, NeedFullView},
      {"view", "Wave / Spectrum", WasClipEditor, "Tools row", -1, NeedFullView},
      {"units", "Bars / Time", WasClipEditor, "Tools row", -1, NeedFullView},
      {"snap", "Snap and its grid", WasClipEditor, "Tools row", -1, NeedFullView},
      {"preview", "Preview and loop the preview", WasClipEditor, "Tools row", -1, NeedFullView},
      {"follow", "Follow", WasClipEditor, "Tools row", -1, NeedFullView},
      {"wave", "Waveform: trim, slip, fades, curves, volume line, warp markers, selection and its menu", WasClipEditor, "Waveform", -1, NeedClip},
      {"overview", "Overview (the view's window)", WasClipEditor, "Overview", -1, NeedClip},
      {"handles", "Whole-file waveform with start / end handles, sampler START / END markers", WasChannelSettings, "Overview: blue handles", -1, NeedAlways},
      {"loopbar", "Sampler LOOP markers", WasChannelSettings, "Overview: the notes' loop bar", -1, NeedAlways},
      {"drop", "Drop a file on the sample", WasChannelSettings, "Waveform and overview", -1, NeedAlways},
      // Clip
      {"gain", "Gain", WasClipEditor, "Clip module", TabClip, NeedClip},
      {"normgain", "Normalize (clip gain)", WasClipEditor, "Clip module: Normalize gain", TabClip, NeedClip},
      {"reverse.clip", "Reverse (this clip)", WasClipEditor, "Clip module: Reverse", TabClip, NeedClip},
      {"mute", "Mute", WasClipEditor, "Clip module", TabClip, NeedClip},
      {"fades", "Fade in / out length, shape, curve", WasClipEditor, "Fades module", TabClip, NeedClip},
      {"autoxfade", "Auto crossfade", WasClipEditor, "Fades module: Auto crossfade", TabClip, NeedClip},
      {"semis", "Pitch: semitones", WasClipEditor, "Clip module", TabClip, NeedClip},
      {"cents", "Pitch: cents", WasClipEditor, "Clip module", TabClip, NeedClip},
      {"loopclip", "Loop the clip, loop start, reset", WasClipEditor, "Clip module: Loop, its start; Reset in its menu", TabClip, NeedClip},
      {"takes", "Takes: take, rating, take lanes", WasClipEditor, "Clip module menu: takes, rating, take lanes", TabClip, NeedClip},
      {"launch", "Launch and follow (clip grid)", WasClipEditor, "Launch module (Clips layout)", TabClip, NeedClipsLaunch},
      // Time and warp
      {"tempo", "Follow the project tempo", WasClipEditor, "Warp module: Follow tempo", TabTime, NeedClip},
      {"stretch", "Stretch, Half, Twice, Original", WasClipEditor, "Warp module", TabTime, NeedClip},
      {"speed", "Speed and Fit to tempo", WasClipEditor, "Warp module: Speed, Fit to tempo", TabTime, NeedClip},
      {"warpops", "Warp markers: add, quantize, strength, clear, swing, groove, snap", WasClipEditor, "Warp module and its menu", TabTime, NeedClip},
      {"engine", "Stretch engine (in three places before)", WasBoth, "Warp module: stretch engine (Stretch engine module without a clip)", -1, NeedAlways},
      {"pitchshift", "Sampler pitch shift", WasChannelSettings, "Stretch engine", -1, NeedAlways},
      {"timestretch", "Sampler time stretch", WasChannelSettings, "Stretch engine", -1, NeedAlways},
      {"fittempo", "Follows tempo from (hidden before)", WasChannelSettings, "Stretch engine", -1, NeedAlways},
      // Sample
      {"file", "Load sample, file name, missing", WasChannelSettings, "Sample module: the file; Replace sample in its menu", TabSample, NeedAlways},
      {"startend", "Sample start / end", WasBoth, "Sample module: start and end", TabSample, NeedAlways},
      {"samplewarp", "Sample warp: warp mode, markers on the sampler's well, its Warp menu", WasChannelSettings, "Sample warp module", TabTime, NeedAlways},
      {"unique", "Make this clip's sample unique", WasClipEditor, "Sample module menu", TabSample, NeedClip},
      {"normalize.sample", "Normalize sample", WasBoth, "Sample module: the older setting's note, its menu", TabSample, NeedAlways},
      {"removedc", "Remove DC", WasBoth, "Sample module", TabSample, NeedAlways},
      {"polarity", "Invert polarity", WasBoth, "Sample module", TabSample, NeedAlways},
      {"swapstereo", "Swap stereo", WasBoth, "Sample module", TabSample, NeedAlways},
      {"smpfades", "Sampler fade in / fade out", WasChannelSettings, "Sample module", TabSample, NeedAlways},
      // Playback
      {"oneshot", "One shot", WasChannelSettings, "Playback module", TabPlayback, NeedAlways},
      {"reverse.notes", "Reverse (the notes)", WasChannelSettings, "Playback module: Reverse sample", TabPlayback, NeedAlways},
      {"loop.notes", "Loop, loop mode, loop points, loop crossfade", WasChannelSettings, "Playback module", TabPlayback, NeedAlways},
      {"declick", "Declick", WasChannelSettings, "Playback module", TabPlayback, NeedAlways},
      {"root", "Root note", WasChannelSettings, "Voice module", TabPlayback, NeedAlways},
      {"voice", "Fine, pan, voices, glide, volume", WasChannelSettings, "Voice module", TabPlayback, NeedAlways},
      {"audition", "Audition keyboard (sets the root), strip preview keyboard", WasChannelSettings, "Voice module: the keyboard", TabPlayback, NeedAlways},
      // Envelope
      {"volenv", "Volume envelope and velocity", WasChannelSettings, "Envelope module", TabEnvelope, NeedAlways},
      {"filter", "Filter type, cutoff, resonance, envelope, velocity, key", WasChannelSettings, "Filter module (with its envelope)", TabFilter, NeedAlways},
      {"lfo", "LFO target, shape, speed, amount, delay", WasChannelSettings, "LFO module", TabEnvelope, NeedAlways},
      // Sample FX
      {"fx.shape", "Ramps, time warp, boost, stereo delay, boost clip, stereo fade", WasChannelSettings, "FX shape module", TabSampleFx, NeedAlways},
      {"fx.tone", "Cutoff, resonance, tilt, ring mix, ring frequency", WasChannelSettings, "FX tone module", TabSampleFx, NeedAlways},
      {"fx.space", "Reverb, room or hall", WasChannelSettings, "FX space module", TabSampleFx, NeedAlways},
      {"presets", "Preset box, previous / next", WasChannelSettings, "Header: the sampler's presets", -1, NeedAlways},
      // Channel
      {"tune", "Fine, bend range, swing, time shift", WasChannelSettings, "Channel module", TabChannel, NeedAlways},
      {"poly", "Voices, glide, mono, porta", WasChannelSettings, "Polyphony module", TabChannel, NeedAlways},
      {"cut", "Cut group, cut by", WasChannelSettings, "Polyphony module", TabChannel, NeedAlways},
      {"expr", "Note bend, pressure, timbre, amount", WasChannelSettings, "Channel module", TabChannel, NeedAlways},
      {"echo", "Echoes, feedback, time, pan, pitch, mod X, mod Y, ping-pong", WasChannelSettings, "Echo module", TabNoteFx, NeedAlways},
      {"layer", "Layer children, layer mode, display group, MIDI input", WasChannelSettings, "Layer and MIDI module", TabMidi, NeedAlways},
      {"macros", "Macros", WasChannelSettings, "Macros module", TabMidi, NeedAlways},
      {"notefx", "Note processors", WasChannelSettings, "Note processors module", TabNoteFx, NeedAlways},
      // Edit audio and Analysis
      {"edit.ops", "Edit audio: the 24 operations", WasClipEditor, "Tools row: Edit audio...", TabEdit, NeedAlways},
      {"edit.send", "Split stems, chop, to sampler, to slicer, export WAV, consolidate", WasClipEditor, "Tools row: Edit audio... > Send and split", TabEdit, NeedClip},
      {"analysis", "Peak, RMS, loudness, true peak, pitch, tempo, DC", WasClipEditor, "Tools row: the live analysis", TabAnalysis, NeedAlways},
  };
  return v;
}

const std::vector<std::string>& mainKeys(bool hasClip) {
  // the sampler's own small window: the header and its menu (the clip's own few entries with a clip), the
  // strip's volume, pan and pitch, the sample row, its four pages' settings (the audition keyboard left to Advanced)
  static const std::vector<std::string> chan = {
      "colour", "name", "info", "route", "routefree", "vol", "pan", "pitch", "loupe", "menu", "menu.presets", "menu.load", "menu.save", "menu.copy",
      "menu.paste", "menu.random", "menu.reset", "menu.replace", "menu.rename", "menu.clone", "menu.colour", "menu.delete", "menu.pianoroll", "menu.rack",
      "presets", "handles", "loopbar", "drop", "file", "startend", "normalize.sample", "removedc", "polarity", "swapstereo", "smpfades", "engine",
      "pitchshift", "timestretch", "fittempo", "oneshot", "reverse.notes", "loop.notes", "declick",
      // Envelope / filter, Misc (root and voice), Sample FX
      "volenv", "filter", "lfo", "root", "voice", "fx.shape", "fx.tone", "fx.space"};
  static const std::vector<std::string> clip = [] {
    std::vector<std::string> v = chan;
    for (const char* k : {"selcount", "menu.unique", "menu.audiotools"})
      v.push_back(k);
    return v;
  }();
  return hasClip ? clip : chan;
}

std::vector<std::string> missingMain(const std::set<std::string>& reached, bool hasClip) {
  std::vector<std::string> out;
  for (const std::string& k : mainKeys(hasClip))
    if (!reached.count(k)) out.push_back(k);
  return out;
}

std::vector<std::string> missing(const std::set<std::string>& reached, bool hasClip, bool clipsLaunch) {
  std::vector<std::string> out;
  for (const Item& it : inventory()) {
    const bool needed = it.need == NeedAlways || ((it.need == NeedClip || it.need == NeedFullView) && hasClip) || (it.need == NeedClipsLaunch && clipsLaunch);
    if (needed && !reached.count(it.key)) out.push_back(it.key);
  }
  return out;
}

}  // namespace gl::clipwin
