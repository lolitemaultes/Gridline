// K-86 part 2 (0.7.9): Collaboration chat model, compact JSON codecs, and live reference resolution.
#include "CollabChat.h"

#include "I18n.h"
#include "Meter.h"
#include "Params.h"
#include "PluginInfo.h"
#include "Song.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace gl {

namespace {

std::string u64Hex(uint64_t v) {
  char buf[20];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

bool parseU64Hex(const std::string& s, uint64_t& out) {
  if (s.empty() || s.size() > 16) return false;
  uint64_t v = 0;
  for (unsigned char c : s) {
    v <<= 4;
    if (c >= '0' && c <= '9') v |= static_cast<uint64_t>(c - '0');
    else if (c >= 'a' && c <= 'f') v |= static_cast<uint64_t>(10 + c - 'a');
    else if (c >= 'A' && c <= 'F') v |= static_cast<uint64_t>(10 + c - 'A');
    else return false;
  }
  out = v;
  return true;
}

// Clamps a UTF-8 string to at most `maxBytes` without splitting a multi-byte sequence.
void clampUtf8(std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return;
  size_t n = maxBytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
  s.resize(n);
}

std::string barLabel(const Song& s, Tick tick) {
  const MeterMap mm(s);
  const int64_t bar = mm.barIndex(std::max<Tick>(0, tick)) + 1;
  return "Bar " + std::to_string(bar);
}

std::string clipTitle(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Pattern) {
    if (const Pattern* p = s.pattern(c.pattern); p && !p->name.empty()) return p->name;
    return "Pattern";
  }
  if (c.kind == ClipKind::Audio) {
    if (const Channel* ch = s.channel(c.source); ch && !ch->name.empty()) return ch->name;
    return "Audio clip";
  }
  if (c.kind == ClipKind::Automation) {
    if (const AutomationClip* ac = s.automation(c.source); ac && !ac->name.empty()) return ac->name;
    return "Automation";
  }
  return "Clip";
}

}  // namespace

std::string cleanChatText(const std::string& raw, size_t maxBytes) {
  std::string out;
  out.reserve(std::min(raw.size(), maxBytes));
  bool prevCr = false;
  for (unsigned char c : raw) {
    if (c == '\r') {
      out.push_back('\n');
      prevCr = true;
      continue;
    }
    if (c == '\n' && prevCr) {
      prevCr = false;
      continue;
    }
    prevCr = false;
    if (c == '\t') {
      out.push_back(' ');
    } else if (c == '\n' || c >= 0x20) {
      if (c != 0x7F) out.push_back(static_cast<char>(c));
    }
  }
  size_t a = 0;
  while (a < out.size() && (out[a] == ' ' || out[a] == '\n')) ++a;
  size_t b = out.size();
  while (b > a && (out[b - 1] == ' ' || out[b - 1] == '\n')) --b;
  out = out.substr(a, b - a);
  clampUtf8(out, maxBytes);
  return out;
}

std::string cleanChatLabel(const std::string& raw, size_t maxBytes) {
  std::string out;
  out.reserve(std::min(raw.size(), maxBytes));
  for (unsigned char c : raw) {
    if (c == '\r' || c == '\n' || c == '\t') out.push_back(' ');
    else if (c >= 0x20 && c != 0x7F) out.push_back(static_cast<char>(c));
  }
  size_t a = 0;
  while (a < out.size() && out[a] == ' ') ++a;
  size_t b = out.size();
  while (b > a && out[b - 1] == ' ') --b;
  out = out.substr(a, b - a);
  clampUtf8(out, maxBytes);
  return out;
}

std::string makeChatMessageId(uint64_t hlc, uint64_t site) { return u64Hex(hlc) + "-" + u64Hex(site); }

const char* chatRefKindKey(ChatRefKind k) {
  switch (k) {
    case ChatRefKind::Clip: return "clip";
    case ChatRefKind::Pattern: return "pat";
    case ChatRefKind::Channel: return "ch";
    case ChatRefKind::Plugin: return "plug";
    case ChatRefKind::Param: return "param";
    case ChatRefKind::MixerTrack: return "mx";
    case ChatRefKind::Notes: return "notes";
    case ChatRefKind::Marker: return "mark";
    default: return "";
  }
}

