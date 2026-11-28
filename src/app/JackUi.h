// Gridline: Settings > Audio for the JACK / PipeWire backend (K-357).
// Mirrors AsioUi.h: driver type switch, status/error banner, sample rate and buffer request,
// named input/output port counts, auto-connect/restore switches, inline connections patcher,
// and extra output pair mixer routing.
#pragma once

namespace gl {
class App;
}

namespace gl::jackui {

// "Driver type" row (System audio / JACK / PipeWire) when the backend is available. True = JACK chosen.
bool backendRow(App& a, float fieldW);
// JACK / PipeWire status, sample rate (server-controlled), buffer request and latency rows.
void deviceRows(App& a, float fieldW);
// The "JACK / PIPEWIRE PORTS" section body: counts, switches, connections table, extra output taps.
void portRows(App& a, float fieldW);
// True when JACK / PipeWire owns audio input routing (hides the SDL input device selector).
bool inputsFromDriver(const App& a);

}  // namespace gl::jackui
