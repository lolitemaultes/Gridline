-- name: Reverse (click-free)
-- category: Time
-- description: Plays the selection backwards and adds very short fades at both edges so the cut points do not click.
-- author: Gridline

function form(f)
  f:knob("Edge fade", 5, 0, 50, "Fade length at each edge, in milliseconds")
end

function apply(audio, p)
  local n = math.min(audio.frames // 2, math.floor(p["Edge fade"] * audio.rate / 1000))
  for _, buf in audio:buffers() do
    buf:reverse()
    if n > 1 then
      buf:ramp(0, 1, "s", 1, n)
      buf:ramp(1, 0, "s", audio.frames - n + 1, audio.frames)
    end
  end
end