ChatRefKind chatRefKindFromKey(const std::string& key) {
  if (key == "clip") return ChatRefKind::Clip;
  if (key == "pat") return ChatRefKind::Pattern;
  if (key == "ch") return ChatRefKind::Channel;
  if (key == "plug") return ChatRefKind::Plugin;
  if (key == "param") return ChatRefKind::Param;
  if (key == "mx") return ChatRefKind::MixerTrack;
  if (key == "notes") return ChatRefKind::Notes;
  if (key == "mark") return ChatRefKind::Marker;
  return ChatRefKind::None;
}

const char* chatRefKindBadge(ChatRefKind k) {
  switch (k) {
    case ChatRefKind::Clip: return i18n::N_("Clip");
    case ChatRefKind::Pattern: return i18n::N_("Pattern");
    case ChatRefKind::Channel: return i18n::N_("Channel");
    case ChatRefKind::Plugin: return i18n::N_("Plugin");
    case ChatRefKind::Param: return i18n::N_("Control");
    case ChatRefKind::MixerTrack: return i18n::N_("Mixer");
    case ChatRefKind::Notes: return i18n::N_("Notes");
    case ChatRefKind::Marker: return i18n::N_("Marker");
    default: return "";
  }
}

Json chatAttachmentToJson(const ChatAttachment& att) {
  Json j = Json::makeObject();
  if (!att.valid()) return j;
  j.object["k"] = Json::makeString(chatRefKindKey(att.kind));
  if (att.id != kNoId) j.object["id"] = Json::makeNumber(static_cast<double>(att.id));
  if (att.sub >= 0) j.object["sub"] = Json::makeNumber(static_cast<double>(att.sub));
  if (att.extra != kNoId) j.object["ex"] = Json::makeNumber(static_cast<double>(att.extra));
  if (att.tick0 != 0 || att.kind == ChatRefKind::Notes || att.kind == ChatRefKind::Marker)
    j.object["t0"] = Json::makeNumber(static_cast<double>(att.tick0));
  if (att.tick1 != 0) j.object["t1"] = Json::makeNumber(static_cast<double>(att.tick1));
  if (att.kind == ChatRefKind::Notes) {
    j.object["p0"] = Json::makeNumber(static_cast<double>(att.pitchLo));
    j.object["p1"] = Json::makeNumber(static_cast<double>(att.pitchHi));
  }
  if (att.count > 0) j.object["n"] = Json::makeNumber(static_cast<double>(att.count));
  const std::string lbl = cleanChatLabel(att.label);
  if (!lbl.empty()) j.object["l"] = Json::makeString(lbl);
  const std::string det = cleanChatLabel(att.detail);
  if (!det.empty()) j.object["d"] = Json::makeString(det);
  return j;
}

bool chatAttachmentFromJson(const Json& j, ChatAttachment& out) {
  out = {};
  if (!j.is(Json::Type::Object)) return false;
  const ChatRefKind k = chatRefKindFromKey(j.text("k"));
  if (k == ChatRefKind::None) return false;
  out.kind = k;
  out.id = static_cast<ObjectId>(std::max(0.0, j.num("id", 0.0)));
  if (j.find("sub")) out.sub = static_cast<int>(std::clamp(j.num("sub", -1.0), -1.0, 1.0e9));
  out.extra = static_cast<ObjectId>(std::max(0.0, j.num("ex", 0.0)));
  out.tick0 = static_cast<Tick>(std::max(0.0, j.num("t0", 0.0)));
  out.tick1 = static_cast<Tick>(std::max(0.0, j.num("t1", 0.0)));
  out.pitchLo = static_cast<int>(std::clamp(j.num("p0", 0.0), 0.0, 127.0));
  out.pitchHi = static_cast<int>(std::clamp(j.num("p1", 0.0), 0.0, 127.0));
  out.count = static_cast<int>(std::clamp(j.num("n", 0.0), 0.0, 1.0e6));
  out.label = cleanChatLabel(j.text("l"));
  out.detail = cleanChatLabel(j.text("d"));
  return true;
}

