// Gridline — out-of-process plugin sandbox ("Run in sandbox") and 32-bit bridge (K-323).
//
// A sandboxed or 32-bit-bridged plugin runs in a child process (`<exe> --plugin-sandbox
// <shm name>`) that hosts the real Backend. The parent talks to it through:
//   * a bitness-independent shared-memory block (`shm::ShmBlock`, identical wire layout
//     between 32-bit x86 and 64-bit x86_64/ARM64 processes): input / sidechain / output
//     buffers, parameter changes, note events, transport, MIDI output, and two lock-free
//     32-bit atomic sequence counters (request, done);
//   * a socket / anonymous pipe channel for RPC messages (create, prepare, state, parameter
//     values and text, programs, editor, host events), request/response, UI thread.
// When the child dies (crash, kill) or stops answering, the instance is marked crashed:
// its output is silent, the song keeps its state, and the UI offers to reload it.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "../Backend.h"

namespace gl::host {

namespace shm {

inline constexpr uint32_t kShmMagic = 0x474C5342u;  // "GLSB"
inline constexpr uint32_t kShmVersion = 3u;         // 3: K-323 bitness-independent fixed-width ABI
inline constexpr int32_t kBlock = 1024;             // AudioEngine::kMaxBlock
inline constexpr int32_t kOutBuses = 16;            // main + 15 extra outputs
inline constexpr int32_t kMaxParams = 2048;
inline constexpr int32_t kMaxEvents = 1024;
inline constexpr int32_t kMaxMidiOut = 512;

// Transport flag bits inside ShmTransport::flags (no C++ bool padding hazards).
inline constexpr uint32_t kTransportPlaying = 1u << 0;
inline constexpr uint32_t kTransportRecording = 1u << 1;
inline constexpr uint32_t kTransportLooping = 1u << 2;
inline constexpr uint32_t kTransportChanged = 1u << 3;
inline constexpr uint32_t kTransportOffline = 1u << 4;

// All 8-byte fields sit on 8-byte boundaries; all structs have explicit padding
// and zero pointers / size_t / long / bool so i686 (32-bit) and x86_64 (64-bit)
// agree byte-for-byte on every field offset and sizeof.
struct alignas(8) ShmParamChange {
  uint32_t id = 0;
  int32_t offset = 0;
  double value = 0.0;
  int32_t mod = 0;   // 1: modulation amount (addParamMod)
  uint32_t pad0 = 0;
};
static_assert(sizeof(ShmParamChange) == 24, "ShmParamChange ABI size");
static_assert(offsetof(ShmParamChange, id) == 0, "ShmParamChange::id offset");
static_assert(offsetof(ShmParamChange, offset) == 4, "ShmParamChange::offset offset");
static_assert(offsetof(ShmParamChange, value) == 8, "ShmParamChange::value offset");
static_assert(offsetof(ShmParamChange, mod) == 16, "ShmParamChange::mod offset");
static_assert(offsetof(ShmParamChange, pad0) == 20, "ShmParamChange::pad0 offset");

struct alignas(8) ShmNoteEvent {
  double tuning = 0.0;
  int32_t offset = 0;
  int32_t noteId = -1;
  float value = 0.0f;
  float velocity = 0.0f;
  float pan = 0.0f;
  float modX = 0.5f;
  float modY = 0.5f;
  int16_t key = 60;
  int8_t channel = 0;
  uint8_t type = 0;
  uint8_t expr = 0;
  uint8_t hasProps = 0;
  uint8_t midi[3] = {0, 0, 0};
  uint8_t pad[3] = {0, 0, 0};
};
static_assert(sizeof(ShmNoteEvent) == 48, "ShmNoteEvent ABI size");
static_assert(offsetof(ShmNoteEvent, tuning) == 0, "ShmNoteEvent::tuning offset");
static_assert(offsetof(ShmNoteEvent, offset) == 8, "ShmNoteEvent::offset offset");
static_assert(offsetof(ShmNoteEvent, noteId) == 12, "ShmNoteEvent::noteId offset");
static_assert(offsetof(ShmNoteEvent, value) == 16, "ShmNoteEvent::value offset");
static_assert(offsetof(ShmNoteEvent, velocity) == 20, "ShmNoteEvent::velocity offset");
static_assert(offsetof(ShmNoteEvent, pan) == 24, "ShmNoteEvent::pan offset");
static_assert(offsetof(ShmNoteEvent, modX) == 28, "ShmNoteEvent::modX offset");
static_assert(offsetof(ShmNoteEvent, modY) == 32, "ShmNoteEvent::modY offset");
static_assert(offsetof(ShmNoteEvent, key) == 36, "ShmNoteEvent::key offset");
static_assert(offsetof(ShmNoteEvent, channel) == 38, "ShmNoteEvent::channel offset");
static_assert(offsetof(ShmNoteEvent, type) == 39, "ShmNoteEvent::type offset");
static_assert(offsetof(ShmNoteEvent, expr) == 40, "ShmNoteEvent::expr offset");
static_assert(offsetof(ShmNoteEvent, hasProps) == 41, "ShmNoteEvent::hasProps offset");
static_assert(offsetof(ShmNoteEvent, midi) == 42, "ShmNoteEvent::midi offset");
static_assert(offsetof(ShmNoteEvent, pad) == 45, "ShmNoteEvent::pad offset");

struct alignas(8) ShmTransport {
  double sampleRate = 44100.0;
  double tempo = 120.0;
  double projectQn = 0.0;
  double barQn = 0.0;
  double loopStartQn = 0.0;
  double loopEndQn = 0.0;
  int64_t projectSamples = 0;
  int64_t continuousSamples = 0;
  int64_t systemNanos = 0;
  int32_t barNumber = 0;
  int32_t tsNum = 4;
  int32_t tsDen = 4;
  uint32_t flags = 0;
};
static_assert(sizeof(ShmTransport) == 88, "ShmTransport ABI size");
static_assert(offsetof(ShmTransport, sampleRate) == 0, "ShmTransport::sampleRate offset");
static_assert(offsetof(ShmTransport, tempo) == 8, "ShmTransport::tempo offset");
static_assert(offsetof(ShmTransport, projectQn) == 16, "ShmTransport::projectQn offset");
static_assert(offsetof(ShmTransport, barQn) == 24, "ShmTransport::barQn offset");
static_assert(offsetof(ShmTransport, loopStartQn) == 32, "ShmTransport::loopStartQn offset");
static_assert(offsetof(ShmTransport, loopEndQn) == 40, "ShmTransport::loopEndQn offset");
static_assert(offsetof(ShmTransport, projectSamples) == 48, "ShmTransport::projectSamples offset");
static_assert(offsetof(ShmTransport, continuousSamples) == 56, "ShmTransport::continuousSamples offset");
static_assert(offsetof(ShmTransport, systemNanos) == 64, "ShmTransport::systemNanos offset");
static_assert(offsetof(ShmTransport, barNumber) == 72, "ShmTransport::barNumber offset");
static_assert(offsetof(ShmTransport, tsNum) == 76, "ShmTransport::tsNum offset");
static_assert(offsetof(ShmTransport, tsDen) == 80, "ShmTransport::tsDen offset");
static_assert(offsetof(ShmTransport, flags) == 84, "ShmTransport::flags offset");

struct alignas(4) ShmMidiOut {
  int32_t offset = 0;
  uint8_t data[3] = {0, 0, 0};
  uint8_t pad0 = 0;
};
static_assert(sizeof(ShmMidiOut) == 8, "ShmMidiOut ABI size");
static_assert(offsetof(ShmMidiOut, offset) == 0, "ShmMidiOut::offset offset");
static_assert(offsetof(ShmMidiOut, data) == 4, "ShmMidiOut::data offset");
static_assert(offsetof(ShmMidiOut, pad0) == 7, "ShmMidiOut::pad0 offset");

struct alignas(8) ShmBlock {
  uint32_t magic;
  uint32_t version;
  std::atomic<uint32_t> request;  // audio handshake (parent -> child)
  std::atomic<uint32_t> done;     // audio handshake (child -> parent)
  int32_t frames;
  int32_t ok;
  int32_t nParams;
  int32_t nEvents;
  ShmTransport transport;
  ShmParamChange params[kMaxParams];
  ShmNoteEvent events[kMaxEvents];
  float in[2][kBlock];
  float sc[2][kBlock];
  float out[kOutBuses][2][kBlock];
  int32_t outCh[kOutBuses];
  int32_t nMidiOut;
  uint32_t padTail;
  ShmMidiOut midiOut[kMaxMidiOut];
};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "lock-free 32-bit atomics are required in shared memory");
static_assert(sizeof(std::atomic<uint32_t>) == 4, "atomic<uint32_t> must be 4 bytes");
static_assert(offsetof(ShmBlock, magic) == 0, "ShmBlock::magic offset");
static_assert(offsetof(ShmBlock, version) == 4, "ShmBlock::version offset");
static_assert(offsetof(ShmBlock, request) == 8, "ShmBlock::request offset");
static_assert(offsetof(ShmBlock, done) == 12, "ShmBlock::done offset");
static_assert(offsetof(ShmBlock, frames) == 16, "ShmBlock::frames offset");
static_assert(offsetof(ShmBlock, ok) == 20, "ShmBlock::ok offset");
static_assert(offsetof(ShmBlock, nParams) == 24, "ShmBlock::nParams offset");
static_assert(offsetof(ShmBlock, nEvents) == 28, "ShmBlock::nEvents offset");
static_assert(offsetof(ShmBlock, transport) == 32, "ShmBlock::transport offset");
static_assert(offsetof(ShmBlock, params) == 120, "ShmBlock::params offset");
static_assert(offsetof(ShmBlock, events) == 49272, "ShmBlock::events offset");
static_assert(offsetof(ShmBlock, in) == 98424, "ShmBlock::in offset");
static_assert(offsetof(ShmBlock, sc) == 106616, "ShmBlock::sc offset");
static_assert(offsetof(ShmBlock, out) == 114808, "ShmBlock::out offset");
static_assert(offsetof(ShmBlock, outCh) == 245880, "ShmBlock::outCh offset");
static_assert(offsetof(ShmBlock, nMidiOut) == 245944, "ShmBlock::nMidiOut offset");
static_assert(offsetof(ShmBlock, padTail) == 245948, "ShmBlock::padTail offset");
static_assert(offsetof(ShmBlock, midiOut) == 245952, "ShmBlock::midiOut offset");
static_assert(sizeof(ShmBlock) == 250048, "ShmBlock exact ABI size");

inline ShmTransport toShm(const Transport& t) {
  ShmTransport s;
  s.sampleRate = t.sampleRate;
  s.tempo = t.tempo;
  s.projectQn = t.projectQn;
  s.barQn = t.barQn;
  s.loopStartQn = t.loopStartQn;
  s.loopEndQn = t.loopEndQn;
  s.projectSamples = t.projectSamples;
  s.continuousSamples = t.continuousSamples;
  s.systemNanos = t.systemNanos;
  s.barNumber = t.barNumber;
  s.tsNum = static_cast<int32_t>(t.tsNum);
  s.tsDen = static_cast<int32_t>(t.tsDen);
  s.flags = (t.playing ? kTransportPlaying : 0u) | (t.recording ? kTransportRecording : 0u) | (t.looping ? kTransportLooping : 0u) |
            (t.changed ? kTransportChanged : 0u) | (t.offline ? kTransportOffline : 0u);
  return s;
}

inline Transport fromShm(const ShmTransport& s) {
  Transport t;
  t.sampleRate = s.sampleRate;
  t.tempo = s.tempo;
  t.projectQn = s.projectQn;
  t.barQn = s.barQn;
  t.loopStartQn = s.loopStartQn;
  t.loopEndQn = s.loopEndQn;
  t.projectSamples = s.projectSamples;
  t.continuousSamples = s.continuousSamples;
  t.systemNanos = s.systemNanos;
  t.barNumber = s.barNumber;
  t.tsNum = static_cast<int>(s.tsNum);
  t.tsDen = static_cast<int>(s.tsDen);
  t.playing = (s.flags & kTransportPlaying) != 0;
  t.recording = (s.flags & kTransportRecording) != 0;
  t.looping = (s.flags & kTransportLooping) != 0;
  t.changed = (s.flags & kTransportChanged) != 0;
  t.offline = (s.flags & kTransportOffline) != 0;
  return t;
}

inline ShmNoteEvent toShm(const NoteEvent& e) {
  ShmNoteEvent s;
  s.tuning = e.tuning;
  s.offset = static_cast<int32_t>(e.offset);
  s.noteId = e.noteId;
  s.value = e.value;
  s.velocity = e.velocity;
  s.pan = e.pan;
  s.modX = e.modX;
  s.modY = e.modY;
  s.key = e.key;
  s.channel = e.channel;
  s.type = static_cast<uint8_t>(e.type);
  s.expr = e.expr;
  s.hasProps = e.hasProps ? 1u : 0u;
  s.midi[0] = e.midi[0];
  s.midi[1] = e.midi[1];
  s.midi[2] = e.midi[2];
  return s;
}

inline NoteEvent fromShm(const ShmNoteEvent& s) {
  NoteEvent e;
  e.tuning = s.tuning;
  e.offset = static_cast<int>(s.offset);
  e.noteId = s.noteId;
  e.value = s.value;
  e.velocity = s.velocity;
  e.pan = s.pan;
  e.modX = s.modX;
  e.modY = s.modY;
  e.key = s.key;
  e.channel = s.channel;
  e.type = static_cast<NoteEvent::Type>(s.type);
  e.expr = s.expr;
  e.hasProps = s.hasProps != 0;
  e.midi[0] = s.midi[0];
  e.midi[1] = s.midi[1];
  e.midi[2] = s.midi[2];
  return e;
}

inline ShmMidiOut toShm(const MidiOut& m) {
  ShmMidiOut s;
  s.offset = static_cast<int32_t>(m.offset);
  s.data[0] = m.data[0];
  s.data[1] = m.data[1];
  s.data[2] = m.data[2];
  return s;
}

inline MidiOut fromShm(const ShmMidiOut& s) {
  MidiOut m;
  m.offset = static_cast<int>(s.offset);
  m.data[0] = s.data[0];
  m.data[1] = s.data[1];
  m.data[2] = s.data[2];
  return m;
}

}  // namespace shm

