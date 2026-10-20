-- name: Fade out (shaped)
-- category: Amplitude
-- description: Fades the selection out to silence (or to an end level) with a choice of curve.
-- author: Gridline

function form(f)
  f:combo("Curve", { "Straight", "Fast end", "Slow end", "S-curve", "Equal power" }, 3)
  f:knob("End level", 0, 0, 1, "Gain at the last sample (0 = silence)")
end

local curves = { "linear", "fast", "slow", "s", "power" }

function apply(audio, p)
  for _, buf in audio:buffers() do
    -- A fade-out is a fade-in played backwards.
    buf:reverse()
    buf:ramp(p["End level"], 1, curves[p["Curve"]])
    buf:reverse()
  end
end
