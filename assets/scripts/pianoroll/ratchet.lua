-- name: Ratchet
-- category: Rhythm
-- description: Splits each note into fast repeats over its length (a ratchet or roll) that can fade in or out and climb in pitch.
-- author: Gridline

function form(f)
  f:knobInt("Repeats", 4, 2, 16, "Pieces per note")
  f:combo("Velocity", { "Even", "Fade out", "Fade in" }, 2)
  f:knob("Fade amount", 0.5, 0, 1)
  f:knob("Gate", 0.6, 0.1, 1, "Length of each piece relative to its slot")
  f:knobInt("Pitch step", 0, -12, 12, "Semitones added on every repeat")
end

function apply(score, p)
  local idx = score:selection()
  local repeats = p["Repeats"]
  local made = {}
  -- Walk backwards so removing notes keeps the remaining indices valid.
  for k = #idx, 1, -1 do
    local n = score:get(idx[k])
    local slot = n.length / repeats
    if slot >= 1 then
      score:remove(idx[k])
      for r = 0, repeats - 1 do
        local x = repeats > 1 and r / (repeats - 1) or 0
        local v = n.velocity
        if p["Velocity"] == 2 then v = v * (1 - p["Fade amount"] * x) end
        if p["Velocity"] == 3 then v = v * (1 - p["Fade amount"] * (1 - x)) end
        local piece = score:newNote(n)
        piece.time = n.time + math.floor(r * slot + 0.5)
        piece.length = math.max(1, math.floor(slot * p["Gate"] + 0.5))
        piece.velocity = math.max(1, math.min(127, math.floor(v + 0.5)))
        piece.pitch = math.max(0, math.min(127, n.pitch + r * p["Pitch step"]))
        made[#made + 1] = piece
      end
    end
  end
  for _, piece in ipairs(made) do score:add(piece) end
end