// K-323: resolves the sibling 32-bit bridge executable (`gridline-bridge32.exe` on Windows,
// `gridline-bridge32` elsewhere) next to `mainExe` (or GRIDLINE_BRIDGE32_EXE when set).
std::string bridge32Executable(const std::string& mainExe);

// K-323: actionable error message when a 32-bit plugin cannot be hosted (macOS 10.15+
// unsupported, or missing `gridline-bridge32.exe` sibling helper on Windows).
std::string bridge32UnavailableError(const std::string& bridgePath,
                                     bool isMacOs =
#if defined(__APPLE__)
                                         true
#else
                                         false
#endif
);

// `name` titles the editor window the sandbox process opens. K-437: `blockNetwork` keeps the process
// off the internet (NetBlock.h); a process that cannot be does not load the plugin (*err says why).
std::unique_ptr<Backend> createSandboxBackend(const std::string& exe, const std::string& format, const std::string& entry,
                                              const std::string& uid, std::string* err, const std::string& name = {},
                                              bool blockNetwork = false);
// Child process entry point (`--plugin-sandbox <shm name>`); returns the exit code.
int sandboxChildMain(const std::string& shmName);

// Child-side native window for a plugin editor (macOS: SandboxWindow.mm; Linux has none: the
// interface attaches to a window of Gridline's).
// Returns the content view to attach the editor to, nullptr when unsupported.
// (x, y): top-left based global position of the window frame, or
// Editor::kNoPlace to centre it.
void* sandboxOpenWindow(const std::string& title, int w, int h, bool resizable, int x = Editor::kNoPlace, int y = Editor::kNoPlace);
// Top-left based global frame of the editor window (title bar included).
bool sandboxWindowFrame(int& x, int& y, int& w, int& h);
void sandboxResizeWindow(int w, int h);
bool sandboxWindowOpen();  // false after the user closed it
void sandboxCloseWindow();
void sandboxPumpEvents();  // non-blocking
// Waits up to `seconds` for `fd` to become readable, handling window events,
// timers and redraws meanwhile (macOS: inside the run loop); true when readable.
bool sandboxWaitEvents(int fd, double seconds);
// Keys the interface in the editor window did not use (for Gridline, J-66).
bool sandboxPopKey(KeyEvent& e);
void sandboxDevKeys();  // dev check: Space typed into the interface's own view
// Dev measurement (GRIDLINE_SANDBOX_STATS=1, GRIDLINE_DEV_DRAG="x,y,s"): the
// interface's drawing per second on stderr, a knob turned after 240 idles.
void sandboxDevStats(int idles);
// Plugin checks (macOS): a parent view in a window that is never shown;
// nullptr when unsupported. pumpOffscreenEvents runs the event loop briefly.
void* offscreenEditorParent(int w, int h);
void closeOffscreenEditorParent();
void pumpOffscreenEvents(double seconds);

}  // namespace gl::host
