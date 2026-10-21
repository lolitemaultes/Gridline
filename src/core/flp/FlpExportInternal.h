// Gridline — FLP export internals shared by FlpExport*.cpp (J-34).
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "FlpEncode.h"
#include "FlpExport.h"
#include "FlpOrigin.h"
#include "FlpStandIn.h"
#include "FlpWriter.h"

namespace gl::flp::xp {

// FL colours are 0x00BBGGRR.
inline uint32_t flColor(Rgb c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

// FL channel types.
enum ChannelType { kSampler = 0, kPlugin = 2, kLayer = 3, kAudio = 4, kAuto = 5 };

struct Exporter {
  const Song& s;
  const ExportOptions& o;
  ExportReport& rep;
  enc::EncodeContext ec;
  IndexMap im;
  EventWriter w;

  // FL rack order: index = FL channel index.
  struct Chan {
    bool automation = false;
    ObjectId id = kNoId;
  };
  std::vector<Chan> chans;
  std::vector<std::pair<int, ObjectId>> patterns;  // (FL index, pattern) in FL order
  std::set<ObjectId> audioChannels;                // channels placed as audio clips
  std::map<ObjectId, int> audioCents;              // audio-clip channel → pitch written in its levels (cents)
  int insertCount = 0;                             // inserts written (master … last, + the current one)
  const StandInResult* standIns = nullptr;         // J-80: plugins replaced by stand-ins (report)
  // Report entry for a plugin: the stand-in's when it has one.
  void reportPlugin(ExportReport::Plugin rp, ObjectId owner, int slot);

  Exporter(const Song& song, const ExportOptions& opt, ExportReport& r) : s(song), o(opt), rep(r) {}

  EventList origin(const std::string& key) const { return o.useOrigin ? originEvents(s, key) : EventList{}; }
  static bool unchanged(const EventList& ev, uint64_t fp) {
    uint64_t f = 0;
    return originFingerprint(ev, f) && f == fp;
  }
  int channelType(const Channel& c) const;

  // Sections (FlpExport*.cpp).
  void assignIndices();
  void writeHeader();
  void writePatternNotes();
  void writeLinks();
  void writeChannel(int flIndex);
  void writePatternMeta();
  void writeArrangements();
  void writeMixer();
  void writeTrailer();

  // A plugin as FL plugin events: name (201), wrapper (212), state (213), and
  // the origin's other plugin events (203, 155, 128, 41) when it has any.
  struct PluginOut {
    bool flPlugin = false;    // an FL plugin is written (false: placeholder)
    std::string flName;
    Bytes wrapper;
    bool hasState = false;
    Bytes state;
    EventList extras;         // origin 203 / 155 / 128 / 41
  };
  PluginOut encodePlugin(const PluginState& p, ObjectId owner, int slot, bool generator, const std::string& where);
  // True when the plugin has an FL plugin to be written as (origin or encoder).
  bool hasFlPlugin(const PluginState& p, ObjectId owner, int slot) const;
  // Encoded plugins (filled before writing: link targets precede channels),
  // and their decoded mappings (automation targets / values), by (owner, slot).
  std::map<std::pair<ObjectId, int>, PluginOut> pluginCache;
  std::map<std::pair<ObjectId, int>, PluginMapping> writtenPlugins;
  void encodeAllPlugins();

  // FL target id (227 / 223) of a Gridline parameter, and the FL value (0..1)
  // for a Gridline normalized value. False when it has no FL target.
  bool flTarget(ParamId id, uint32_t& target);
  double flValue(uint32_t target, double glNorm);  // NaN when unknown
  // Gridline normalized value the importer makes of an FL value for a target (NaN: unknown).
  double glValue(uint32_t target, double flValue);
  // Import-side view used to resolve targets with the importer's own logic.
  struct Resolver;
  std::shared_ptr<Resolver> resolver;
  Resolver& res();
};
inline constexpr int kMixerBase = 448;  // FL 25: mixer target / record insert base (0x7000 >> 6)

// Pattern note record (24 bytes) for a Gridline note.
Bytes noteRecord(const Note& n, int flChannel, const Bytes* tmpl);
// Decodes a note record the way the importer does (for template matching).
Note decodeNoteRecord(const uint8_t* r, int ppq, Tick stepLen);

}  // namespace gl::flp::xp
