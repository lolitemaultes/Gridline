#include "MidiSync.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/qos.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <semaphore.h>
#include <time.h>
#endif

namespace gl {

// ---------------------------------------------------------------- K-142: the sync sender

#if defined(__APPLE__)
WakeSignal::WakeSignal() : h_(dispatch_semaphore_create(0)) {}
WakeSignal::~WakeSignal() { dispatch_release(static_cast<dispatch_semaphore_t>(h_)); }
void WakeSignal::post() noexcept { dispatch_semaphore_signal(static_cast<dispatch_semaphore_t>(h_)); }
void WakeSignal::wait() { dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(h_), DISPATCH_TIME_FOREVER); }
bool WakeSignal::waitFor(double seconds) {
  const int64_t ns = seconds <= 0 ? 0 : static_cast<int64_t>(seconds * 1e9);
  return dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(h_), dispatch_time(DISPATCH_TIME_NOW, ns)) == 0;
}
#elif defined(_WIN32)
WakeSignal::WakeSignal() : h_(CreateSemaphoreW(nullptr, 0, 0x7fffffff, nullptr)) {}
WakeSignal::~WakeSignal() { CloseHandle(static_cast<HANDLE>(h_)); }
void WakeSignal::post() noexcept { ReleaseSemaphore(static_cast<HANDLE>(h_), 1, nullptr); }
void WakeSignal::wait() { WaitForSingleObject(static_cast<HANDLE>(h_), INFINITE); }
bool WakeSignal::waitFor(double seconds) {
  const DWORD ms = seconds <= 0 ? 0 : static_cast<DWORD>(std::ceil(seconds * 1000.0));
  return WaitForSingleObject(static_cast<HANDLE>(h_), ms) == WAIT_OBJECT_0;
}
#else
WakeSignal::WakeSignal() : h_(new sem_t) { sem_init(static_cast<sem_t*>(h_), 0, 0); }
WakeSignal::~WakeSignal() {
  sem_destroy(static_cast<sem_t*>(h_));
  delete static_cast<sem_t*>(h_);
}
void WakeSignal::post() noexcept { sem_post(static_cast<sem_t*>(h_)); }
void WakeSignal::wait() {
  while (sem_wait(static_cast<sem_t*>(h_)) != 0 && errno == EINTR) {
  }
}
bool WakeSignal::waitFor(double seconds) {
  sem_t* s = static_cast<sem_t*>(h_);
  if (seconds <= 0) return sem_trywait(s) == 0;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
  const clockid_t clk = CLOCK_MONOTONIC;  // (a wall clock set back never stretches the wait)
#else
  const clockid_t clk = CLOCK_REALTIME;
#endif
  timespec ts{};
  clock_gettime(clk, &ts);
  const int64_t ns = static_cast<int64_t>(ts.tv_nsec) + static_cast<int64_t>(seconds * 1e9);
  ts.tv_sec += static_cast<time_t>(ns / 1000000000);
  ts.tv_nsec = static_cast<long>(ns % 1000000000);
  for (;;) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
    const int r = sem_clockwait(s, clk, &ts);
#else
    const int r = sem_timedwait(s, &ts);
#endif
    if (r == 0) return true;
    if (errno != EINTR) return false;
  }
}
#endif

bool makeSyncSenderRealtime() {
  if (const char* off = std::getenv("GRIDLINE_NO_REALTIME"); off && *off && *off != '0') return false;
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  const double ticksPerMs = 1e6 * static_cast<double>(tb.denom) / static_cast<double>(tb.numer);
  thread_time_constraint_policy_data_t pol;
  pol.period = 0;                                             // it wakes when a message is due
  pol.computation = static_cast<uint32_t>(0.5 * ticksPerMs);  // sending a message takes microseconds
  pol.constraint = static_cast<uint32_t>(1.0 * ticksPerMs);
  pol.preemptible = 1;
  return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&pol),
                           THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
  return false;
#endif
}

void prepareSyncSender(SyncSenderHooks& h) {
#if defined(__APPLE__)
  h.coarseTimers = !makeSyncSenderRealtime();  // its timed waits end on time
#elif defined(__FreeBSD__)
  h.coarseTimers = true;  // a timed wait may end up to 5 % late (kern.timecounter.alloweddeviation)
#elif defined(__OpenBSD__) || defined(__NetBSD__)
  h.tickTimers = true;
#else
  (void)h;
#endif
}

