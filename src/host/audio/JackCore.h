// Gridline: platform-independent JACK / PipeWire core logic (K-357).
// Port planning, name sanitization, track-alias mapping, saved connection serialization and
// restoration filtering, physical port auto-connect selection, latency calculation, status text
// and PipeWire environment formatting. Tested on every platform.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "AsioCore.h"
#include "JackApi.h"

namespace gl::jack {

inline constexpr int kMaxInputs = 16;       // matches kMaxInputChannels
inline constexpr int kMaxExtraPairs = 15;   // matches AudioEngine::kMaxDevicePairs
inline constexpr size_t kMaxClientNameLen = 32;
inline constexpr size_t kMaxPortNameLen = 64;

// Sanitizes a JACK client name, port name or alias: removes control chars, ':' (reserved as
// client:port separator) and path separators, collapses whitespace and clamps length cleanly.
std::string sanitizeName(const std::string& raw, size_t maxLen = kMaxPortNameLen, const std::string& fallback = "Gridline");
std::string sanitizeClientName(const std::string& raw);

// Builds an input port's display alias from its base short name ("In 1") and optional track name ("Vocal").
std::string formatTrackAlias(const std::string& basePort, const std::string& trackName);

struct PortSpec {
  std::string name;   // short JACK port name ("In 1", "Out 1", ...)
  std::string alias;  // human alias / pretty name ("In 1", "Main L", "Pair 1 L", ...)
  bool input = false;
  int channel = 0;    // engine input index (for inputs) or output index (for outputs)
};

struct PortPlan {
  std::vector<PortSpec> ins;   // "In 1" .. "In N"
  std::vector<PortSpec> outs;  // "Out 1", "Out 2", then "Out 3" .. for extra pairs
};

// Plans the fixed pool of named ports for `inputs` (clamped 0..16) and `extraPairs` (clamped 0..15).
PortPlan planPorts(int inputs, int extraPairs);

// Minimal view of a mixer track for deriving input port aliases off the audio thread.
struct TrackInputRef {
  std::string name;
  int input = -1;
  bool stereo = true;
};

// Computes one alias per planned input port: first track using that channel gives "In k (<Track>)".
std::vector<std::string> planInputAliases(const PortPlan& plan, const std::vector<TrackInputRef>& tracks);

// A saved connection between one of Gridline's ports (short name) and another client's full port name.
struct Link {
  std::string own;   // e.g. "In 1" or "Out 1"
  std::string peer;  // e.g. "Tone:out_1" or "system:playback_1"
  bool operator==(const Link& o) const { return own == o.own && peer == o.peer; }
  bool operator!=(const Link& o) const { return !(*this == o); }
};

// Serialization helpers for text representation ("own -> peer" per line).
std::vector<Link> parseLinks(const std::string& text);
std::string formatLinks(const std::vector<Link>& links);

// Filters `saved` to links whose own port exists in `ownPorts`, whose peer exists in `existingPeers`,
// and that are not already present in `current`. Never returns duplicates.
std::vector<Link> linksToRestore(const std::vector<Link>& saved, const std::vector<std::string>& ownPorts,
                                 const std::vector<std::string>& existingPeers, const std::vector<Link>& current);

// Picks up to `wantCount` physical ports for auto-connect in natural channel order.
std::vector<std::string> pickPhysical(const std::vector<std::string>& physicalPorts, int wantCount);

// Automatic recording latency (samples): max capture latency of connected inputs + max playback
// latency of connected main outputs.
int recordLatency(const std::vector<LatencyRange>& captureRanges, const std::vector<LatencyRange>& playbackRanges);

// Formats PIPEWIRE_LATENCY="<frames>/<rate>" when requestFrames > 0, else empty string.
std::string pipewireLatencyEnv(int requestFrames, int sampleRate);

// Maps "mixer:pair" extra-output settings onto `extraPairs` slots (-1 = direct outputs only).
std::vector<int> resolveExtraMixers(int extraPairs, const std::vector<asio::ExtraOut>& extras);

struct StatusInfo {
  std::string clientName;
  bool viaPipeWire = false;
  bool fake = false;
  int rate = 0;
  int buffer = 0;
  int inputs = 0;
  int outputs = 0;
  int inLatency = 0;
  int outLatency = 0;
  int xruns = 0;
};

// Formats human-readable status / latency summaries (English; UI wraps static labels via i18n).
std::string formatStatusLine(const StatusInfo& s);
std::string formatLatencyLine(const StatusInfo& s);

// Compact peer list for track input menus ("Tone:out_1, Tone:out_2" or empty when unconnected).
std::string formatConnectedPeers(const std::vector<std::string>& peers);

}  // namespace gl::jack
