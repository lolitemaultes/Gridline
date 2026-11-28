// Gridline — "Block internet access" for one installed plugin.
//
// A plugin can only be kept off the internet when it runs in a process of its own (the plugin
// sandbox, or the scanner child): one in Gridline's process shares Gridline's network. The child
// restricts itself before any plugin code is loaded, with what the system offers an ordinary
// program (no administrator rights):
//   * macOS: the system sandbox's "no internet" profile (sandbox_init): TCP/IP is refused for the
//     process and every program it starts; local sockets (Gridline's own channel, the window
//     server, audio) keep working;
//   * Linux: a private network space (a user and network namespace, no route out), or where the
//     system does not allow one, a system call filter (seccomp) that refuses internet sockets;
//     both pass to every program the plugin starts;
//   * other systems: not available (Windows needs administrator rights or an app container;
//     FreeBSD and NetBSD need root).
// Not covered anywhere: programs that were not started by the blocked plugin (a licence manager,
// a system service, another plugin) and what the plugin does with files. It is a privacy setting,
// not protection against harmful plugins.
//
// Fail closed: a child asked to block that cannot does not load the plugin.
#pragma once
#include <string>

namespace gl::host {

// How a child process is kept off the internet (0: it is not).
enum NetGuard : int {
  kNetOpen = 0,
  kNetSeatbelt = 1,   // macOS: the system sandbox's no internet profile
  kNetNamespace = 2,  // Linux: a private network space
  kNetSeccomp = 3,    // Linux: a system call filter refusing internet sockets
};

// True when this build can block a plugin's internet access on this kind of system.
bool networkBlockAvailable();

// Child side (scanner and sandbox processes): restricts this process (call it first, while the
// process has one thread, before any plugin is loaded). Returns the guard now in effect, kNetOpen
// on failure with the reason in *why. GRIDLINE_DEV_NET_BLOCK_FAIL=1 makes it fail (tests).
int applyNetworkBlock(std::string* why);
// What the child was asked for and got (set by noteNetworkBlock from the command line).
void noteNetworkBlock(bool requested, int guard, const std::string& why);
bool networkBlockRequested();
int networkGuard();
std::string networkBlockError();

// The child's command line switch.
inline constexpr const char* kNetBlockArg = "--net-block";

}  // namespace gl::host