void runSyncSender(MidiEventRing& ring, WakeSignal& wake, const std::atomic<bool>& quit, const SyncSenderHooks& h) {
  MidiOutEvent e;
  while (!quit.load()) {
    double wait = -1;  // < 0: until something is pushed
    while (ring.peek(e)) {
      const double due = h.due(e), now = h.now();
      if (due > now + kSyncSendEarly && due < now + 1.0) {  // not yet: sleep until just before it
        wait = due - now - kSyncSendEarly * 0.5;
        break;
      }
      ring.pop(e);
      h.send(e);
    }
    if (quit.load()) break;
    if (wait < 0) ring.sleepUntilPushed(wake);
    else if (h.tickTimers) wake.waitFor(std::min(wait, 0.0005));
    else wake.waitFor(h.coarseTimers && wait > 0.001 ? wait / 3.0 : wait);
    if (h.woke) h.woke();
  }
}

const char* syncOutName(SyncOut s) {
  static const char* n[] = {"Off", "MIDI clock", "MTC 24 fps", "MTC 25 fps", "MTC 29.97 fps (drop)", "MTC 30 fps"};
  int i = static_cast<int>(s);
  return i >= 0 && i < static_cast<int>(SyncOut::Count) ? n[i] : "?";
}

double mtcFrameRate(SyncOut s) {
  switch (s) {
    case SyncOut::Mtc24: return 24.0;
    case SyncOut::Mtc25: return 25.0;
    case SyncOut::Mtc2997Drop: return 30000.0 / 1001.0;
    default: return 30.0;
  }
}
int mtcNominalFps(SyncOut s) { return s == SyncOut::Mtc24 ? 24 : s == SyncOut::Mtc25 ? 25 : 30; }
int mtcRateCode(SyncOut s) { return s == SyncOut::Mtc24 ? 0 : s == SyncOut::Mtc25 ? 1 : s == SyncOut::Mtc2997Drop ? 2 : 3; }

Timecode mtcTimecode(SyncOut s, int64_t frame) {
  if (frame < 0) frame = 0;
  const int fps = mtcNominalFps(s);
  if (s == SyncOut::Mtc2997Drop) {
    // Drop-frame labels skip frame numbers 0 and 1 at the start of every
    // minute except each tenth minute.
    const int64_t per10 = 17982, perMin = 1798;
    int64_t d = frame / per10, m = frame % per10;
    frame += 18 * d + (m > 1 ? 2 * ((m - 2) / perMin) : 0);
  }
  Timecode t;
  t.f = static_cast<int>(frame % fps);
  int64_t secs = frame / fps;
  t.s = static_cast<int>(secs % 60);
  t.m = static_cast<int>((secs / 60) % 60);
  t.h = static_cast<int>((secs / 3600) % 24);
  return t;
}

void MidiClockOut::emit(int64_t at, uint8_t a, int size, uint8_t b, uint8_t c) {
  if (!out) return;
  MidiOutEvent e;
  e.sample = at;
  e.size = static_cast<uint8_t>(size);
  e.data[0] = a;
  e.data[1] = b;
  e.data[2] = c;
  if (!out->push(e)) ++dropped_;
}

void MidiClockOut::start(double tick, double seconds, int64_t at) {
  (void)at;
  if (mode_ == SyncOut::Off) return;
  running_ = true;
  pendingStart_ = true;
  startTick_ = tick;
  startSec_ = seconds;
  nextTick_ = tick;
}

void MidiClockOut::stop(int64_t at) {
  if (!running_) return;
  running_ = false;
  pendingStart_ = false;
  if (mode_ == SyncOut::Clock) emit(at, 0xFC);
}

void MidiClockOut::announce(double tick, double sec, int64_t at, bool fresh) {
  if (mode_ == SyncOut::Clock) {
    if (fresh && tick < 1e-9) {
      emit(at, 0xFA);  // start from the top
      return;
    }
    // Song position pointer counts 16th notes (6 clocks each).
    int spp = static_cast<int>(std::floor(tick / (ppq_ / 4.0) + 1e-9));
    spp = std::clamp(spp, 0, 16383);
    if (!fresh) emit(at, 0xFC);
    emit(at, 0xF2, 3, static_cast<uint8_t>(spp & 0x7F), static_cast<uint8_t>((spp >> 7) & 0x7F));
    emit(at, 0xFB);
  } else {
    fullFrame(sec, at);
  }
}

