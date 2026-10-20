-- name: Fade in (shaped)
-- category: Amplitude
-- description: Fades the selection in from silence (or from a start level) with a choice of curve.
-- author: Gridline

function form(f)
  f:combo("Curve", { "Straight", "Fast start", "Slow start", "S-curve", "Equal power" }, 3)
  f:knob("Start level", 0, 0, 1, "Gain at the first sample (0 = silence)")
end

local curves = { "linear", "fast", "slow", "s", "power" }

function apply(audio, p)
  for _, buf in audio:buffers() do
    buf:ramp(p["Start level"], 1, curves[p["Curve"]])
  end
end
