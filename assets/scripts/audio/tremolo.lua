-- name: Tremolo
-- category: Modulation
-- description: Modulates the level with a low-frequency wave; the right channel can run out of phase for an auto-pan.
-- author: Gridline

function form(f)
  f:knob("Rate", 4, 0.1, 30, "Cycles per second")
  f:knob("Depth", 0.6, 0, 1, "How far the level dips")
  f:combo("Wave", { "Sine", "Triangle", "Square" }, 1)
  f:check("Auto-pan", false, "Right channel half a cycle later")
end

local waves = { "sine", "triangle", "square" }

function apply(audio, p)
  local depth = p["Depth"]
  for c, buf in audio:buffers() do
    local lfo = buffer.new(audio.frames)
    local phase = (p["Auto-pan"] and c == 2) and 0.5 or 0
    lfo:tone(waves[p["Wave"]], p["Rate"], audio.rate, 0.5, phase)
    -- -0.5..0.5 -> 1 - depth .. 1
    lfo:offset(0.5)
    lfo:scale(depth)
    lfo:offset(1 - depth)
    buf:mul(lfo)
  end
end