void MidiClockOut::quarterFrame(int64_t at, int64_t j) {
  const int piece = static_cast<int>(j % 8);
  const Timecode t = mtcTimecode(mode_, (j - piece) / 4);
  int nib = 0;
  switch (piece) {
    case 0: nib = t.f & 0xF; break;
    case 1: nib = (t.f >> 4) & 0x1; break;
    case 2: nib = t.s & 0xF; break;
    case 3: nib = (t.s >> 4) & 0x3; break;
    case 4: nib = t.m & 0xF; break;
    case 5: nib = (t.m >> 4) & 0x3; break;
    case 6: nib = t.h & 0xF; break;
    default: nib = ((t.h >> 4) & 0x1) | (mtcRateCode(mode_) << 1); break;
  }
  emit(at, 0xF1, 2, static_cast<uint8_t>((piece << 4) | nib));
}

void MidiClockOut::fullFrame(double sec, int64_t at) {
  if (!out) return;
  const Timecode t = mtcTimecode(mode_, static_cast<int64_t>(std::floor(std::max(0.0, sec) * mtcFrameRate(mode_) + 1e-9)));
  MidiOutEvent e;
  e.sample = at;
  e.size = 10;
  const uint8_t d[10] = {0xF0, 0x7F, 0x7F, 0x01, 0x01, static_cast<uint8_t>(t.h | (mtcRateCode(mode_) << 5)),
                         static_cast<uint8_t>(t.m), static_cast<uint8_t>(t.s), static_cast<uint8_t>(t.f), 0xF7};
  std::memcpy(e.data, d, 10);
  if (!out->push(e)) ++dropped_;
}

// ---------------------------------------------------------------- follower ----

void MidiClockFollower::reset() {
  count_ = head_ = 0;
  bpm_ = 0;
  running_ = false;
  clocks_ = 0;
  cmds_.clear();
}

void MidiClockFollower::message(uint8_t status, uint8_t d1, uint8_t d2, double t) {
  switch (status) {
    case 0xF8: {
      if (count_ > 0) {
        double last = times_[(head_ + kWindow) % (kWindow + 1)];
        if (t - last > 0.5 || t < last) count_ = 0;  // a gap: the clock stopped
      }
      times_[head_] = t;
      head_ = (head_ + 1) % (kWindow + 1);
      count_ = std::min(count_ + 1, kWindow + 1);
      if (count_ >= 7) {
        double newest = times_[(head_ + kWindow) % (kWindow + 1)];
        double oldest = times_[(head_ + kWindow + 1 - count_) % (kWindow + 1)];
        double interval = (newest - oldest) / (count_ - 1);
        if (interval > 1e-6) bpm_ = 60.0 / (interval * 24.0);
      }
      if (running_) ++clocks_;
      break;
    }
    case 0xFA:
      running_ = true;
      clocks_ = 0;
      cmds_.push_back({Cmd::Start, 0});
      break;
    case 0xFB:
      running_ = true;
      cmds_.push_back({Cmd::Continue, 0});
      break;
    case 0xFC:
      running_ = false;
      cmds_.push_back({Cmd::Stop, 0});
      break;
    case 0xF2: {
      double s = static_cast<double>((d1 & 0x7F) | ((d2 & 0x7F) << 7));
      clocks_ = static_cast<int64_t>(s) * 6;
      cmds_.push_back({Cmd::Locate, s});
      break;
    }
    default: break;
  }
  if (cmds_.size() > 256) cmds_.erase(cmds_.begin(), cmds_.begin() + 128);
}

std::vector<MidiClockFollower::Cmd> MidiClockFollower::takeCommands() {
  std::vector<Cmd> out;
  out.swap(cmds_);
  return out;
}

// ---------------------------------------------------------------- receiving (MI-02)

const char* syncSourceName(SyncSource s) {
  static const char* n[] = {"Internal", "MIDI clock", "MIDI time code"};
  const int i = static_cast<int>(s);
  return i >= 0 && i < static_cast<int>(SyncSource::Count) ? n[i] : "?";
}

SyncOut mtcRateFromCode(int code) {
  switch (code & 3) {
    case 0: return SyncOut::Mtc24;
    case 1: return SyncOut::Mtc25;
    case 2: return SyncOut::Mtc2997Drop;
    default: return SyncOut::Mtc30;
  }
}

int64_t mtcFrameOf(SyncOut rate, const Timecode& t) {
  const int fps = mtcNominalFps(rate);
  int64_t f = ((static_cast<int64_t>(t.h) * 60 + t.m) * 60 + t.s) * fps + t.f;
  if (rate == SyncOut::Mtc2997Drop) {  // two labels dropped every minute except each tenth
    const int64_t minutes = static_cast<int64_t>(t.h) * 60 + t.m;
    f -= 2 * (minutes - minutes / 10);
  }
  return std::max<int64_t>(0, f);
}

