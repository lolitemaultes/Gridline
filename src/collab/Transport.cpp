// Gridline - J-111 collaboration: secure reliable link. See Transport.h.
#include "Transport.h"
#include "Debug.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gl::collab {

namespace {

constexpr uint8_t kH1 = 1, kH2 = 2, kData = 3;
constexpr uint8_t kFAck = 1, kFStream = 2, kFDgram = 3, kFPing = 4, kFClose = 5;
constexpr size_t kMaxMessage = 64u << 20;
constexpr size_t kMinCwnd = 4 * 1024, kMaxCwnd = 4u << 20;

void put8(std::string& s, uint8_t v) { s.push_back(static_cast<char>(v)); }
void put16(std::string& s, uint16_t v) {
  s.push_back(static_cast<char>(v & 0xFF));
  s.push_back(static_cast<char>(v >> 8));
}
void put32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

struct Reader {
  const std::string& s;
  size_t at = 0;
  bool ok = true;
  explicit Reader(const std::string& x, size_t start = 0) : s(x), at(start) {}
  bool has(size_t n) const { return ok && at + n <= s.size(); }
  uint64_t get(int bytes) {
    if (!has(static_cast<size_t>(bytes))) {
      ok = false;
      return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(s[at + static_cast<size_t>(i)])) << (8 * i);
    at += static_cast<size_t>(bytes);
    return v;
  }
  std::string bytes(size_t n) {
    if (!has(n)) {
      ok = false;
      return {};
    }
    std::string r = s.substr(at, n);
    at += n;
    return r;
  }
};

std::string nonceZero() { return std::string(24, '\0'); }

}  // namespace

SecureLink::SecureLink(Hs hs) : hs_(std::move(hs)) {
  ePriv_ = crypto::randomKey();
  ePub_ = crypto::dhPublic(ePriv_);
  if (hs_.hsid.size() != 16) hs_.hsid.resize(16, '\0');
  if (hs_.remoteDhKnown) remoteDh_ = hs_.remoteDh;
}

void SecureLink::start(uint64_t nowMs) {
  // (J-111 fix: a link that already closed stays closed: an initiator whose ICE
  // connected late used to send one H1 from here after giving up, so the
  // responder admitted a joiner whose own side ignored the answer.)
  if (started_ || state_ != State::Handshake) return;
  started_ = true;
  now_ = nowMs;
  lastRecv_ = nowMs;
  if (hs_.initiator) sendH1();
}

void SecureLink::sendH1() {
  if (h1_.empty()) {
    const crypto::Key es = crypto::dh(ePriv_, hs_.remoteDh);
    k1_ = crypto::kdf(hs_.psk, "gridline-hs1", hs_.hsid + crypto::keyString(ePub_) + crypto::keyString(es));
    Json hello = hs_.hello;
    hello.object["dh"] = Json::makeString(crypto::toHex(hs_.me.dhPublic.data(), 32));
    std::string p;
    put8(p, kH1);
    p += hs_.hsid;
    p += crypto::keyString(ePub_);
    p += crypto::seal(k1_, nonceZero(), p, jsonCompact(hello));
    h1_ = std::move(p);
  }
  lastH1_ = now_;
  ++h1Tries_;
  if (h1Tries_ <= 3 || h1Tries_ % 10 == 0) debugf("%s H1 sent (try %d, %zu bytes)", tag.c_str(), h1Tries_, h1_.size());
  if (output) output(h1_);
}

void SecureLink::deriveSession(const crypto::Key& k2, const std::string& transcript) {
  const std::string th = crypto::keyString(crypto::hash(transcript));
  const crypto::Key i2r = crypto::kdf(k2, "gridline-i2r", th), r2i = crypto::kdf(k2, "gridline-r2i", th);
  sendKey_ = hs_.initiator ? i2r : r2i;
  recvKey_ = hs_.initiator ? r2i : i2r;
  sendDir_ = hs_.initiator ? 0 : 1;
  recvDir_ = hs_.initiator ? 1 : 0;
  const crypto::Key sc = crypto::kdf(k2, "gridline-safety", th);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | sc[static_cast<size_t>(i)];
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04u %04u %04u %04u", static_cast<unsigned>(v % 10000), static_cast<unsigned>((v / 10000) % 10000),
                static_cast<unsigned>((v / 100000000) % 10000), static_cast<unsigned>((v / 1000000000000ull) % 10000));
  safety_ = buf;
}

