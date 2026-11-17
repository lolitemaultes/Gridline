# Gridline - K-106 cp18 (K-316 plan 3, 5, 6, 8): the Tracker's lower-band screens in the real app.
#   Navigation: a Tracker Sampler opens the Sample editor; another instrument the Channel frame;
#   Esc, Ctrl+Alt+E again and Exit close it; Open plugin window floats the channel settings and the
#   band stays closed.
#   The Sample editor: zoom in, zoom to a range, the whole sample; play a part (the play tap's line);
#   Alt+A selects all while it has the keys; each edit is one undo step and undoing them all saves
#   the same bytes as before.
#   The Channel frame: pitch, fine and key range through the channel's own undo steps; Convert to
#   Tracker Sampler... asks, converts as one step, and undo brings the Sampler back byte for byte.
#   Looking only (open, zoom, range, play, close) saves the same bytes.
#   cp20: a right-drag across the wave draws (one undo step; a right-click alone opens the menu), the
#   Instrument editor's Extra page sets the playback values (one step each), a sounding note draws its
#   play line on the volume envelope.
#   cmake -DAPP=<gridline-app> -DDEMO=<song> -DWAV=<audio file> -DOUT=<dir> -P k316_tracker_editors_app.cmake
cmake_minimum_required(VERSION 3.16)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/cfg/gridline" "${OUT}/home")
set(ENV{SDL_AUDIODRIVER} "dummy")
set(ENV{GRIDLINE_NO_PLUGIN_SCAN} "1")
set(ENV{GRIDLINE_CONFIG_HOME} "${OUT}/cfg")
set(ENV{HOME} "${OUT}/home")
file(WRITE "${OUT}/cfg/gridline/settings.json"
     "{\"format\": \"gridline-settings\", \"values\": {\"ui.layoutChosen\": true, \"ui.layoutDev\": true, \"ui.layout\": \"tracker\", \"keys.profile\": \"tracker\", \"ui.menuStyle\": \"tracker\"}}\n")
configure_file("${DEMO}" "${OUT}/song.gridline.json" COPYONLY)
configure_file("${WAV}" "${OUT}/smp.wav" COPYONLY)

function(fail what)
  message(FATAL_ERROR "FAIL: ${what}\n(the run's log: ${OUT}/run.log)")
endfunction()

