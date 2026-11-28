#pragma once
// K-86 part 2 (0.7.9): Collaboration chat messages and structured reference attachments.
// Persisted in project JSON only when messages exist (empty projects serialize byte-identically
// to 0.7.8) and synced peer-to-peer via signed CRDT ops under meta/chat:* without touching undo.

#include "Json.h"
#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gl {

struct Song;
struct ParamId;

enum class ChatRefKind : uint8_t {
  None = 0,
  Clip,        // Playlist clip (pattern, audio, or automation): id = Clip::id
  Pattern,     // Pattern: id = Pattern::id
  Channel,     // Channel: id = Channel::id
  Plugin,      // Plugin or mixer effect slot: id = owner (channelId or mixerTrackId), sub = slot (-1 channel, 0..9 mixer)
  Param,       // Knob / parameter: id = ParamId::owner, sub = ParamId::index
  MixerTrack,  // Mixer track: id = MixerTrack::id
  Notes,       // Note selection: id = Pattern::id, extra = Channel::id, tick0..tick1, pitchLo..pitchHi, count
  Marker,      // Timeline marker: tick0 = Marker::pos, label = marker name snapshot
  Count,
};

struct ChatAttachment {
  ChatRefKind kind = ChatRefKind::None;
  ObjectId id = kNoId;     // primary target object id (clip / pattern / channel / owner / mixerTrack)
  int sub = -1;            // mixer slot (0..9), -1 for channel plugin, or ParamId::index
  ObjectId extra = kNoId;  // secondary object id (channel id for Notes)
  Tick tick0 = 0;          // start tick (Notes / Marker / Clip)
  Tick tick1 = 0;          // end tick (Notes)
  int pitchLo = 0;         // lowest MIDI pitch (Notes)
  int pitchHi = 0;         // highest MIDI pitch (Notes)
  int count = 0;           // note count (Notes)
  std::string label;       // fallback display name snapshot (used if deleted)
  std::string detail;      // compact secondary info snapshot (e.g. "Bar 2", "Insert 1 · Slot 2")

  bool valid() const { return kind != ChatRefKind::None && kind < ChatRefKind::Count; }

  bool operator==(const ChatAttachment& o) const {
    return kind == o.kind && id == o.id && sub == o.sub && extra == o.extra && tick0 == o.tick0 && tick1 == o.tick1 &&
           pitchLo == o.pitchLo && pitchHi == o.pitchHi && count == o.count && label == o.label && detail == o.detail;
  }
  bool operator!=(const ChatAttachment& o) const { return !(*this == o); }
};

struct ChatMessage {
  std::string id;       // deterministic unique id ("<hlc16hex>-<site16hex>")
  uint64_t hlc = 0;     // Hybrid Logical Clock timestamp for deterministic ordering across peers
  uint64_t site = 0;    // author SiteId (tie-breaker for equal HLC and live profile/colour lookup)
  uint64_t atMs = 0;    // wall-clock UTC ms for human timestamp display (never used for ordering)
  std::string author;   // author display name snapshot
  uint32_t color = 0;   // author 0xRRGGBB colour snapshot (0 = derive from site)
  std::string text;     // optional message / comment body (UTF-8, cleaned)
  ChatAttachment att;   // optional structured reference to a song element

  bool hasAttachment() const { return att.valid(); }

  bool operator==(const ChatMessage& o) const {
    return id == o.id && hlc == o.hlc && site == o.site && atMs == o.atMs && author == o.author && color == o.color &&
           text == o.text && att == o.att;
  }
  bool operator!=(const ChatMessage& o) const { return !(*this == o); }
};

// Deterministic total order across all peers: (hlc, site, id).
inline bool chatMessageLess(const ChatMessage& a, const ChatMessage& b) {
  if (a.hlc != b.hlc) return a.hlc < b.hlc;
  if (a.site != b.site) return a.site < b.site;
  return a.id < b.id;
}

constexpr size_t kChatMaxTextBytes = 2000;
constexpr size_t kChatMaxLabelBytes = 160;
constexpr size_t kChatMaxAuthorBytes = 64;
// A chat message as it travels (JSON, compact): above this a member's session refuses it.
constexpr size_t kChatMaxRawBytes = 8192;

std::string cleanChatText(const std::string& raw, size_t maxBytes = kChatMaxTextBytes);
std::string cleanChatLabel(const std::string& raw, size_t maxBytes = kChatMaxLabelBytes);

// Deterministic message ID "<hlc16hex>-<site16hex>".
std::string makeChatMessageId(uint64_t hlc, uint64_t site);

const char* chatRefKindKey(ChatRefKind k);
ChatRefKind chatRefKindFromKey(const std::string& key);
// Translatable English badge label (pass to i18n::tr).
const char* chatRefKindBadge(ChatRefKind k);

Json chatAttachmentToJson(const ChatAttachment& att);
bool chatAttachmentFromJson(const Json& j, ChatAttachment& out);

Json chatMessageToJson(const ChatMessage& msg);
bool chatMessageFromJson(const Json& j, ChatMessage& out);

struct ChatRefResolution {
  bool exists = false;
  std::string title;   // live resolved name, or snapshot label when deleted
  std::string detail;  // live resolved context detail, or snapshot detail when deleted
};
ChatRefResolution resolveChatAttachment(const Song& s, const ChatAttachment& att);

ChatAttachment makeClipAttachment(const Song& s, ObjectId clipId);
ChatAttachment makePatternAttachment(const Song& s, ObjectId patId);
ChatAttachment makeChannelAttachment(const Song& s, ObjectId chId);
ChatAttachment makePluginAttachment(const Song& s, ObjectId ownerId, int slot);
ChatAttachment makeParamAttachment(const Song& s, const ParamId& pid);
ChatAttachment makeMixerTrackAttachment(const Song& s, ObjectId mixerId);
ChatAttachment makeNotesAttachment(const Song& s, ObjectId patId, ObjectId chId, const std::vector<size_t>& selectedIndices);
ChatAttachment makeMarkerAttachment(const Song& s, size_t markerIndex);

}  // namespace gl