void SecureLink::handleH1(const std::string& d, uint64_t now) {
  if (hs_.initiator) return;
  if (state_ == State::Open || state_ == State::Rejected) {
    if (d == h1_ && !h2_.empty() && output) {  // our H2 was lost: send it again
      lastRecv_ = now;  // the initiator is still there (it has not heard our H2 yet)
      debugf("%s H1 again: H2 resent", tag.c_str());
      output(h2_);
    } else {
      debugf("%s H1 ignored (link %s, %s)", tag.c_str(), state_ == State::Open ? "open" : "refused", d == h1_ ? "same" : "another handshake");
    }
    return;
  }
  Reader r(d, 1);
  const std::string hsid = r.bytes(16);
  const std::string eI = r.bytes(32);
  if (!r.ok || hsid != hs_.hsid) return;
  crypto::Key eIk{};
  crypto::keyFrom(eI, eIk);
  const crypto::Key es = crypto::dh(hs_.me.dhSecret, eIk);
  const crypto::Key k1 = crypto::kdf(hs_.psk, "gridline-hs1", hsid + eI + crypto::keyString(es));
  std::string plain;
  if (!crypto::open(k1, nonceZero(), d.substr(0, 49), d.substr(49), plain)) {  // wrong invite / key: silence
    debugf("%s H1 does not decrypt (another invite or key)", tag.c_str());
    return;
  }
  Json hello;
  if (!jsonParse(plain, hello) || !hello.is(Json::Type::Object)) return;
  std::string dhb;
  crypto::Key sI{};
  if (!crypto::fromHex(hello.text("dh"), dhb) || !crypto::keyFrom(dhb, sI)) return;
  if (hs_.remoteDhKnown && sI != hs_.remoteDh) {  // not the member this link is for
    debugf("%s H1 from another member's key: ignored", tag.c_str());
    return;
  }
  bool ok = true;
  Json reply = hs_.accept ? hs_.accept(hello, sI, ok) : hs_.hello;
  if (!reply.is(Json::Type::Object)) reply = Json::makeObject();
  if (!ok && reply.num("silent", 0) != 0) {  // J-146: no answer at all (a presence probe while hidden); the link keeps waiting
    debugf("%s H1 left unanswered", tag.c_str());
    return;
  }
  reply.object["ok"] = Json::makeNumber(ok ? 1 : 0);
  const crypto::Key ee = crypto::dh(ePriv_, eIk), se = crypto::dh(ePriv_, sI);
  const crypto::Key k2 = crypto::kdf(k1, "gridline-hs2", crypto::keyString(ePub_) + crypto::keyString(ee) + crypto::keyString(se));
  std::string p;
  put8(p, kH2);
  p += crypto::keyString(ePub_);
  const std::string ad = p + crypto::keyString(crypto::hash(d));
  p += crypto::seal(k2, nonceZero(), ad, jsonCompact(reply));
  h1_ = d;
  h2_ = p;
  lastH2_ = now;
  h2Tries_ = 1;
  remoteHello_ = hello;
  remoteDh_ = sI;
  lastRecv_ = now;
  debugf("%s H1 received, H2 sent (%zu bytes, %s)", tag.c_str(), h2_.size(), ok ? "accepted" : "refused");
  if (output) output(h2_);
  if (!ok) {
    refusal_ = reply;
    state_ = State::Rejected;
    if (onClose) onClose("refused");
    return;
  }
  deriveSession(k2, d + h2_);
  state_ = State::Open;
  opened_ = now;
  if (onOpen) onOpen();
}