Json chatMessageToJson(const ChatMessage& msg) {
  Json j = Json::makeObject();
  j.object["id"] = Json::makeString(msg.id.empty() ? makeChatMessageId(msg.hlc, msg.site) : msg.id);
  j.object["hlc"] = Json::makeString(u64Hex(msg.hlc));
  j.object["s"] = Json::makeString(u64Hex(msg.site));
  if (msg.atMs > 0) j.object["at"] = Json::makeNumber(static_cast<double>(msg.atMs));
  const std::string au = cleanChatLabel(msg.author, kChatMaxAuthorBytes);
  if (!au.empty()) j.object["a"] = Json::makeString(au);
  if (msg.color != 0) j.object["c"] = Json::makeNumber(static_cast<double>(msg.color & 0xFFFFFFu));
  const std::string txt = cleanChatText(msg.text);
  if (!txt.empty()) j.object["m"] = Json::makeString(txt);
  if (msg.att.valid()) j.object["r"] = chatAttachmentToJson(msg.att);
  return j;
}

bool chatMessageFromJson(const Json& j, ChatMessage& out) {
  out = {};
  if (!j.is(Json::Type::Object)) return false;
  uint64_t hlc = 0, site = 0;
  if (!parseU64Hex(j.text("hlc"), hlc)) return false;
  if (!parseU64Hex(j.text("s"), site)) return false;
  out.hlc = hlc;
  out.site = site;
  out.id = j.text("id");
  if (out.id.empty()) out.id = makeChatMessageId(hlc, site);
  out.atMs = static_cast<uint64_t>(std::max(0.0, j.num("at", 0.0)));
  out.author = cleanChatLabel(j.text("a"), kChatMaxAuthorBytes);
  out.color = static_cast<uint32_t>(std::max(0.0, j.num("c", 0.0))) & 0xFFFFFFu;
  out.text = cleanChatText(j.text("m"));
  if (const Json* r = j.find("r"); r && r->is(Json::Type::Object)) {
    ChatAttachment att;
    if (chatAttachmentFromJson(*r, att)) out.att = std::move(att);
  }
  return !out.text.empty() || out.att.valid();
}

