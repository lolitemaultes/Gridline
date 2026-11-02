#!/usr/bin/env python3
"""Control host: loads a bridged plugin in Carla (from the distribution) and shows its interface.

Usage: carla_control.py <vst3|vst2|clap> <entry> <unused>. Runs until it is terminated. The test
drives the window with the same XTEST input it uses on Gridline, so that a failure can be pinned on
Gridline or on Wine, yabridge or the virtual display.
"""
import signal
import sys
import time

sys.path.insert(0, "/usr/share/carla")
import carla_backend as cb  # noqa: E402

LIBS = ["/usr/lib/carla/libcarla_standalone2.so", "/usr/lib/x86_64-linux-gnu/carla/libcarla_standalone2.so"]


def main():
    fmt, entry = sys.argv[1], sys.argv[2]
    kinds = {"vst3": cb.PLUGIN_VST3, "vst2": cb.PLUGIN_VST2, "clap": getattr(cb, "PLUGIN_CLAP", None)}
    if kinds.get(fmt) is None:
        print(f"this Carla has no {fmt} support", flush=True)
        return 2
    host = None
    for lib in LIBS:
        try:
            host = cb.CarlaHostDLL(lib, False)
            break
        except OSError as e:
            print(f"{lib}: {e}", flush=True)
    if host is None:
        return 2
    # Carla's dummy driver takes neither its default process mode nor JACK transport.
    host.set_engine_option(cb.ENGINE_OPTION_PROCESS_MODE, cb.ENGINE_PROCESS_MODE_CONTINUOUS_RACK, "")
    host.set_engine_option(cb.ENGINE_OPTION_TRANSPORT_MODE, cb.ENGINE_TRANSPORT_MODE_INTERNAL, "")
    host.set_engine_option(cb.ENGINE_OPTION_PATH_BINARIES, 0, "/usr/lib/carla")
    host.set_engine_option(cb.ENGINE_OPTION_PATH_RESOURCES, 0, "/usr/share/carla/resources")
    if not host.engine_init("Dummy", "control"):
        print("engine: " + str(host.get_last_error()), flush=True)
        return 2
    if not host.add_plugin(cb.BINARY_NATIVE, kinds[fmt], entry, None, None, 0, None, 0):
        print("add_plugin: " + str(host.get_last_error()), flush=True)
        return 2
    print("loaded " + str(host.get_plugin_info(0).get("name")), flush=True)
    host.show_custom_ui(0, True)
    running = [True]
    signal.signal(signal.SIGTERM, lambda *_: running.__setitem__(0, False))
    while running[0]:
        host.engine_idle()
        time.sleep(0.02)
    host.show_custom_ui(0, False)
    host.remove_all_plugins()
    host.engine_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