void SecureLink::handleH2(const std::string& d, uint64_t now) {
  if (!hs_.initiator || state_ != State::Handshake || h1_.empty()) {
    if (hs_.initiator) debugf("%s H2 ignored (%s)", tag.c_str(), state_ == State::Open ? "already open" : "no H1 sent");
    return;
  }
  Reader r(d, 1);
  const std::string eR = r.bytes(32);
  if (!r.ok) return;
  crypto::Key eRk{};
  crypto::keyFrom(eR, eRk);
  const crypto::Key ee = crypto::dh(ePriv_, eRk), se = crypto::dh(hs_.me.dhSecret, eRk);
  const crypto::Key k2 = crypto::kdf(k1_, "gridline-hs2", eR + crypto::keyString(ee) + crypto::keyString(se));
  const std::string ad = d.substr(0, 33) + crypto::keyString(crypto::hash(h1_));
  std::string plain;
  if (!crypto::open(k2, nonceZero(), ad, d.substr(33), plain)) {
    debugf("%s H2 does not decrypt", tag.c_str());
    return;
  }
  Json reply;
  if (!jsonParse(plain, reply) || !reply.is(Json::Type::Object)) return;
  remoteHello_ = reply;
  remoteDh_ = hs_.remoteDh;
  lastRecv_ = now;
  debugf("%s H2 received (%s)", tag.c_str(), reply.num("ok", 0) == 0 ? "refused" : "accepted");
  if (reply.num("ok", 0) == 0) {
    refusal_ = reply;
    state_ = State::Rejected;
    if (onClose) onClose("refused");
    return;
  }
  h2_ = d;
  deriveSession(k2, h1_ + h2_);
  state_ = State::Open;
  opened_ = now;
  if (onOpen) onOpen();
  flush(now);
}

void SecureLink::input(const std::string& d, uint64_t nowMs) {
  now_ = nowMs;
  if (d.empty() || state_ == State::Closed) return;
  switch (static_cast<uint8_t>(d[0])) {
    case kH1: handleH1(d, nowMs); break;
    case kH2: handleH2(d, nowMs); break;
    case kData: handleData(d, nowMs); break;
    default: break;
  }
}

void SecureLink::handleData(const std::string& d, uint64_t now) {
  if (state_ != State::Open || d.size() < 9 + 16) return;
  Reader hdr(d, 1);
  const uint64_t counter = hdr.get(8);
  // replay window
  if (anyRecv_) {
    if (counter <= recvMax_) {
      const uint64_t back = recvMax_ - counter;
      if (back >= 64 || ((recvWindow_ >> back) & 1) != 0) return;
    }
  }
  std::string plain;
  if (!crypto::open(recvKey_, crypto::counterNonce(counter, recvDir_), d.substr(0, 9), d.substr(9), plain)) return;
  if (!anyRecv_ || counter > recvMax_) {
    const uint64_t shift = anyRecv_ ? counter - recvMax_ : 64;
    recvWindow_ = shift >= 64 ? 1 : (recvWindow_ << shift) | 1;
    recvMax_ = counter;
    anyRecv_ = true;
  } else {
    recvWindow_ |= 1ull << (recvMax_ - counter);
  }
  lastRecv_ = now;
  Reader r(plain);
  const uint32_t pn = static_cast<uint32_t>(r.get(4));
  if (!r.ok) return;
  bool eliciting = false;
  while (r.ok && r.at < plain.size()) {
    const uint8_t type = static_cast<uint8_t>(r.get(1));
    if (type == kFAck) {
      const uint32_t largest = static_cast<uint32_t>(r.get(4));
      const uint64_t bits = r.get(8);
      if (r.ok) onAck(largest, bits, now);
    } else if (type == kFStream) {
      const int s = static_cast<int>(r.get(1));
      const uint64_t off = r.get(8);
      const size_t len = static_cast<size_t>(r.get(2));
      std::string data = r.bytes(len);
      if (!r.ok || s < 0 || s > 1) break;
      eliciting = true;
      Stream& st = st_[s];
      if (off + len > st.delivered) {
        if (off < st.delivered) {
          data = data.substr(static_cast<size_t>(st.delivered - off));
          st.segs[st.delivered] = std::move(data);
        } else {
          auto it = st.segs.find(off);
          if (it == st.segs.end() || it->second.size() < data.size()) st.segs[off] = std::move(data);
        }
        deliverStream(s);
      }
    } else if (type == kFDgram) {
      const size_t len = static_cast<size_t>(r.get(2));
      std::string data = r.bytes(len);
      if (!r.ok) break;
      eliciting = true;
      if (onMessage) onMessage(2, data);
    } else if (type == kFPing) {
      eliciting = true;
    } else if (type == kFClose) {
      const size_t len = static_cast<size_t>(r.get(2));
      const std::string why = r.bytes(len);
      state_ = State::Closed;
      debugf("%s closed by the other side: %s", tag.c_str(), why.c_str());
      if (onClose) onClose(why.empty() ? "closed" : why);
      return;
    } else {
      break;
    }
  }
  // record for our ack
  if (pn > rcvLargest_) {
    const uint32_t shift = pn - rcvLargest_;
    rcvBits_ = rcvLargest_ == 0 ? 0 : (shift >= 64 ? 0 : (rcvBits_ << shift) | (1ull << (shift - 1)));
    rcvLargest_ = pn;
  } else if (pn < rcvLargest_ && rcvLargest_ - pn <= 64) {
    rcvBits_ |= 1ull << (rcvLargest_ - pn - 1);
  }
  if (eliciting) {
    ++unacked_;
    if (ackDue_ == 0) ackDue_ = now + 20;
    if (unacked_ >= 2) ackDue_ = now;
  }
  flush(now);
}

