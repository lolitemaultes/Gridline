-- name: Tone generator
-- category: Generate
-- description: Writes a test tone: replaces the selection, or is inserted at the cursor when nothing is selected.
-- author: Gridline
-- target: cursor

function form(f)
  f:combo("Shape", { "Sine", "Square", "Saw", "Triangle" }, 1)
  f:knob("Note", 69, 12, 120, "MIDI note (69 = A at 440 Hz)")
  f:knob("Fine", 0, -100, 100, "Detune in cents")
  f:knob("Level", -12, -60, 0, "Level in dB")
  f:knob("Length", 1, 0.05, 30, "Seconds (used when inserting at the cursor)")
  f:knob("Edge fade", 5, 0, 100, "Fade in / out in milliseconds")
end

local shapes = { "sine", "square", "saw", "triangle" }

function apply(audio, p)
  if not audio.selected then audio:resize(math.floor(p["Length"] * audio.rate + 0.5)) end
  local hz = dsp.noteToHz(p["Note"] + p["Fine"] / 100)
  local amp = dsp.dbToGain(p["Level"])
  local edge = math.min(audio.frames // 2, math.floor(p["Edge fade"] * audio.rate / 1000))
  for _, buf in audio:buffers() do
    buf:tone(shapes[p["Shape"]], hz, audio.rate, amp)
    if edge > 1 then
      buf:ramp(0, 1, "linear", 1, edge)
      buf:ramp(1, 0, "linear", audio.frames - edge + 1, audio.frames)
    end
  end
end