ChatRefResolution resolveChatAttachment(const Song& s, const ChatAttachment& att) {
  ChatRefResolution res;
  res.title = att.label;
  res.detail = att.detail;
  if (!att.valid()) return res;

  switch (att.kind) {
    case ChatRefKind::Clip: {
      if (const Clip* c = s.clip(att.id)) {
        res.exists = true;
        res.title = clipTitle(s, *c);
        res.detail = barLabel(s, c->start);
        if (c->track >= 0 && static_cast<size_t>(c->track) < s.tracks.size())
          res.detail += " \xC2\xB7 " + playlistTrackName(s, static_cast<size_t>(c->track));
      }
      break;
    }
    case ChatRefKind::Pattern: {
      if (const Pattern* p = s.pattern(att.id)) {
        res.exists = true;
        res.title = p->name.empty() ? std::string("Pattern") : p->name;
        res.detail = std::to_string(std::max(1, p->bars)) + (p->bars == 1 ? " bar" : " bars");
      }
      break;
    }
    case ChatRefKind::Channel: {
      if (const Channel* ch = s.channel(att.id)) {
        res.exists = true;
        res.title = ch->name.empty() ? std::string("Channel") : ch->name;
        if (const PluginInfo* pi = findPlugin(ch->plugin.type)) res.detail = pi->name;
      }
      break;
    }
    case ChatRefKind::Plugin: {
      if (att.sub < 0) {
        if (const Channel* ch = s.channel(att.id)) {
          res.exists = true;
          const PluginInfo* pi = findPlugin(ch->plugin.type);
          res.title = pi ? pi->name : (ch->name.empty() ? std::string("Instrument") : ch->name);
          res.detail = ch->name;
        }
      } else if (att.sub < kEffectSlots) {
        if (const MixerTrack* mt = s.mixerTrack(att.id)) {
          const EffectSlot& es = mt->slots[static_cast<size_t>(att.sub)];
          if (!es.plugin.type.empty()) {
            res.exists = true;
            const PluginInfo* pi = findPlugin(es.plugin.type);
            res.title = pi ? pi->name : es.plugin.type;
            const int mi = s.mixerIndex(mt->id);
            res.detail = (mi >= 0 ? mixerTrackName(s, static_cast<size_t>(mi)) : mt->name) + " \xC2\xB7 Slot " +
                         std::to_string(att.sub + 1);
          }
        }
      }
      break;
    }
    case ChatRefKind::Param: {
      if (att.sub >= 0) {
        const ParamId pid{att.id, static_cast<uint32_t>(att.sub)};
        if (const auto d = describeParam(s, pid)) {
          res.exists = true;
          res.title = paramDisplayName(s, pid);
          if (const auto v = getParam(s, pid)) res.detail = d->format(*v);
        }
      }
      break;
    }
    case ChatRefKind::MixerTrack: {
      const int mi = s.mixerIndex(att.id);
      if (mi >= 0) {
        res.exists = true;
        res.title = mixerTrackName(s, static_cast<size_t>(mi));
        res.detail = mi == 0 ? "Master" : ("Insert " + std::to_string(mi));
      }
      break;
    }
    case ChatRefKind::Notes: {
      const Pattern* p = s.pattern(att.id);
      const Channel* ch = s.channel(att.extra);
      if (p && ch) {
        auto it = p->notes.find(ch->id);
        int matched = 0;
        if (it != p->notes.end()) {
          for (const Note& n : it->second) {
            const Tick nEnd = n.start + std::max<Tick>(1, n.length);
            const bool timeHit = att.tick1 > att.tick0 ? (n.start < att.tick1 && nEnd > att.tick0) : (n.start == att.tick0);
            const bool pitchHit = att.pitchHi >= att.pitchLo ? (n.pitch >= att.pitchLo && n.pitch <= att.pitchHi) : true;
            if (timeHit && pitchHit) ++matched;
          }
        }
        if (matched > 0) {
          res.exists = true;
          res.title = (ch->name.empty() ? std::string("Channel") : ch->name) + " (" +
                      (p->name.empty() ? std::string("Pattern") : p->name) + ")";
          res.detail = std::to_string(matched) + (matched == 1 ? " note" : " notes");
        }
      }
      break;
    }
    case ChatRefKind::Marker: {
      const Marker* found = nullptr;
      for (const Marker& mk : s.markers) {
        if (mk.pos == att.tick0) {
          found = &mk;
          break;
        }
      }
      if (!found && !att.label.empty()) {
        for (const Marker& mk : s.markers) {
          if (mk.name == att.label) {
            found = &mk;
            break;
          }
        }
      }
      if (found) {
        res.exists = true;
        res.title = found->name.empty() ? std::string("Marker") : found->name;
        res.detail = barLabel(s, found->pos);
      }
      break;
    }
    default: break;
  }

  if (res.title.empty()) res.title = chatRefKindBadge(att.kind);
  return res;
}

ChatAttachment makeClipAttachment(const Song& s, ObjectId clipId) {
  ChatAttachment a;
  const Clip* c = s.clip(clipId);
  if (!c) return a;
  a.kind = ChatRefKind::Clip;
  a.id = clipId;
  a.tick0 = c->start;
  const ChatRefResolution r = resolveChatAttachment(s, a);
  a.label = cleanChatLabel(r.title);
  a.detail = cleanChatLabel(r.detail);
  return a;
}

ChatAttachment makePatternAttachment(const Song& s, ObjectId patId) {
  ChatAttachment a;
  if (!s.pattern(patId)) return a;
  a.kind = ChatRefKind::Pattern;
  a.id = patId;
  const ChatRefResolution r = resolveChatAttachment(s, a);
  a.label = cleanChatLabel(r.title);
  a.detail = cleanChatLabel(r.detail);
  return a;
}