void SecureLink::deliverStream(int s) {
  Stream& st = st_[s];
  for (;;) {
    auto it = st.segs.begin();
    if (it == st.segs.end() || it->first > st.delivered) break;
    const uint64_t end = it->first + it->second.size();
    if (end > st.delivered) st.pending += it->second.substr(static_cast<size_t>(st.delivered - it->first));
    st.delivered = std::max(st.delivered, end);
    st.segs.erase(it);
  }
  size_t at = 0;
  while (st.pending.size() - at >= 4) {
    Reader r(st.pending, at);
    const size_t len = static_cast<size_t>(r.get(4));
    if (len > kMaxMessage) {
      close("message too large");
      return;
    }
    if (st.pending.size() - at - 4 < len) break;
    std::string msg = st.pending.substr(at + 4, len);
    at += 4 + len;
    if (onMessage) onMessage(s, msg);
    if (state_ != State::Open) return;
  }
  if (at) st.pending.erase(0, at);
}

void SecureLink::onAck(uint32_t largest, uint64_t bits, uint64_t now) {
  bool newly = false;
  for (auto it = inflight_.begin(); it != inflight_.end();) {
    const uint32_t pn = it->first;
    if (pn > largest) break;
    const bool acked = pn == largest || (largest - pn - 1 < 64 && ((bits >> (largest - pn - 1)) & 1));
    if (!acked) {
      ++it;
      continue;
    }
    if (pn == largest) {
      const double sample = static_cast<double>(now - it->second.time);
      if (srtt_ < 0) {
        srtt_ = sample;
        rttvar_ = sample / 2;
      } else {
        rttvar_ = 0.75 * rttvar_ + 0.25 * std::fabs(srtt_ - sample);
        srtt_ = 0.875 * srtt_ + 0.125 * sample;
      }
    }
    for (const auto& [s, a, b] : it->second.frames) {
      Stream& st = st_[s];
      if (b <= st.base) continue;
      st.acked[std::max(a, st.base)] = std::max(st.acked[std::max(a, st.base)], b);
    }
    bytesInFlight_ -= std::min(bytesInFlight_, it->second.bytes);
    if (cwnd_ < ssthresh_) cwnd_ += it->second.bytes;
    else cwnd_ += kMaxPacket * it->second.bytes / std::max<size_t>(cwnd_, 1);
    cwnd_ = std::min(cwnd_, kMaxCwnd);
    it = inflight_.erase(it);
    newly = true;
  }
  if (!newly) return;
  rtoBackoff_ = 1;
  // advance stream bases over contiguous acked bytes
  for (auto& st : st_) {
    // merge acked ranges from base
    bool moved = true;
    while (moved) {
      moved = false;
      for (auto it = st.acked.begin(); it != st.acked.end();) {
        if (it->first <= st.base) {
          if (it->second > st.base) {
            const uint64_t drop = it->second - st.base;
            st.buf.erase(0, static_cast<size_t>(std::min<uint64_t>(drop, st.buf.size())));
            st.base = it->second;
            moved = true;
          }
          it = st.acked.erase(it);
        } else {
          break;
        }
      }
    }
    for (auto it = st.lost.begin(); it != st.lost.end();) it = it->second <= st.base ? st.lost.erase(it) : std::next(it);
  }
  // loss: packets 3 below the largest acked, or older than 1.25 RTT while later ones arrived
  const double age = srtt_ < 0 ? 1000.0 : srtt_ * 1.25 + 5;
  bool loss = false;
  for (auto it = inflight_.begin(); it != inflight_.end();) {
    if (it->first < largest && (largest - it->first >= 3 || static_cast<double>(now - it->second.time) > age)) {
      for (const auto& [s, a, b] : it->second.frames) {
        Stream& st = st_[s];
        if (b > st.base) st.lost[std::max(a, st.base)] = std::max(st.lost[std::max(a, st.base)], b);
      }
      if (it->first > lossRecoveryPn_) loss = true;
      bytesInFlight_ -= std::min(bytesInFlight_, it->second.bytes);
      it = inflight_.erase(it);
    } else {
      ++it;
    }
  }
  if (loss) {
    ssthresh_ = std::max(cwnd_ / 2, kMinCwnd);
    cwnd_ = ssthresh_;
    lossRecoveryPn_ = nextPn_ - 1;
  }
}

