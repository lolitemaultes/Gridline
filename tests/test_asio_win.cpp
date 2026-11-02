// K-6: Windows only: the installed ASIO drivers are listed from the registry (passes with none
// installed, as on CI runners). No driver is loaded: a real one could open a device.
#include <cstdio>

#include "Check.h"
#include "host/audio/AsioHost.h"

int main() {
  const auto drivers = gl::asio::listInstalledDrivers();
  bool wellFormed = true;
  for (const auto& d : drivers) {
    std::printf("# driver: %s %s\n", d.name.c_str(), d.clsid.c_str());
    if (d.name.empty() || d.clsid.size() < 2 || d.clsid.front() != '{' || d.simulated) wellFormed = false;
  }
  std::printf("# %zu ASIO driver(s) installed\n", drivers.size());
  expect(wellFormed, "every listed driver has a name and a class id");
  expect(gl::asio::backendAvailable(), "the ASIO backend is offered on Windows");
  return finish();
}
