-- name: Euclidean rhythm
-- category: Rhythm
-- description: Spreads a number of hits as evenly as possible over a cycle of steps and repeats the cycle over the pattern. Three voices, each with its own hits, cycle, rotation and note.
-- author: Gridline

local defaults = {
  { hits = 4, steps = 16, note = 36 },
  { hits = 3, steps = 8, note = 38 },
  { hits = 7, steps = 16, note = 42 },
}

function form(f)
  f:combo("Step", { "1/32", "1/16", "1/8", "1/4" }, 2, "Length of one step")
  f:check("Replace notes", true, "Clear the channel's notes first")
  for v, d in ipairs(defaults) do
    f:group("Voice " .. v)
    f:knobInt("Hits " .. v, d.hits, 0, 32, "Notes per cycle (0 = voice off)")
    f:knobInt("Steps " .. v, d.steps, 1, 32, "Steps per cycle")
    f:knobInt("Rotate " .. v, 0, 0, 31, "Shifts the cycle by whole steps")
    f:knobInt("Note " .. v, d.note, 0, 127, "Pitch of this voice")
    f:knobInt("Velocity " .. v, 100, 1, 127)
  end
  f:endGroup()
end

-- true/false per step: step i is a hit when the running total of hits/steps
-- crosses a whole number there (an even spread of `hits` over `steps`).
local function cycle(hits, steps, rotate)
  local out = {}
  for i = 0, steps - 1 do
    local j = (i + rotate) % steps
    out[i + 1] = hits > 0 and (j * hits) % steps < hits
  end
  return out
end

function apply(score, p)
  local step = score.ppq * 4 / ({ 32, 16, 8, 4 })[p["Step"]]
  local total = score.length > 0 and score.length or score.bar
  if p["Replace notes"] then score:clear(true) end
  for v = 1, #defaults do
    local steps = p["Steps " .. v]
    local hits = math.min(p["Hits " .. v], steps)
    if hits > 0 then
      local c = cycle(hits, steps, p["Rotate " .. v] % steps)
      local i = 0
      while i * step < total do
        if c[i % steps + 1] then
          score:add {
            time = math.floor(i * step),
            length = math.max(1, math.floor(step * 0.9)),
            pitch = p["Note " .. v],
            velocity = p["Velocity " .. v],
            selected = true,
          }
        end
        i = i + 1
      end
    end
  end
end