bool SecureLink::send(int stream, const std::string& message) {
  if (state_ == State::Closed || state_ == State::Rejected) return false;
  if (stream == 2) {
    if (message.size() > kMaxPacket - 40) return false;
    if (dgrams_.size() > 64) dgrams_.pop_front();
    dgrams_.push_back(message);
  } else if (stream == 0 || stream == 1) {
    if (message.size() > kMaxMessage) return false;
    std::string& b = st_[stream].buf;
    put32(b, static_cast<uint32_t>(message.size()));
    b += message;
  } else {
    return false;
  }
  if (state_ == State::Open) flush(now_);
  return true;
}

size_t SecureLink::queued(int stream) const {
  if (stream == 2) return dgrams_.size();
  if (stream < 0 || stream > 1) return 0;
  return st_[stream].buf.size();
}

void SecureLink::close(const std::string& reason) {
  if (state_ == State::Open) {
    std::string p;
    put32(p, nextPn_++);
    put8(p, kFClose);
    put16(p, static_cast<uint16_t>(std::min<size_t>(reason.size(), 200)));
    p += reason.substr(0, 200);
    std::string pkt;
    put8(pkt, kData);
    put64(pkt, sendCounter_);
    pkt += crypto::seal(sendKey_, crypto::counterNonce(sendCounter_, sendDir_), pkt, p);
    ++sendCounter_;
    if (output) output(pkt);
  }
  if (state_ != State::Closed) {
    state_ = State::Closed;
    if (onClose) onClose(reason);
  }
}

void SecureLink::fail(State st, const std::string& reason) {
  debugf("%s closed: %s (last heard %llu ms ago, %llu packets sent, %llu resent)", tag.c_str(), reason.c_str(),
         static_cast<unsigned long long>(now_ - lastRecv_), static_cast<unsigned long long>(pktSent_), static_cast<unsigned long long>(framesResent_));
  state_ = st;
  if (onClose) onClose(reason);
}

