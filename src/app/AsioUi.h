// Gridline: Settings > Audio for the ASIO backend (K-6): the driver type switch, the driver,
// its sample rates and buffer sizes, the control panel, latency, and the channel routing
// (driver inputs into the mixer's In 1.., the main output pair, extra output pairs).
#pragma once

namespace gl {
class App;
}

namespace gl::asioui {

// "Driver type" row (System audio / ASIO) when the backend is available. True = ASIO chosen.
bool backendRow(App& a, float fieldW);
// The ASIO device rows (in place of the output device / rate / buffer rows).
void deviceRows(App& a, float fieldW);
// The "ASIO CHANNELS" section body.
void channelRows(App& a, float fieldW);
// Input & recording: the driver's inputs feed the mixer (the system input device is not used).
bool inputsFromDriver(const App& a);

}  // namespace gl::asioui
