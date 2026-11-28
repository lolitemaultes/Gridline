// Gridline — a CLAP effect that tries to reach the network, for the "Block internet access" tests
// (built in-tree as GridlineNetProbe.clap in its own folder; never installed). POSIX only.
//   "org.gridline.test.netprobe"  stereo pass-through. Each time an instance is created (scans
//       create one too) it tries, against 127.0.0.1 on the port in GRIDLINE_NETPROBE_PORT, never
//       anywhere else:
//         T  a TCP connection (one message),
//         U  a UDP datagram,
//         F  a TCP connection from a child process it forks,
//         S  a TCP connection from a program it starts (GRIDLINE_NETPROBE_HELPER, its own executable),
//       each message "<letter>:<GRIDLINE_NETPROBE_TAG>". It appends what each attempt returned to
//       the file GRIDLINE_NETPROBE_OUT ("<tag> tcp=ok udp=refused fork=ok spawn=refused"), so a test
//       knows the attempts were made whether or not they arrived. No variable: no attempt.
#include <clap/clap.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern char** environ;

namespace {

sockaddr_in loopback(int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}

bool sendTcp(int port, const std::string& msg) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  const sockaddr_in a = loopback(port);
  bool ok = ::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
  ok = ok && ::send(fd, msg.data(), msg.size(), 0) == static_cast<ssize_t>(msg.size());
  ::close(fd);
  return ok;
}

bool sendUdp(int port, const std::string& msg) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  const sockaddr_in a = loopback(port);
  const bool ok = ::sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr*>(&a), sizeof a) == static_cast<ssize_t>(msg.size());
  ::close(fd);
  return ok;
}

bool fromFork(int port, const std::string& msg) {
  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) _exit(sendTcp(port, msg) ? 0 : 1);
  int st = 0;
  if (::waitpid(pid, &st, 0) != pid) return false;
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

bool fromProgram(const char* helper, int port, const std::string& msg) {
  if (!helper || !*helper) return false;
  std::string portText = std::to_string(port);
  char* argv[] = {const_cast<char*>(helper), portText.data(), const_cast<char*>(msg.c_str()), nullptr};
  pid_t pid = 0;
  if (::posix_spawn(&pid, helper, nullptr, nullptr, argv, environ) != 0) return false;
  int st = 0;
  if (::waitpid(pid, &st, 0) != pid) return false;
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

void probe() {
  const char* portText = std::getenv("GRIDLINE_NETPROBE_PORT");
  if (!portText || !*portText) return;
  const int port = std::atoi(portText);
  const char* tagText = std::getenv("GRIDLINE_NETPROBE_TAG");
  const std::string tag = tagText && *tagText ? tagText : "x";
  const bool tcp = sendTcp(port, "T:" + tag);
  const bool udp = sendUdp(port, "U:" + tag);
  const bool fork = fromFork(port, "F:" + tag);
  const bool spawn = fromProgram(std::getenv("GRIDLINE_NETPROBE_HELPER"), port, "S:" + tag);
  if (const char* out = std::getenv("GRIDLINE_NETPROBE_OUT"); out && *out) {
    if (std::FILE* f = std::fopen(out, "a")) {
      auto word = [](bool ok) { return ok ? "ok" : "refused"; };
      std::fprintf(f, "%s tcp=%s udp=%s fork=%s spawn=%s\n", tag.c_str(), word(tcp), word(udp), word(fork), word(spawn));
      std::fclose(f);
    }
  }
}

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr};
const clap_plugin_descriptor_t kDesc = {CLAP_VERSION_INIT, "org.gridline.test.netprobe", "Gridline Net Probe", "Gridline Tests", "", "", "",
                                        "1.0.0", "tries to connect (tests)", kFeatures};

uint32_t portCount(const clap_plugin_t*, bool) { return 1; }
bool portGet(const clap_plugin_t*, uint32_t i, bool input, clap_audio_port_info_t* info) {
  if (i != 0) return false;
  info->id = input ? 1 : 2;
  std::snprintf(info->name, CLAP_NAME_SIZE, "%s", input ? "In" : "Out");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kPorts = {portCount, portGet};

bool save(const clap_plugin_t*, const clap_ostream_t* st) {
  const int32_t magic = 0x4E455450;  // "NETP"
  return st->write(st, &magic, sizeof magic) == static_cast<int64_t>(sizeof magic);
}
bool load(const clap_plugin_t*, const clap_istream_t* st) {
  int32_t magic = 0;
  return st->read(st, &magic, sizeof magic) == static_cast<int64_t>(sizeof magic) && magic == 0x4E455450;
}
const clap_plugin_state_t kState = {save, load};

const void* extension(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  return nullptr;
}

clap_process_status process(const clap_plugin_t*, const clap_process_t* pr) {
  if (pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;
  const clap_audio_buffer_t& in = pr->audio_inputs[0];
  const clap_audio_buffer_t& out = pr->audio_outputs[0];
  for (uint32_t c = 0; c < 2 && c < in.channel_count && c < out.channel_count; ++c)
    if (in.data32 && out.data32 && in.data32[c] && out.data32[c] && in.data32[c] != out.data32[c])
      std::memcpy(out.data32[c], in.data32[c], sizeof(float) * pr->frames_count);
  return CLAP_PROCESS_CONTINUE;
}

clap_plugin_t gTemplate = [] {
  clap_plugin_t p{};
  p.desc = &kDesc;
  p.init = [](const clap_plugin_t*) {
    probe();
    return true;
  };
  p.destroy = [](const clap_plugin_t* pl) { delete pl; };
  p.activate = [](const clap_plugin_t*, double, uint32_t, uint32_t) { return true; };
  p.deactivate = [](const clap_plugin_t*) {};
  p.start_processing = [](const clap_plugin_t*) { return true; };
  p.stop_processing = [](const clap_plugin_t*) {};
  p.reset = [](const clap_plugin_t*) {};
  p.process = process;
  p.get_extension = extension;
  p.on_main_thread = [](const clap_plugin_t*) {};
  return p;
}();

uint32_t count(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kDesc : nullptr; }
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t*, const char* id) {
  if (std::strcmp(id, kDesc.id) != 0) return nullptr;
  return new clap_plugin_t(gTemplate);
}
const clap_plugin_factory_t kFactory = {count, descriptor, create};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