void MtcReceiver::reset() { *this = MtcReceiver{}; }

bool MtcReceiver::message(const uint8_t* m, size_t n, double t) {
  if (n >= 2 && m[0] == 0xF1) {
    const int piece = (m[1] >> 4) & 7, v = m[1] & 0xF;
    ++qfCount_;
    if (t - lastQf_ > 0.25) {  // a pause: the master stopped; resync
      seqRun_ = 0;
      assembled_ = false;
      expect_ = -1;
    }
    lastQf_ = t;
    if (expect_ >= 0 && piece != expect_) {  // out of sequence (lost message, reverse play)
      ++dropouts_;
      seqRun_ = 0;
      assembled_ = false;
      expect_ = -1;
    }
    if (expect_ < 0 && piece != 0) return true;  // wait for the start of a timecode
    nib_[piece] = static_cast<uint8_t>(v);
    expect_ = (piece + 1) & 7;
    ++seqRun_;
    if (piece == 7) {
      // Piece k of the label for frame F is sent at F + k/4 frames.
      tc_.f = nib_[0] | ((nib_[1] & 1) << 4);
      tc_.s = nib_[2] | ((nib_[3] & 3) << 4);
      tc_.m = nib_[4] | ((nib_[5] & 3) << 4);
      tc_.h = nib_[6] | ((nib_[7] & 1) << 4);
      rate_ = mtcRateFromCode((nib_[7] >> 1) & 3);
      anchorSec_ = (static_cast<double>(mtcFrameOf(rate_, tc_)) + 1.75) / mtcFrameRate(rate_);
      anchorTime_ = t;
      assembled_ = hasPos_ = true;
    } else if (assembled_) {
      anchorSec_ += 0.25 / mtcFrameRate(rate_);
      anchorTime_ = t;
    }
    return true;
  }
  if (n >= 10 && m[0] == 0xF0 && m[1] == 0x7F && m[3] == 0x01 && m[4] == 0x01) {  // full frame: a locate
    rate_ = mtcRateFromCode((m[5] >> 5) & 3);
    tc_.h = m[5] & 0x1F;
    tc_.m = m[6] & 0x3F;
    tc_.s = m[7] & 0x3F;
    tc_.f = m[8] & 0x1F;
    anchorSec_ = static_cast<double>(mtcFrameOf(rate_, tc_)) / mtcFrameRate(rate_);
    anchorTime_ = t;
    hasPos_ = true;
    assembled_ = false;
    seqRun_ = 0;
    expect_ = -1;
    locate_ = true;
    locateSec_ = anchorSec_;
    return true;
  }
  return false;
}

bool MtcReceiver::locked(double now, double timeout) const { return assembled_ && seqRun_ >= 8 && now - lastQf_ < timeout; }

double MtcReceiver::seconds(double now) const {
  if (!locked(now)) return anchorSec_;
  // Between quarter frames the position runs on in real time (a few frames at most).
  return anchorSec_ + std::clamp(now - anchorTime_, 0.0, 4.0 / mtcFrameRate(rate_));
}

bool MtcReceiver::takeLocate(double& sec) {
  if (!locate_) return false;
  locate_ = false;
  sec = locateSec_;
  return true;
}

void SyncChase::reset() {
  chasing_ = false;
  drift_ = 0;
  settleUntil_ = -1e9;
  over_ = 0;
}

SyncChase::Action SyncChase::update(double now, bool masterRunning, double masterPos, bool localPlaying, double localPos) {
  Action a;
  const double target = masterPos + offset;
  if (!masterRunning) {
    if (chasing_) {
      chasing_ = false;
      drift_ = 0;
      over_ = 0;
      if (localPlaying) a.kind = Action::Stop;
    }
    return a;
  }
  if (!chasing_ || !localPlaying) {  // the master rolls: start where it is
    chasing_ = true;
    drift_ = 0;
    over_ = 0;
    settleUntil_ = now + settle;
    a.kind = Action::Start;
    a.position = target;
    return a;
  }
  const double d = localPos - target;
  drift_ = drift_ * 0.8 + d * 0.2;
  if (now < settleUntil_) {
    over_ = 0;
    return a;
  }
  if (std::fabs(d) > tolerance) {
    if (++over_ >= std::max(1, confirm)) {  // a real drift (or a jump of the master), not jitter
      a.kind = Action::Locate;
      a.position = target;
      ++relocations_;
      over_ = 0;
      drift_ = 0;
      settleUntil_ = now + settle;
    }
  } else {
    over_ = 0;
  }
  return a;
}

}  // namespace gl
