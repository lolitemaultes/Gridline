// Gridline - J-111 collaboration: one encrypted, reliable link between two
// peers over unreliable datagrams (ICE / UDP).
//
// Handshake (Noise-like, a pre-shared key bound to the invite or to the two
// members' static keys, forward secret through ephemeral X25519):
//   I -> R  H1: e_I, AEAD_k1(I's hello)        k1 = KDF(psk, hsid, e_I, DH(e_I, S_R))
//   R -> I  H2: e_R, AEAD_k2(R's hello / no)   k2 = KDF(k1, e_R, DH(e_R, e_I), DH(e_R, S_I))
// Session keys and a safety code come from k2 and the transcript.
// Data: [3][counter u64][XChaCha20-Poly1305], replay window; inside, a small
// QUIC-like layer: packet numbers, ack + 64-bit SACK, RTT estimate,
// retransmission of lost stream data, AIMD congestion window, two ordered
// reliable streams (control first, then bulk) and unreliable datagrams.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../core/Json.h"
#include "Crypto.h"

namespace gl::collab {

inline constexpr size_t kMaxPacket = 1150;     // plaintext payload per datagram
inline constexpr uint64_t kIdleTimeoutMs = 15000;
inline constexpr uint64_t kKeepaliveMs = 2000;

class SecureLink {
 public:
  enum class State { Handshake, Open, Closed, Rejected };
  struct Hs {
    bool initiator = false;
    crypto::Key psk{};
    std::string hsid;            // 16 bytes: invite id or link id
    crypto::Identity me;
    crypto::Key remoteDh{};      // the responder's static key (initiator: always; responder: member links)
    bool remoteDhKnown = false;
    Json hello = Json::makeObject();  // what we tell the other side
    // Responder: decides on the initiator's hello; `ok` false refuses (the
    // returned JSON travels back with the refusal, e.g. a version notice).
    std::function<Json(const Json& theirHello, const crypto::Key& theirDh, bool& ok)> accept;
  };

  explicit SecureLink(Hs hs);
  // Once the datagram path works (ICE connected). Initiator: sends H1 and repeats
  // it until H2 (30 s); responder: answers H1 (repeating H2 until data arrives).
  // Does nothing on a link that already started or closed.
  void start(uint64_t nowMs);
  bool started() const { return started_; }
  bool heardData() const { return anyRecv_; }
  void input(const std::string& datagram, uint64_t nowMs);
  void tick(uint64_t nowMs);
  std::function<void(const std::string& datagram)> output;
  std::string tag;  // names the link in GRIDLINE_COLLAB_DEBUG lines

  // Streams 0 (control) and 1 (bulk) are reliable and ordered; 2 is unreliable.
  bool send(int stream, const std::string& message);
  size_t queued(int stream) const;
  void close(const std::string& reason);

  State state() const { return state_; }
  const Json& remoteHello() const { return remoteHello_; }
  const Json& refusal() const { return refusal_; }  // Rejected: the responder's reasons
  const std::string& safetyCode() const { return safety_; }
  crypto::Key remoteDhStatic() const { return remoteDh_; }
  double rttMs() const { return srtt_ < 0 ? -1 : srtt_; }
  uint64_t lastHeardMs() const { return lastRecv_; }
  // statistics (tests)
  uint64_t packetsSent() const { return pktSent_; }
  uint64_t packetsResent() const { return framesResent_; }
  size_t cwnd() const { return cwnd_; }

  std::function<void()> onOpen;
  std::function<void(int stream, const std::string& message)> onMessage;
  std::function<void(const std::string& reason)> onClose;

 private:
  void sendH1();
  void handleH1(const std::string& d, uint64_t now);
  void handleH2(const std::string& d, uint64_t now);
  void handleData(const std::string& d, uint64_t now);
  void deriveSession(const crypto::Key& k2, const std::string& transcript);
  void flush(uint64_t now);
  void onAck(uint32_t largest, uint64_t bits, uint64_t now);
  void deliverStream(int s);
  void fail(State st, const std::string& reason);

  Hs hs_;
  State state_ = State::Handshake;
  crypto::Key ePriv_{}, ePub_{};
  crypto::Key k1_{};
  std::string h1_, h2_;      // sent handshake packets (resent on loss)
  uint64_t lastH1_ = 0, lastH2_ = 0;
  int h1Tries_ = 0, h2Tries_ = 0;
  bool started_ = false;
  crypto::Key sendKey_{}, recvKey_{};
  uint8_t sendDir_ = 0, recvDir_ = 1;
  uint64_t sendCounter_ = 0;
  uint64_t recvMax_ = 0;
  uint64_t recvWindow_ = 0;  // bit i: recvMax_ - i seen
  bool anyRecv_ = false;
  Json remoteHello_, refusal_;
  std::string safety_;
  crypto::Key remoteDh_{};
  // reliability
  struct Stream {
    std::string buf;       // unacked + unsent bytes, starting at `base`
    uint64_t base = 0, next = 0;  // next = first byte never sent
    std::map<uint64_t, uint64_t> lost;   // ranges to resend: start -> end
    std::map<uint64_t, uint64_t> acked;  // acked ranges above base
    // receive
    uint64_t delivered = 0;
    std::map<uint64_t, std::string> segs;
    std::string pending;   // bytes delivered but not yet a whole message
  };
  Stream st_[2];
  std::deque<std::string> dgrams_;
  struct Sent {
    uint64_t time = 0;
    size_t bytes = 0;
    bool eliciting = false;
    std::vector<std::tuple<int, uint64_t, uint64_t>> frames;  // stream, start, end
  };
  std::map<uint32_t, Sent> inflight_;
  uint32_t nextPn_ = 1;
  size_t bytesInFlight_ = 0;
  size_t cwnd_ = 32 * 1024, ssthresh_ = 4u << 20;
  uint32_t lossRecoveryPn_ = 0;
  double srtt_ = -1, rttvar_ = 0;
  uint64_t rtoBackoff_ = 1;
  // acks to send
  uint32_t rcvLargest_ = 0;
  uint64_t rcvBits_ = 0;
  int unacked_ = 0;
  uint64_t ackDue_ = 0;
  uint64_t lastSend_ = 0, lastRecv_ = 0, opened_ = 0, now_ = 0;
  uint64_t pktSent_ = 0, framesResent_ = 0;
};

}  // namespace gl::collab
