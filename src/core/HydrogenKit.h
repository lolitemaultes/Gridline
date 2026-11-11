// Gridline: drum kits in the open drum machine kit format (a "drumkit.xml" beside its samples,
// shipped packed as a ".h2drumkit" archive: a tar file, usually gzip compressed).
//
// The reader is Gridline's own, written from the format's published schema. It takes every version
// of the file: the current one (components inside each instrument, each with its own sample choice),
// the 1.x one (a kit-wide component list the instruments' components point at), the 0.9 one (layers
// straight inside the instrument) and the oldest (one sample file named on the instrument).
//
// A kit becomes Abacus (gl.pads) channels: an instrument per pad (32 per channel, more instruments
// go on to another channel), its layers as velocity layers, its mute group as a cut group, its
// volume, gain, pan, pitch, envelope and stop-note setting on the pad. kitToPads says what could not
// be carried over, in plain words, for the import report.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "PluginInfo.h"

namespace gl::hydrogen {

enum class Select { Velocity, RoundRobin, Random };

struct Layer {
  std::string file;  // as written (relative to the kit folder, or absolute in old kits)
  float min = 0, max = 1;  // velocity range 0..1
  float gain = 1;
  float pitch = 0;  // semitones
  bool muted = false;
};

struct Component {
  int id = 0;  // 1.x: the kit component it belongs to
  std::string name;
  float gain = 1;
  bool muted = false;
  Select select = Select::Velocity;
  std::vector<Layer> layers;
};

struct Instrument {
  int id = 0;
  std::string name;
  float volume = 1, gain = 1;
  float pan = 0;          // -1 (left) .. 1 (right)
  float pitch = 0;        // semitones
  float randomPitch = 0;  // 0..1
  bool muted = false, soloed = false;
  bool applyVelocity = true;
  bool stopNote = false;  // a note off stops the sample
  bool filterActive = false;
  float cutoff = 1, resonance = 0;  // 0..1
  // Envelope: attack, decay and release in sample frames, sustain 0..1.
  int attack = 0, decay = 0, release = 1000;
  float sustain = 1;
  int muteGroup = -1;
  int midiOutNote = -1;
  std::vector<Component> components;
};

struct KitComponent {  // 1.x kits: the kit-wide component list
  int id = 0;
  std::string name;
  float volume = 1;
};

struct Kit {
  std::string name, author, info, license, image, imageLicense;
  int formatVersion = 0;
  std::vector<std::string> tags;
  std::vector<KitComponent> kitComponents;
  std::vector<Instrument> instruments;
  std::string folder;  // the folder that holds drumkit.xml (sample paths resolve against it)
};

// The file's text. False (and `err`) when it is not a kit file.
bool parseDrumkitXml(const std::string& xml, Kit& out, std::string* err = nullptr);
// The drumkit.xml in a folder (or in the one folder inside it), or the path itself when it names the
// file. Empty when there is none.
std::string kitXmlIn(const std::string& folderOrFile);
// A folder holding a kit (cheap: one check).
bool isKitFolder(const std::string& dir);
// Reads a kit from its folder or its drumkit.xml.
bool readKit(const std::string& folderOrFile, Kit& out, std::string* err = nullptr);
// The file a layer plays: relative names resolve against the kit folder; a name that is not there is
// looked for without regard to case, then as .flac (old kits renamed their samples once).
std::string samplePath(const Kit& kit, const std::string& file);

// ---- the packed kit (.h2drumkit)
bool isKitArchive(const std::string& path);
// gzip (RFC 1952), checked against its CRC. Data that is not gzip is copied as it is (a plain tar).
bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string* err = nullptr);
struct TarEntry {
  std::string name;  // '/' separators
  uint64_t offset = 0, size = 0;
  bool dir = false;
};
// The files and folders of a tar archive (ustar, GNU long names, pax path records). Links and other
// special entries are left out.
bool listTar(const std::vector<uint8_t>& tar, std::vector<TarEntry>& out, std::string* err = nullptr);
// Unpacks a .h2drumkit's bytes below `folder`. Names that would leave it refuse the archive before
// anything is written. `written` lists the files, relative to `folder`.
bool unpackKit(const std::vector<uint8_t>& archive, const std::string& folder, std::vector<std::string>& written, std::string* err = nullptr);

// ---- the kit as Abacus channels
inline constexpr int kFirstNote = 36;    // instrument k plays on note 36 + k, as in the kit's own player
inline constexpr int kMaxChannels = 3;   // 96 instruments
inline constexpr double kFrameRate = 44100.0;  // the envelope's frames, in seconds
struct PadsImport {
  std::vector<PluginState> channels;  // gl.pads states
  std::vector<std::string> names;     // a name per channel
  int instruments = 0, layers = 0;
  std::vector<std::string> missing;   // samples that are not there
  // What could not be carried over, one line each: `text` (English, translatable, one %s) and `arg`.
  struct Note {
    const char* text;
    std::string arg;
  };
  std::vector<Note> notes;
};
PadsImport kitToPads(const Kit& kit);
// The kit's credits for the report, the ones it states: {label (English, translatable), value}.
std::vector<std::pair<const char*, std::string>> creditLines(const Kit& kit);

}  // namespace gl::hydrogen