ChatAttachment makeChannelAttachment(const Song& s, ObjectId chId) {
  ChatAttachment a;
  if (!s.channel(chId)) return a;
  a.kind = ChatRefKind::Channel;
  a.id = chId;
  const ChatRefResolution r = resolveChatAttachment(s, a);
  a.label = cleanChatLabel(r.title);
  a.detail = cleanChatLabel(r.detail);
  return a;
}

ChatAttachment makePluginAttachment(const Song& s, ObjectId ownerId, int slot) {
  ChatAttachment a;
  a.kind = ChatRefKind::Plugin;
  a.id = ownerId;
  a.sub = slot;
  const ChatRefResolution r = resolveChatAttachment(s, a);
  if (!r.exists) return {};
  a.label = cleanChatLabel(r.title);
  a.detail = cleanChatLabel(r.detail);
  return a;
}

ChatAttachment makeParamAttachment(const Song& s, const ParamId& pid) {
  ChatAttachment a;
  if (!describeParam(s, pid)) return a;
  a.kind = ChatRefKind::Param;
  a.id = pid.owner;
  a.sub = static_cast<int>(pid.index);
  const ChatRefResolution r = resolveChatAttachment(s, a);
  a.label = cleanChatLabel(r.title);
  a.detail = cleanChatLabel(r.detail);
  return a;
}

ChatAttachment makeMixerTrackAttachment(const Song& s, ObjectId mixerId) {
  ChatAttachment a;
  if (s.mixerIndex(mixerId) < 0) return a;
  a.kind = ChatRefKind::MixerTrack;
  a.id = mixerId;
  const ChatRefResolution r = resolveChatAttachment(s, a);
  a.label = cleanChatLabel(r.title);
  a.detail = cleanChatLabel(r.detail);
  return a;
}

ChatAttachment makeNotesAttachment(const Song& s, ObjectId patId, ObjectId chId, const std::vector<size_t>& selectedIndices) {
  ChatAttachment a;
  const Pattern* p = s.pattern(patId);
  const Channel* ch = s.channel(chId);
  if (!p || !ch) return a;
  auto it = p->notes.find(chId);
  if (it == p->notes.end() || it->second.empty()) return a;
  const std::vector<Note>& ns = it->second;
  Tick t0 = 0, t1 = 0;
  int pLo = 127, pHi = 0, cnt = 0;
  auto take = [&](const Note& n) {
    const Tick e = n.start + std::max<Tick>(1, n.length);
    if (cnt == 0) {
      t0 = n.start;
      t1 = e;
    } else {
      t0 = std::min(t0, n.start);
      t1 = std::max(t1, e);
    }
    pLo = std::min(pLo, static_cast<int>(n.pitch));
    pHi = std::max(pHi, static_cast<int>(n.pitch));
    ++cnt;
  };
  if (!selectedIndices.empty()) {
    for (size_t idx : selectedIndices)
      if (idx < ns.size()) take(ns[idx]);
  } else {
    for (const Note& n : ns) take(n);
  }
  if (cnt == 0) return a;
  a.kind = ChatRefKind::Notes;
  a.id = patId;
  a.extra = chId;
  a.tick0 = t0;
  a.tick1 = t1;
  a.pitchLo = pLo;
  a.pitchHi = pHi;
  a.count = cnt;
  const ChatRefResolution r = resolveChatAttachment(s, a);
  a.label = cleanChatLabel(r.title);
  a.detail = cleanChatLabel(r.detail);
  return a;
}

ChatAttachment makeMarkerAttachment(const Song& s, size_t markerIndex) {
  ChatAttachment a;
  if (markerIndex >= s.markers.size()) return a;
  const Marker& mk = s.markers[markerIndex];
  a.kind = ChatRefKind::Marker;
  a.tick0 = mk.pos;
  a.label = cleanChatLabel(mk.name.empty() ? std::string("Marker") : mk.name);
  a.detail = cleanChatLabel(barLabel(s, mk.pos));
  return a;
}

}  // namespace gl