void SecureLink::flush(uint64_t now) {
  if (state_ != State::Open || !output) return;
  for (int guard = 0; guard < 4096; ++guard) {
    std::string p;
    const uint32_t pn = nextPn_;
    put32(p, pn);
    bool eliciting = false;
    const bool ackNow = rcvLargest_ > 0 && (ackDue_ != 0 && ackDue_ <= now);
    bool anyAck = false;
    auto addAck = [&] {
      if (rcvLargest_ == 0) return;
      put8(p, kFAck);
      put32(p, rcvLargest_);
      put64(p, rcvBits_);
      anyAck = true;
    };
    std::vector<std::tuple<int, uint64_t, uint64_t>> frames;
    // datagrams (presence): latest first, never retransmitted
    while (!dgrams_.empty() && p.size() + 3 + dgrams_.front().size() <= kMaxPacket - 40) {
      put8(p, kFDgram);
      put16(p, static_cast<uint16_t>(dgrams_.front().size()));
      p += dgrams_.front();
      dgrams_.pop_front();
      eliciting = true;
    }
    // stream data (control first), within the congestion window
    for (int s = 0; s < 2; ++s) {
      Stream& st = st_[s];
      while (bytesInFlight_ + p.size() + 64 < cwnd_ || (bytesInFlight_ == 0 && p.size() < 64)) {
        const size_t room = kMaxPacket - 40 > p.size() + 12 ? kMaxPacket - 40 - p.size() - 12 : 0;
        if (room < 32) break;
        uint64_t a = 0, b = 0;
        bool resend = false;
        if (!st.lost.empty()) {
          auto it = st.lost.begin();
          a = std::max(it->first, st.base);
          b = std::min(it->second, a + room);
          if (b >= it->second) st.lost.erase(it);
          else {
            const uint64_t end = it->second;
            st.lost.erase(it);
            st.lost[b] = end;
          }
          if (a >= b) continue;
          resend = true;
        } else if (st.next < st.base + st.buf.size()) {
          a = std::max(st.next, st.base);
          b = std::min<uint64_t>(st.base + st.buf.size(), a + room);
          st.next = b;
        } else {
          break;
        }
        put8(p, kFStream);
        put8(p, static_cast<uint8_t>(s));
        put64(p, a);
        put16(p, static_cast<uint16_t>(b - a));
        p += st.buf.substr(static_cast<size_t>(a - st.base), static_cast<size_t>(b - a));
        frames.emplace_back(s, a, b);
        eliciting = true;
        if (resend) ++framesResent_;
      }
    }
    if (!eliciting && opened_ && now - lastSend_ >= kKeepaliveMs) {
      put8(p, kFPing);
      eliciting = true;
    }
    if (eliciting || ackNow) addAck();
    if (!eliciting && !anyAck) return;
    std::string pkt;
    put8(pkt, kData);
    put64(pkt, sendCounter_);
    pkt += crypto::seal(sendKey_, crypto::counterNonce(sendCounter_, sendDir_), pkt, p);
    ++sendCounter_;
    ++nextPn_;
    ++pktSent_;
    lastSend_ = now;
    if (anyAck) {
      unacked_ = 0;
      ackDue_ = 0;
    }
    if (eliciting) {
      Sent sent;
      sent.time = now;
      sent.bytes = pkt.size();
      sent.eliciting = true;
      sent.frames = std::move(frames);
      bytesInFlight_ += pkt.size();
      inflight_[pn] = std::move(sent);
    }
    output(pkt);
    if (!eliciting) return;
  }
}

void SecureLink::tick(uint64_t nowMs) {
  now_ = nowMs;
  if (state_ == State::Handshake) {
    // H1 is repeated only once started (ICE connected): the budget is 30 s of
    // reachable network, not time spent waiting for ICE.
    if (hs_.initiator && started_ && nowMs - lastH1_ >= 500) {
      if (h1Tries_ >= 60) {
        fail(State::Closed, "no answer");
        return;
      }
      sendH1();
    }
    return;
  }
  // Responder: the first H2 often goes out before ICE on this side is
  // connected (and is dropped there). Repeat it until the initiator's first
  // data packet shows that it arrived, without waiting for a repeated H1.
  if (!hs_.initiator && !h2_.empty() && !anyRecv_ && h2Tries_ < 40 && nowMs - lastH2_ >= 500 && output) {
    lastH2_ = nowMs;
    ++h2Tries_;
    if (h2Tries_ <= 3 || h2Tries_ % 10 == 0) debugf("%s H2 repeated (try %d)", tag.c_str(), h2Tries_);
    output(h2_);
  }
  if (state_ != State::Open) return;
  if (nowMs - lastRecv_ > kIdleTimeoutMs) {
    fail(State::Closed, "timeout");
    return;
  }
  // retransmission timeout: everything in flight is considered lost
  if (!inflight_.empty()) {
    const double rto = std::max(200.0, srtt_ < 0 ? 1000.0 : srtt_ + 4 * rttvar_) * static_cast<double>(rtoBackoff_);
    if (static_cast<double>(nowMs - inflight_.begin()->second.time) > rto) {
      for (auto& [pn, sent] : inflight_)
        for (const auto& [s, a, b] : sent.frames) {
          Stream& st = st_[s];
          if (b > st.base) st.lost[std::max(a, st.base)] = std::max(st.lost[std::max(a, st.base)], b);
        }
      inflight_.clear();
      bytesInFlight_ = 0;
      ssthresh_ = std::max(cwnd_ / 2, kMinCwnd);
      cwnd_ = kMinCwnd * 2;
      rtoBackoff_ = std::min<uint64_t>(rtoBackoff_ * 2, 16);
    }
  }
  flush(nowMs);
}

}  // namespace gl::collab