set(E "tracker editors")
file(WRITE "${OUT}/input.txt" "5 toasts clear
6 ${E} open frame 7
8 replace gl.tracker
10 ${E} load \"${OUT}/smp.wav\"
12 addvia filerack \"${OUT}/smp.wav\"
16 ${E} close
20 save \"${OUT}/before.gridline.json\"
22 ${E} open sample 7
26 ${E} dump
28 ${E} zoom in
30 ${E} dump
32 ${E} range 1000 30000
33 ${E} zoom range
34 ${E} dump
36 ${E} zoom all
37 ${E} note 49
38 ${E} play range
41 ${E} dump
46 ${E} play stop
48 ${E} close
50 ${E} dump
52 save \"${OUT}/looked.gridline.json\"
54 ${E} open sample 7
56 ${E} focus
58 sdlkey a down alt
59 sdlkey a up alt
62 ${E} dump
64 sdlkey escape down
65 sdlkey escape up
68 ${E} dump
70 ${E} open sample 7
72 ${E} range 10000 20000
73 ${E} dump
74 ${E} op reverse
76 ${E} op fadein
78 ${E} loop forward
80 ${E} bits 8
82 ${E} pin 2 15000
84 ${E} modal volume 50 100
86 ${E} range 10000 20000
87 ${E} op cut
89 ${E} op paste
91 ${E} dump
92 undo
93 undo
94 undo
95 undo
96 undo
97 undo
98 undo
99 undo
101 ${E} dump
102 save \"${OUT}/undone.gridline.json\"
104 ${E} open frame 8
106 ${E} dump
107 ${E} frame pitch 3
108 ${E} frame fine 12
109 ${E} frame keylo 30
111 ${E} dump
112 undo
113 undo
114 undo
116 ${E} convert
120 key enter
124 ${E} dump
126 undo
128 ${E} dump
130 save \"${OUT}/unconverted.gridline.json\"
132 ${E} plugin 8
136 ${E} dump
138 ${E} close
140 ${E} open sample 7
142 ${E} close
144 sdlkey e down ctrl+alt
145 sdlkey e up ctrl+alt
148 ${E} dump
150 sdlkey e down ctrl+alt
151 sdlkey e up ctrl+alt
154 ${E} dump
156 sdlkey #224 up
160 ${E} open instrument 7 1
162 ${E} load \"${OUT}/smp.wav\"
165 save \"${OUT}/before_ins.gridline.json\"
166 ${E} ins dump
168 ${E} ins at env vol 3 20
169 sdldown 0
171 sdlup 0
173 ${E} ins dump
175 ${E} ins at env vol 6 0
176 sdldown 0
178 ${E} ins at env vol 8 20
180 ${E} ins at env vol 10 30
182 sdlup 0
184 ${E} ins dump
186 ${E} ins at key 24
187 sdldown 0
189 ${E} ins at key 27
191 ${E} ins at key 30
193 sdlup 0
195 ${E} ins dump
197 ${E} focus
199 sdlkey insert down
200 sdlkey insert up
202 sdlkey tab down
203 sdlkey tab up
205 sdlkey insert down
206 sdlkey insert up
208 sdlkey tab down
209 sdlkey tab up
211 sdlkey up down shift
212 sdlkey up up shift
214 ${E} ins dump
216 ${E} ins env vol sustain 1
217 ${E} ins env vol loop 1 2
218 ${E} ins env vol preset 2
219 ${E} ins set rel 5
220 ${E} ins set fine -20
221 ${E} ins set fadeout 300
222 ${E} ins set fadeout 310
223 ${E} ins env vol end
224 ${E} ins set shape 2
225 ${E} ins away
227 ${E} ins dump
229 undo
230 undo
231 undo
232 undo
233 undo
234 undo
235 undo
236 undo
237 undo
238 undo
239 undo
240 undo
241 undo
242 undo
243 undo
250 ${E} ins dump
252 save \"${OUT}/undone_ins.gridline.json\"
254 sdlkey escape down
255 sdlkey escape up
258 ${E} ins dump
260 ${E} open sample 7 1
262 ${E} view 0 4000
264 save \"${OUT}/before_pencil.gridline.json\"
265 ${E} dump
266 sdlmove 300 600
267 sdldown 1
269 sdlmove 500 700
271 sdlmove 700 560
273 sdlup 1
276 ${E} dump
277 sdlmove 900 650
278 sdldown 1
279 sdlup 1
282 ${E} dump
283 closepopups
285 undo
287 ${E} dump
288 save \"${OUT}/pencil_undone.gridline.json\"
290 ${E} open instrument 7 1
292 ${E} ins extra on
293 ${E} ins extra set interp 1
294 ${E} ins extra set ticks 9
296 ${E} ins dump
297 undo
298 undo
300 save \"${OUT}/extra_undone.gridline.json\"
302 noteon 7 60
310 ${E} ins dump
312 noteoff 7 60
")
execute_process(COMMAND "${APP}" "${OUT}/song.gridline.json" --hidden --no-ui-sounds --video-clock 60 --size 1440x900 --layout tracker
                        --dev-input "${OUT}/input.txt" --fixed-dt 16 --frames 316 --screenshot "${OUT}/end.ppm"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
file(WRITE "${OUT}/run.log" "${out}\n---- stderr ----\n${err}")
if(NOT rc EQUAL 0)
  fail("gridline-app failed (${rc})")
endif()

macro(line frame)
  if(NOT out MATCHES "tracker editors frame ${frame}: ([^\n]*)")
    fail("no report on frame ${frame}")
  endif()
  set(L "${CMAKE_MATCH_1}")
endmacro()
macro(want what msg)
  if(NOT L MATCHES "${what}")
    fail("frame ${f}: ${msg} (${L})")
  endif()
endmacro()

# ---- the Sample editor opens, zooms, plays a part ----
set(f 26)
line(${f})
want("shown 1 screen 1 want 1 channel 7 tracker 1 slot 0 frames ([0-9]+) " "the Sample editor on the Tracker Sampler with its file")
string(REGEX MATCH "frames ([0-9]+)" _ "${L}")
set(frames "${CMAKE_MATCH_1}")
if(frames LESS 20000)
  fail("the loaded file has ${frames} frames")
endif()
want("view 0 ${frames} " "the whole sample in view at first")
want("keys 1 " "opened by its key, the screen has the keys")
want("need [1-9][0-9][0-9]" "the band's height asked for")
set(f 30)
line(${f})
math(EXPR half "${frames} / 2")
if(NOT L MATCHES "view [0-9]+ (${half}|[0-9]+) ")
  fail("zoom in")
endif()
string(REGEX MATCH "view ([0-9]+) ([0-9]+) " _ "${L}")
math(EXPR d "${CMAKE_MATCH_2} - ${half}")
if(d GREATER 1 OR d LESS -1)
  fail("zoom in halves the view (${CMAKE_MATCH_2} of ${frames})")
endif()
set(f 34)
line(${f})
want("range 1000 30000 view 1000 29000 " "zoom to range")
set(f 41)
line(${f})
want("previewing 1 lines 1 " "the range plays, and its play line shows")
set(f 50)
line(${f})
want("shown 0 " "Exit closes")

# ---- keys ----
set(f 62)
line(${f})
want("range 0 ${frames} " "Alt+A selects the whole sample")
set(f 68)
line(${f})
want("shown 0 " "Esc closes")

# ---- edits, one undo step each ----
set(f 73)
line(${f})
string(REGEX MATCH "pos ([0-9]+) " _ "${L}")
set(pos0 "${CMAKE_MATCH_1}")
set(f 91)
line(${f})
math(EXPR pos8 "${pos0} + 8")
want("loop 1 [0-9]+ [0-9]+ bits 8 " "the loop type and 8-bit play changed")
want("pos ${pos8} " "eight edits, eight undo steps (from ${pos0})")
want("undo \"paste into sample\"" "the last step is the paste")
set(f 101)
line(${f})
want("pos ${pos0} " "eight undos back")
want("loop 0 [0-9]+ [0-9]+ bits 16 " "undo brings back the loop type and 16-bit play")
file(READ "${OUT}/before.gridline.json" before)
file(READ "${OUT}/looked.gridline.json" looked)
file(READ "${OUT}/undone.gridline.json" undone)
file(READ "${OUT}/unconverted.gridline.json" unconverted)
if(NOT before STREQUAL looked)
  fail("looking (open, zoom, range, play, close) changed the saved song")
endif()
if(NOT before STREQUAL undone)
  fail("the edits undone do not save the same bytes")
endif()

# ---- the Channel frame, Convert ----
set(f 106)
line(${f})
want("shown 1 screen 3 [^\n]* channel 8 tracker 0 " "a Sampler shows the Channel frame")
if(NOT out MATCHES "tracker editors frame 111: channel gain [0-9.]+ pan [-0-9.]+ pitch 3 fine 12 mute 0 out [0-9]+ keys 30 [0-9]+ type gl.sampler")
  fail("the Channel frame's pitch, fine and key range")
endif()
set(f 124)
line(${f})
want("channel 8 tracker 1 slot 0 frames ${frames} " "Convert to Tracker Sampler made a Tracker Sampler with the file")
want("undo \"convert to Tracker Sampler\"" "Convert is one undo step")
set(f 128)
line(${f})
want("channel 8 tracker 0 " "undo brings the Sampler back")
if(NOT before STREQUAL unconverted)
  fail("Convert undone (and the frame's edits undone) do not save the same bytes")
endif()

# ---- the plugin window, Ctrl+Alt+E ----
set(f 136)
line(${f})
want("shown 0 [^\n]* pluginwin 1 " "Open plugin window floats the channel settings, the band closed")
set(f 148)
line(${f})
want("shown 1 screen 1 want 1 channel 7 " "Ctrl+Alt+E opens the Sample editor")
set(f 154)
line(${f})
want("shown 0 " "Ctrl+Alt+E again closes it")

# ---- K-106 cp19: the Instrument editor ----
set(f 166)
line(${f})
want("ins shown 1 screen 4 slot 1 vol [[]f3 n2 s0 l0-0 0:64,6:0[]] pan [[]f0 n0 " "the Instrument editor on slot 2 with the default envelopes")
want("keys 1 need [1-9][0-9][0-9] " "opened, it has the keys and asks for the band")
string(REGEX MATCH "pos ([0-9]+) " _ "${L}")
set(ipos0 "${CMAKE_MATCH_1}")
set(f 173)
line(${f})
want("vol [[]f3 n3 s0 l0-0 0:64,3:20,6:0[]]" "a click in the volume envelope adds a point there")
want("undo \"volume envelope\"" "the added point is a volume envelope step")
set(f 184)
line(${f})
want("vol [[]f3 n3 s0 l0-0 0:64,3:20,10:30[]]" "a drag moves the last point")
set(f 195)
line(${f})
string(REPEAT "0" 24 low)
string(REPEAT "0" 65 high)
want("map ${low}1111111${high} " "a drag along the keyboard maps keys 24 to 30 to this sample")
want("undo \"map keys\"" "the key map is one undo step")
set(f 214)
line(${f})
want("vol [[]f3 n4 [^]]*,26:38[]] pan [[]f1 n2 s0 l0-0 0:32,16:32[]]" "Insert adds a point, Tab and Insert switch the panning envelope on, Shift+Up raises")
set(f 227)
line(${f})
want("vol [[]f3 n4 s2 l0-0 0:0,24:64,32:56,64:0[]]" "a preset replaces the volume envelope")
want("fade 310 vib 2 64 2 8 smp 64 0 -20 5 " "fadeout, vibrato shape, finetune and relative note")
want("map 0+ " "keys away gives the keys back to the other sample")
string(REGEX MATCH "pos ([0-9]+) " _ "${L}")
math(EXPR steps "${CMAKE_MATCH_1} - ${ipos0}")
if(NOT steps EQUAL 15)
  fail("the instrument edits made ${steps} undo steps, not 15 (fadeout's two values are one)")
endif()
set(f 250)
line(${f})
want("pos ${ipos0} " "fifteen undos back")
want("vol [[]f3 n2 s0 l0-0 0:64,6:0[]] pan [[]f0 n0 " "undo brings back the envelopes")
file(READ "${OUT}/before_ins.gridline.json" before_ins)
file(READ "${OUT}/undone_ins.gridline.json" undone_ins)
if(NOT before_ins STREQUAL undone_ins)
  fail("the instrument edits undone do not save the same bytes")
endif()
set(f 258)
line(${f})
want("ins shown 0 " "Esc closes the Instrument editor")

# ---- K-106 cp20: the pencil, the Extra page, the envelopes' play lines ----
set(f 265)
line(${f})
string(REGEX MATCH "pos ([0-9]+) " _ "${L}")
set(ppos0 "${CMAKE_MATCH_1}")
set(f 276)
line(${f})
math(EXPR ppos1 "${ppos0} + 1")
want("pos ${ppos1} undo \"draw sample\"" "a right-drag across the wave draws: one undo step")
set(f 282)
line(${f})
want("pos ${ppos1} " "a right-click without a drag opens the menu and draws nothing")
set(f 287)
line(${f})
want("pos ${ppos0} " "undo takes the stroke back")
file(READ "${OUT}/before_pencil.gridline.json" bp)
file(READ "${OUT}/pencil_undone.gridline.json" pu)
if(NOT bp STREQUAL pu)
  fail("the pencil stroke undone does not save the same bytes")
endif()
set(f 296)
line(${f})
math(EXPR ppos2 "${ppos0} + 2")
want("pos ${ppos2} " "the Extra page's interpolation and ticks: two undo steps")
file(READ "${OUT}/extra_undone.gridline.json" eu)
if(NOT bp STREQUAL eu)
  fail("the Extra page's edits undone do not save the same bytes")
endif()
set(f 310)
line(${f})
want("envlines [1-9] 0 " "a sounding note draws its play line on the volume envelope (the panning one is off)")
message(STATUS "k316_tracker_editors_app: ok (${frames} frames)")
