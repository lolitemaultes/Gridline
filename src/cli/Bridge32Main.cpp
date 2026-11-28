// Gridline — K-323: 32-bit plugin bridge child executable (`gridline-bridge32`).
// Minimal entry point delegating to `gl::host::childMain` for `--scan-plugin`,
// `--scan-vst3`, and `--plugin-sandbox` over the bitness-independent SHM ABI.
#include "host/PluginScan.h"

int main(int argc, char** argv) {
  const int r = gl::host::childMain(argc, argv);
  return r >= 0 ? r : 1;
}
