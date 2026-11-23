#!/usr/bin/env python3
"""Gridline: MIDI devices through the real system back end, plugged in and out while the app runs.

The unit and app tests use stand-in devices (GRIDLINE_TEST_MIDI=fake). This driver runs the real app
(hidden, quiet windows, SDL's dummy audio, a scratch config folder and HOME) with GRIDLINE_TEST_MIDI=1,
so it opens the system's devices, and drives it while it runs through --dev-input-live. It changes
the machine's MIDI setup (kernel modules, the sound server), so it runs only on a throwaway CI
machine and only with GRIDLINE_MIDI_REAL=1 (.github/workflows/midi-real.yml).

Linux (ALSA):
  A. a kernel virtual MIDI device (snd-virmidi): present at launch it opens; a note written into its
     raw MIDI node reaches Gridline's sequencer input; unplugged (modprobe -r, or the driver unbound
     in sysfs when the module is held) it closes within a few seconds (the /dev/snd watcher, not the
     10 s timer), the held note is released and nothing keeps sounding; plugged in again it reopens
     without a restart and its notes arrive.
  B. a user-space sequencer port (tools/ci/midi-seq-port.c, as a soft synth or a USB driver offers):
     it appears after launch and opens (no /dev/snd change: the timer rescan finds it), a note
     arrives, it goes away and closes with the note released, it comes back and reopens.
  When kernel modules cannot be loaded, A is reported as skipped (a failure with --require-kernel)
  and B runs alone.
BSD (sndio): sndiod's MIDI thru box midithru/0 opens at launch and a note sent into it with
  midicat reaches Gridline. sndio has no hot-plug in Gridline (listing a device opens it, so devices
  are listed at startup and by Rescan only): stopping sndiod and pressing Rescan closes the input
  with its held note released, starting it again and pressing Rescan reopens it.

  GRIDLINE_MIDI_REAL=1 midi-real.py --app <gridline-app> --demo <song> --out <dir> [--helper <midi-seq-port>]
      [--require-kernel] [--require-sndiod]
Writes <out>/summary.md (what ran, what passed, what was skipped and why); exits 1 on any failure.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time

GITHUB = os.environ.get("GITHUB_ACTIONS") == "true" or os.environ.get("CI") == "true"
PRIV = [] if os.geteuid() == 0 else ["sudo", "-n"]


def log(msg):
    print(msg, flush=True)


class Report:
    def __init__(self, title):
        self.title = title
        self.rows = []
        self.failed = False

    def _add(self, kind, what, detail):
        self.rows.append((kind, what, detail))
        log("%s - %s%s" % (kind, what, (": " + detail) if detail else ""))

    def ok(self, what, detail=""):
        self._add("PASS", what, detail)

    def fail(self, what, detail=""):
        self.failed = True
        self._add("FAIL", what, detail)
        if GITHUB:
            log("::error::%s%s" % (what, (": " + detail) if detail else ""))

    def skip(self, what, detail=""):
        self._add("SKIP", what, detail)
        if GITHUB:
            log("::warning::%s%s" % (what, (": " + detail) if detail else ""))

    def note(self, what, detail=""):
        self._add("NOTE", what, detail)

    def check(self, cond, what, detail=""):
        (self.ok if cond else self.fail)(what, detail)
        return cond

    def write(self, path):
        with open(path, "w") as f:
            f.write("## %s\n\n| result | check | detail |\n|---|---|---|\n" % self.title)
            for kind, what, detail in self.rows:
                f.write("| %s | %s | %s |\n" % (kind, what.replace("|", "/"), detail.replace("|", "/").replace("\n", " ")))
            f.write("\n%s\n" % ("Some checks FAILED." if self.failed else "Every check that ran passed."))


def run(cmd, timeout=60, env=None, priv=False):
    """Runs a command; returns (exit code, stdout + stderr). Never raises."""
    full = (PRIV + list(cmd)) if priv else list(cmd)
    try:
        p = subprocess.run(full, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, env=env)
        out = p.stdout.decode(errors="replace")
        rc = p.returncode
    except FileNotFoundError:
        out, rc = "%s: not found" % full[0], 127
    except subprocess.TimeoutExpired:
        out, rc = "%s: timed out" % " ".join(full), 124
    log("$ %s -> %d%s" % (" ".join(full), rc, ("\n" + out.rstrip()) if out.strip() else ""))
    return rc, out


MIDIOPEN = re.compile(r'midiopen frame -?\d+: inputs "([^"]*)" ports "([^"]*)" held "([^"]*)" output (\d) released (\d+) '
                      r'listed "([^"]*)" outputs (\d+) rescans (\d+)')
SOUND = re.compile(r"sound frame -?\d+: peak ([0-9.eE+-]+) live (-?\d+)")


class App:
    """The real gridline-app, hidden, driven while it runs through --dev-input-live."""

    def __init__(self, opts, tag):
        self.work = os.path.join(opts.out, tag)
        shutil.rmtree(self.work, ignore_errors=True)
        cfg = os.path.join(self.work, "cfg")
        os.makedirs(os.path.join(cfg, "gridline"))
        self.home = os.path.join(self.work, "home")
        os.makedirs(self.home)
        with open(os.path.join(cfg, "gridline", "settings.json"), "w") as f:
            f.write('{"format": "gridline-settings", "values": {"ui.layoutChosen": true, "ui.layout": "tracker", '
                    '"keys.profile": "tracker", "ui.menuStyle": "tracker"}}\n')
        song = os.path.join(self.work, "song.gridline.json")
        shutil.copyfile(opts.demo, song)
        self.live = os.path.join(self.work, "live.txt")
        env = dict(os.environ)
        env.update(SDL_AUDIODRIVER="dummy", GRIDLINE_NO_PLUGIN_SCAN="1", GRIDLINE_HIDE_PLUGINS="all", GRIDLINE_CONFIG_HOME=cfg,
                   HOME=self.home, GRIDLINE_QUIET_WINDOWS="1", GRIDLINE_NO_UPDATE_CHECK="1", GRIDLINE_NO_REALTIME="1",
                   GRIDLINE_TEST_MIDI="1")  # "1": the real system MIDI service in a hidden run (not the stand-ins)
        args = [opts.app, song, "--hidden", "--no-ui-sounds", "--no-update-check", "--size", "1440x900", "--layout", "tracker",
                "--dev-input-live", self.live]
        self.outp = os.path.join(self.work, "out.log")
        self.out = open(self.outp, "w")
        log("starting %s (log %s)" % (tag, self.outp))
        self.proc = subprocess.Popen(args, env=env, stdout=self.out, stderr=subprocess.STDOUT)
        end = time.time() + 120
        try:
            while "midiopen frame" not in self.text():
                if self.proc.poll() is not None:
                    raise RuntimeError("gridline-app exited at launch (%s); log: %s" % (self.proc.returncode, self.outp))
                if time.time() > end:
                    raise RuntimeError("gridline-app did not answer within 120 s; log: %s" % self.outp)
                self.send("midiopen")
                time.sleep(0.1)
        except RuntimeError:
            if self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait()
            self.out.close()
            raise
        self.last = self.state()

    def text(self):
        with open(self.outp, errors="replace") as f:
            return f.read()

    def send(self, *lines, timeout=60):
        tmp = self.live + ".tmp"
        with open(tmp, "w") as f:
            f.write("".join("0 %s\n" % ln for ln in lines))
        os.rename(tmp, self.live)
        end = time.time() + timeout
        while os.path.exists(self.live):
            if self.proc.poll() is not None:
                raise RuntimeError("gridline-app exited (%s); log: %s" % (self.proc.returncode, self.outp))
            if time.time() > end:
                raise RuntimeError("gridline-app stopped reading its input; log: %s" % self.outp)
            time.sleep(0.005)

    def ask(self, cmd, pat):
        n = len(self.text())
        self.send(cmd)
        end = time.time() + 30
        while time.time() < end:
            m = pat.search(self.text()[n:])
            if m:
                return m
            time.sleep(0.02)
        raise RuntimeError("no answer to %s; log: %s" % (cmd, self.outp))

    def state(self):
        m = self.ask("midiopen", MIDIOPEN)
        names = [x for x in m.group(1).split("|") if x]
        held = [int(x) for x in m.group(3).split(",") if x]
        return {"inputs": names, "held": dict(zip(names, held)), "released": int(m.group(5)),
                "listed": [x for x in m.group(6).split("|") if x], "rescans": int(m.group(8)), "line": m.group(0)}

    def sound(self):
        m = self.ask("sound", SOUND)
        return float(m.group(1)), int(m.group(2))

    def wait(self, pred, timeout, step=0.2):
        """Polls the MIDI state until pred(state) holds: (state, seconds) or (None, seconds) with the last state."""
        t0 = time.time()
        while True:
            st = self.state()
            if pred(st):
                return st, time.time() - t0
            if time.time() - t0 > timeout:
                self.last = st
                return None, time.time() - t0
            time.sleep(step)

    def wait_fresh_rescan(self, timeout=15):
        """Waits for a background rescan to begin, so the next timer rescan is about 10 s away."""
        r0 = self.state()["rescans"]
        st, _ = self.wait(lambda s: s["rescans"] > r0, timeout, step=0.1)
        return st is not None

    def silent(self, timeout=8):
        """Waits until no live note is held in the engine and the output is quiet: (live, peak, ok)."""
        end = time.time() + timeout
        self.sound()  # resets the peak
        while True:
            time.sleep(0.5)
            peak, live = self.sound()
            if (live == 0 and peak < 1e-4) or time.time() > end:
                return live, peak, live == 0 and peak < 1e-4

    def quit(self):
        try:
            self.send("quit discard", timeout=15)
        except RuntimeError:
            pass
        try:
            rc = self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            try:
                rc = self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                rc = self.proc.wait()
        self.out.close()
        return rc


def open_name(st, part):
    for n in st["inputs"]:
        if part in n:
            return n
    return None


def held(st, part):
    n = open_name(st, part)
    return st["held"].get(n, 0) if n else -1


def finish(rep, app, tag):
    rc = app.quit()
    rep.check(rc == 0, "%s: the app quits cleanly" % tag, "exit code %s" % rc)


# ---------------------------------------------------------------------------------------------- Linux


def virmidi_card():
    try:
        with open("/proc/asound/cards") as f:
            for line in f:
                m = re.match(r"\s*(\d+)\s+\[([^\]]*)\]:\s*(.*)", line)
                if m and "virmidi" in (m.group(2) + m.group(3)).lower():
                    card = int(m.group(1))
                    return card if glob.glob("/dev/snd/midiC%dD*" % card) else None
    except OSError:
        pass
    return None


def wait_card(present, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        c = virmidi_card()
        if (c is not None) == present:
            return c
        time.sleep(0.1)
    return virmidi_card()


def amidi(card, hexbytes):
    rc, _ = run(["amidi", "-p", "hw:%d,0" % card, "-S", hexbytes], priv=True)
    return rc == 0


def setup_seq(rep):
    rc, out = run(["modprobe", "snd-seq"], priv=True)
    if not os.path.exists("/dev/snd/seq"):
        rep.fail("no ALSA sequencer (/dev/snd/seq): no real ALSA MIDI could be tested",
                 "modprobe snd-seq: %s (kernel %s)" % (out.strip() or rc, os.uname().release))
        return False
    run(["chmod", "a+rw", "/dev/snd/seq"], priv=True)  # the runner's user is not in the audio group
    if not os.access("/dev/snd/seq", os.R_OK | os.W_OK):
        rep.fail("the ALSA sequencer is there but this user cannot open it", "/dev/snd/seq")
        return False
    rep.ok("ALSA sequencer available", "kernel %s" % os.uname().release)
    return True


def unplug_virmidi(rep):
    rc, out = run(["modprobe", "-r", "snd-virmidi"], priv=True)
    if rc == 0 and wait_card(False, 5) is None:
        return ("modprobe", [])
    rep.note("modprobe -r snd-virmidi was refused while Gridline held the port open",
             (out.strip() or "exit %d" % rc) + "; the driver holds a module reference per subscriber, so the device is removed "
             "by unbinding it from its driver in sysfs instead (the card goes away while in use, as a USB unplug does)")
    devs = [os.path.basename(d) for d in glob.glob("/sys/bus/platform/drivers/snd_virmidi/snd_virmidi.*")]
    for d in devs:
        run(["sh", "-c", "echo %s > /sys/bus/platform/drivers/snd_virmidi/unbind" % d], priv=True)
    if devs and wait_card(False, 5) is None:
        return ("unbind", devs)
    return None


def plug_virmidi(how):
    method, devs = how
    if method == "modprobe":
        run(["modprobe", "snd-virmidi", "midi_devs=1"], priv=True)
    else:
        for d in devs:
            run(["sh", "-c", "echo %s > /sys/bus/platform/drivers/snd_virmidi/bind" % d], priv=True)
    return wait_card(True, 10)


def scenario_virmidi(rep, opts, card):
    app = App(opts, "alsa_virmidi")
    try:
        st, t = app.wait(lambda s: open_name(s, "VirMIDI"), 30)
        if not rep.check(st is not None, "A1 kernel device present at launch opens", (open_name(st, "VirMIDI") if st else app.last["line"])):
            return
        name = open_name(st, "VirMIDI")
        amidi(card, "90 3C 64")
        st, t = app.wait(lambda s: held(s, "VirMIDI") >= 1, 10)
        rep.check(st is not None, "A2 a note written into the device's raw MIDI node reaches Gridline (ALSA sequencer input)",
                  "held %d on %s" % (held(st or app.last, "VirMIDI"), name))
        released0 = (st or app.last)["released"]
        if not app.wait_fresh_rescan():
            rep.note("no background rescan seen within 15 s before the unplug", "the timing check below is then not conclusive")
        how = unplug_virmidi(rep)
        if not rep.check(how is not None, "A3 the device can be unplugged on this runner", how[0] if how else "modprobe -r and unbind both failed"):
            return
        st, t = app.wait(lambda s: open_name(s, "VirMIDI") is None, 25)
        if not rep.check(st is not None, "A3 unplugged (%s): the input closes without a restart" % how[0],
                         "%.1f s" % t if st else app.last["line"]):
            return
        rep.check(t < 6.0, "A3 the close was driven by the /dev/snd watcher (well before the 10 s timer rescan)", "%.1f s" % t)
        rep.check(st["released"] > released0, "A4 the note held on the unplugged device is released", "released %d" % st["released"])
        live, peak, quiet = app.silent()
        rep.check(quiet, "A4 no stuck note: nothing is held in the engine and the output goes quiet", "live %d peak %.6f" % (live, peak))
        if not app.wait_fresh_rescan():
            rep.note("no background rescan seen within 15 s before the plug", "the timing check below is then not conclusive")
        card = plug_virmidi(how)
        if not rep.check(card is not None, "A5 the device can be plugged in again", how[0]):
            return
        st, t = app.wait(lambda s: open_name(s, "VirMIDI"), 25)
        if not rep.check(st is not None, "A5 plugged in again: it reopens without a restart", "%.1f s, %s" % (t, open_name(st, "VirMIDI")) if st else app.last["line"]):
            return
        rep.check(t < 6.0, "A5 the reopen was driven by the /dev/snd watcher", "%.1f s" % t)
        amidi(card, "90 3E 64")
        st, t = app.wait(lambda s: held(s, "VirMIDI") >= 1, 10)
        rep.check(st is not None, "A6 after the replug its notes arrive", "held %d" % held(st or app.last, "VirMIDI"))
        amidi(card, "80 3E 00")
        st, t = app.wait(lambda s: held(s, "VirMIDI") == 0, 10)
        rep.check(st is not None, "A6 and its note-off arrives", "held %d" % held(st or app.last, "VirMIDI"))
    finally:
        finish(rep, app, "A")


class SeqPort:
    def __init__(self, helper, name):
        self.p = subprocess.Popen([helper, name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  universal_newlines=True)
        self.ready = self.p.stdout.readline().strip()
        log("midi-seq-port: %s" % self.ready)

    def send(self, hexbytes):
        self.p.stdin.write(hexbytes + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def unplug(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except OSError:
            pass
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill()


def scenario_seqport(rep, opts):
    name = "Gridline CI Keys"
    app = App(opts, "alsa_softport")
    port = None
    try:
        st = app.state()
        rep.check(open_name(st, name) is None, "B1 the software port is not there at launch", st["line"])
        port = SeqPort(opts.helper, name)
        if not rep.check(port.ready.startswith("ready"), "B1 a user-space sequencer port is created", port.ready):
            return
        st, t = app.wait(lambda s: open_name(s, name), 30)
        if not rep.check(st is not None, "B1 a port that appears after launch opens (found by the timer rescan: no /dev/snd change)",
                         "%.1f s" % t if st else app.last["line"]):
            return
        released0 = st["released"]
        port.send("90 40 64")
        st, t = app.wait(lambda s: held(s, name) >= 1, 10)
        rep.check(st is not None, "B2 a note sent from the port reaches Gridline", "held %d" % held(st or app.last, name))
        port.unplug()
        port = None
        st, t = app.wait(lambda s: open_name(s, name) is None, 30)
        if not rep.check(st is not None, "B3 the port goes away: the input closes", "%.1f s" % t if st else app.last["line"]):
            return
        rep.check(st["released"] > released0, "B3 its held note is released", "released %d" % st["released"])
        live, peak, quiet = app.silent()
        rep.check(quiet, "B3 no stuck note", "live %d peak %.6f" % (live, peak))
        port = SeqPort(opts.helper, name)
        st, t = app.wait(lambda s: open_name(s, name), 30)
        if not rep.check(st is not None, "B4 the port comes back: it reopens", "%.1f s" % t if st else app.last["line"]):
            return
        port.send("90 41 64")
        st, t = app.wait(lambda s: held(s, name) >= 1, 10)
        rep.check(st is not None, "B4 and its notes arrive", "held %d" % held(st or app.last, name))
        port.send("80 41 00")
    finally:
        if port:
            port.unplug()
        finish(rep, app, "B")


def linux(rep, opts):
    run(["uname", "-a"])
    if not setup_seq(rep):
        return
    rc, out = run(["modprobe", "snd-virmidi", "midi_devs=1"], priv=True)
    card = wait_card(True, 10)
    if card is None:
        why = out.strip() or "exit %d" % rc
        if opts.require_kernel:
            rep.fail("A kernel virtual MIDI device (snd-virmidi) could not be loaded", why)
        else:
            rep.skip("A kernel virtual MIDI device (snd-virmidi) could not be loaded: the /dev/snd watcher and a kernel device's "
                     "unplug were NOT exercised", why)
    else:
        run(["cat", "/proc/asound/cards"])
        run(["aconnect", "-l"])
        rep.ok("kernel virtual MIDI device loaded", "card %d" % card)
        try:
            scenario_virmidi(rep, opts, card)
        except RuntimeError as e:
            rep.fail("A stopped", str(e))
    if opts.helper and os.access(opts.helper, os.X_OK):
        try:
            scenario_seqport(rep, opts)
        except RuntimeError as e:
            rep.fail("B stopped", str(e))
    else:
        rep.skip("B the user-space port helper was not built", str(opts.helper))


# ---------------------------------------------------------------------------------------------- sndio


def sndiod_running():
    rc, _ = run(["pgrep", "-x", "sndiod"])
    return rc == 0


def start_sndiod():
    if sndiod_running():
        return True
    if sys.platform.startswith("openbsd"):
        run(["rcctl", "-f", "start", "sndiod"], priv=True)
    if not sndiod_running():
        run(["sndiod"], priv=True)  # puts itself in the background
    for _ in range(20):
        if sndiod_running():
            return True
        time.sleep(0.25)
    return False


def stop_sndiod():
    if sys.platform.startswith("openbsd"):
        run(["rcctl", "stop", "sndiod"], priv=True)
    run(["pkill", "-x", "sndiod"], priv=True)
    for _ in range(40):
        if not sndiod_running():
            return True
        time.sleep(0.25)
    return False


def midicat(home, hexbytes, work):
    """Sends bytes into sndiod's MIDI thru box. The same HOME as Gridline's: sndiod accepts a second client
    only with the same session cookie (~/.sndio/cookie) as the one connected."""
    path = os.path.join(work, "send.mid")
    with open(path, "wb") as f:
        f.write(bytes(int(x, 16) for x in hexbytes.split()))
    env = dict(os.environ, HOME=home)
    rc, _ = run(["midicat", "-i", path, "-q", "midithru/0"], timeout=15, env=env)
    return rc == 0


def sndio(rep, opts):
    run(["uname", "-a"])
    for tool in ("sndiod", "midicat"):
        if not shutil.which(tool):
            (rep.fail if opts.require_sndiod else rep.skip)("sndio: %s is not installed: sndio MIDI input was NOT exercised" % tool)
            return
    if not start_sndiod():
        (rep.fail if opts.require_sndiod else rep.skip)("sndio: sndiod could not be started: sndio MIDI input was NOT exercised")
        return
    rep.ok("sndiod running")
    app = App(opts, "sndio")
    thru = "midithru/0"
    try:
        st, t = app.wait(lambda s: open_name(s, thru), 30)
        if not rep.check(st is not None, "S1 sndiod's MIDI thru box is listed and opens at launch", st["line"] if st else app.last["line"]):
            return
        rep.check(midicat(app.home, "90 3C 64", app.work), "S2 midicat sends a note into midithru/0")
        st, t = app.wait(lambda s: held(s, thru) >= 1, 10)
        rep.check(st is not None, "S2 the note reaches Gridline's sndio input", "held %d" % held(st or app.last, thru))
        midicat(app.home, "80 3C 00", app.work)
        st, t = app.wait(lambda s: held(s, thru) == 0, 10)
        rep.check(st is not None, "S2 and its note-off", "held %d" % held(st or app.last, thru))
        # No hot-plug on sndio: the device going away (sndiod stopped) and Rescan.
        midicat(app.home, "90 3E 64", app.work)
        st, t = app.wait(lambda s: held(s, thru) >= 1, 10)
        released0 = (st or app.last)["released"]
        rep.check(stop_sndiod(), "S3 sndiod stopped (the device goes away)")
        time.sleep(3)
        st = app.state()
        rep.note("S3 without Rescan the input stays as it was (no hot-plug on sndio, by design)", st["line"])
        app.send("midirescan")
        st, t = app.wait(lambda s: open_name(s, thru) is None, 20)
        if not rep.check(st is not None, "S3 Rescan with sndiod gone closes the input", "%.1f s" % t if st else app.last["line"]):
            return
        rep.check(st["released"] > released0, "S3 the note it held is released", "released %d" % st["released"])
        live, peak, quiet = app.silent()
        rep.check(quiet, "S3 no stuck note", "live %d peak %.6f" % (live, peak))
        rep.check(start_sndiod(), "S4 sndiod started again")
        app.send("midirescan")
        st, t = app.wait(lambda s: open_name(s, thru), 20)
        if not rep.check(st is not None, "S4 Rescan reopens it without a restart", "%.1f s" % t if st else app.last["line"]):
            return
        midicat(app.home, "90 40 64", app.work)
        st, t = app.wait(lambda s: held(s, thru) >= 1, 10)
        rep.check(st is not None, "S4 and its notes arrive", "held %d" % held(st or app.last, thru))
        midicat(app.home, "80 40 00", app.work)
    finally:
        finish(rep, app, "S")
    rep.note("hot-plug on the BSDs", "not testable here: Gridline lists sndio devices at startup and on Rescan only (listing opens each "
             "device), and the VM has no USB MIDI port (rmidi) to plug in or out")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--app", required=True)
    ap.add_argument("--demo", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--helper", default="")
    ap.add_argument("--require-kernel", action="store_true", help="Linux: fail when snd-virmidi cannot be loaded")
    ap.add_argument("--require-sndiod", action="store_true", help="BSD: fail when sndiod cannot be run")
    opts = ap.parse_args()
    if os.environ.get("GRIDLINE_MIDI_REAL") != "1":
        log("midi-real: changes the machine's MIDI setup (kernel modules, sndiod); runs only with GRIDLINE_MIDI_REAL=1")
        return 2
    opts.app = os.path.abspath(opts.app)
    opts.out = os.path.abspath(opts.out)
    shutil.rmtree(opts.out, ignore_errors=True)
    os.makedirs(opts.out)
    linux_host = sys.platform.startswith("linux")
    rep = Report("MIDI through the real back end (%s, %s)" % ("ALSA" if linux_host else "sndio", os.uname().sysname))
    try:
        (linux if linux_host else sndio)(rep, opts)
    except Exception as e:  # anything unexpected is a failure with its reason, never a silent pass
        rep.fail("the run stopped", "%s: %s" % (type(e).__name__, e))
    rep.write(os.path.join(opts.out, "summary.md"))
    with open(os.path.join(opts.out, "summary.md")) as f:
        log("\n" + f.read())
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
