// Gridline: a small program with the crash handler installed: the symbol test needs debug
// information without paying for a debug build of the whole app, and the false-alarm tests need
// precise control of the "interface" thread.
//   crash_probe <config folder> fault | busy <seconds> | quiet <seconds> | beat <seconds> | unarmed <seconds>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include "core/Crash.h"

using namespace gl;

namespace {

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void probeFaultHere() {
  crash::crashNowForTest();  // probeFaultHere: the line the symbol test looks for
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void probeCaller() {
  probeFaultHere();
  std::puts("not reached");
}

void waitSeconds(int s) { std::this_thread::sleep_for(std::chrono::seconds(s)); }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string cmd = argv[2];
  const int seconds = argc > 3 ? std::atoi(argv[3]) : 0;
  crash::Config c;
  c.configDir = argv[1];
  c.sentinel = false;
  c.frozenAfterSeconds = 2;
  crash::install(c);
  crash::crumb("probe", cmd.c_str());
  if (cmd == "fault") probeCaller();
  if (cmd == "unarmed") {  // startup work is not watched
    waitSeconds(seconds);
    return 0;
  }
  crash::armWatchdog();
  if (cmd == "busy") {  // long legitimate work: no report
    crash::BusyScope busy("probe work", 60);
    waitSeconds(seconds);
  } else if (cmd == "beat") {  // a slow but living interface: no report
    for (int i = 0; i < seconds; ++i) {
      crash::uiHeartbeat();
      waitSeconds(1);
    }
  } else if (cmd == "quiet") {  // really silent, and still silent at the end: the report stays
    waitSeconds(seconds);
    std::fflush(nullptr);
    std::_Exit(0);
  }
  crash::markCleanExit();
  return 0;
}
