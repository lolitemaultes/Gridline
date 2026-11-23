/* Gridline CI helper: a user-space ALSA sequencer client with one readable MIDI port, as a USB
 * keyboard's driver would offer, for tools/ci/midi-real.py. It reads lines of hex bytes on stdin
 * ("90 3c 64") and sends each to the port's subscribers; "quit" or the end of stdin removes the
 * client (the device is unplugged). Linux only, built by CMake when GRIDLINE_MIDI_REAL=1.
 *   midi-seq-port "<client name>"
 */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  const char* name = argc > 1 ? argv[1] : "Gridline CI Keys";
  snd_seq_t* seq = NULL;
  int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0);
  if (err < 0) {
    fprintf(stderr, "midi-seq-port: cannot open the ALSA sequencer: %s\n", snd_strerror(err));
    return 1;
  }
  snd_seq_set_client_name(seq, name);
  const int port = snd_seq_create_simple_port(seq, "Keys", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                              SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
  if (port < 0) {
    fprintf(stderr, "midi-seq-port: cannot create the port: %s\n", snd_strerror(port));
    return 1;
  }
  snd_midi_event_t* enc = NULL;
  if (snd_midi_event_new(256, &enc) < 0) {
    fprintf(stderr, "midi-seq-port: no MIDI event encoder\n");
    return 1;
  }
  printf("ready %d:%d\n", snd_seq_client_id(seq), port);
  fflush(stdout);
  char line[512];
  while (fgets(line, sizeof line, stdin)) {
    if (strncmp(line, "quit", 4) == 0) break;
    int events = 0;
    snd_midi_event_reset_encode(enc);
    for (char* tok = strtok(line, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
      const int b = (int)strtol(tok, NULL, 16);
      snd_seq_event_t ev;
      snd_seq_ev_clear(&ev);
      if (snd_midi_event_encode_byte(enc, b, &ev) == 1) {
        snd_seq_ev_set_source(&ev, port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        if (snd_seq_event_output_direct(seq, &ev) >= 0) ++events;
      }
    }
    printf("sent %d\n", events);
    fflush(stdout);
  }
  snd_midi_event_free(enc);
  snd_seq_close(seq);
  return 0;
}
