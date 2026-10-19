-- name: Mirror in scale
-- category: Melody
-- description: Turns a melody upside down around an axis note, step for step inside the scale, so the result stays in key.
-- author: Gridline

local scales = {
  { "Piano roll scale", nil },
  { "Major", { 0, 2, 4, 5, 7, 9, 11 } },
  { "Natural minor", { 0, 2, 3, 5, 7, 8, 10 } },
  { "Harmonic minor", { 0, 2, 3, 5, 7, 8, 11 } },
  { "Dorian", { 0, 2, 3, 5, 7, 9, 10 } },
  { "Major pentatonic", { 0, 2, 4, 7, 9 } },
  { "Minor pentatonic", { 0, 3, 5, 7, 10 } },
  { "Chromatic", { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
}
local keys = { "Piano roll key", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }

function form(f)
  local names = {}
  for i, s in ipairs(scales) do names[i] = s[1] end
  f:combo("Axis", { "Lowest note", "Highest note", "Middle", "Fixed note" }, 3, "The note the melody turns around")
  f:knobInt("Fixed axis", 60, 0, 127, "Axis for 'Fixed note' (60 = C5)")
  f:combo("Scale", names, 1)
  f:combo("Key", keys, 1)
end

-- Scale helpers: degree index <-> pitch over all octaves.
local function makeScale(steps, root)
  local k = #steps
  local s = {}
  function s.index(pitch)
    local rel = pitch - root
    local oct = rel // 12
    local within = rel % 12
    local i = 1
    for j = 1, k do if steps[j] <= within then i = j end end
    return oct * k + (i - 1)
  end
  function s.pitch(index)
    return root + 12 * (index // k) + steps[index % k + 1]
  end
  return s
end

function apply(score, p)
  local steps = scales[p["Scale"]][2] or (#score.scaleSteps > 0 and score.scaleSteps or scales[2][2])
  local root = p["Key"] == 1 and score.root or p["Key"] - 2
  local sc = makeScale(steps, root)
  local notes = score:targets()
  if #notes == 0 then return end
  local lo, hi = 127, 0
  for _, n in ipairs(notes) do
    lo = math.min(lo, n.pitch)
    hi = math.max(hi, n.pitch)
  end
  local axis = ({ lo, hi, (lo + hi) // 2, p["Fixed axis"] })[p["Axis"]]
  local a = sc.index(axis)
  for _, n in ipairs(notes) do
    local m = sc.pitch(2 * a - sc.index(n.pitch))
    n.pitch = math.max(0, math.min(127, m))
  end
end
