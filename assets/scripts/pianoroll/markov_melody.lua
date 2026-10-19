-- name: Markov melody
-- category: Melody
-- description: Writes a new melody that moves like the notes already there. It learns which scale-step jumps tend to follow each other (a first-order Markov chain) and walks that chain; with fewer than three notes to learn from it uses a gentle stepwise model. New variation gives another take.
-- author: Gridline

local scales = {
  { "Piano roll scale", nil },
  { "Major", { 0, 2, 4, 5, 7, 9, 11 } },
  { "Natural minor", { 0, 2, 3, 5, 7, 8, 10 } },
  { "Dorian", { 0, 2, 3, 5, 7, 9, 10 } },
  { "Major pentatonic", { 0, 2, 4, 7, 9 } },
  { "Minor pentatonic", { 0, 3, 5, 7, 10 } },
}
local keys = { "Piano roll key", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }
local MAXJUMP = 7

function form(f)
  local names = {}
  for i, s in ipairs(scales) do names[i] = s[1] end
  f:knobInt("Bars", 2, 1, 16, "Length of the new melody")
  f:combo("Rhythm", { "1/8 notes", "1/16 notes", "Mixed", "Learned" }, 3)
  f:knob("Rests", 0.15, 0, 0.8, "Chance that a step stays silent")
  f:combo("Scale", names, 1)
  f:combo("Key", keys, 1)
  f:knobInt("Start note", 60, 24, 96)
  f:knobInt("Lowest", 48, 0, 127)
  f:knobInt("Highest", 84, 0, 127)
  f:knobInt("Velocity", 96, 1, 127)
  f:check("Replace notes", true, "Off: the melody is added after the existing notes")
end

local function makeScale(steps, root)
  local k = #steps
  local s = {}
  function s.index(pitch)
    local rel = pitch - root
    local i = 1
    for j = 1, k do if steps[j] <= rel % 12 then i = j end end
    return (rel // 12) * k + (i - 1)
  end
  function s.pitch(index) return root + 12 * (index // k) + steps[index % k + 1] end
  return s
end

-- Weighted choice from w[-MAXJUMP..MAXJUMP].
local function pick(w)
  local sum = 0
  for j = -MAXJUMP, MAXJUMP do sum = sum + (w[j] or 0) end
  if sum <= 0 then return 0 end
  local r = math.random() * sum
  for j = -MAXJUMP, MAXJUMP do
    r = r - (w[j] or 0)
    if r <= 0 then return j end
  end
  return MAXJUMP
end

function apply(score, p)
  local steps = scales[p["Scale"]][2] or (#score.scaleSteps > 0 and score.scaleSteps or scales[2][2])
  local root = p["Key"] == 1 and score.root or p["Key"] - 2
  local sc = makeScale(steps, root)

  -- Source melody: the highest note at each start time.
  local tops, times = {}, {}
  for _, n in ipairs(score:targets()) do
    if not tops[n.time] then times[#times + 1] = n.time end
    if not tops[n.time] or n.pitch > tops[n.time].pitch then tops[n.time] = n end
  end
  table.sort(times)

  -- Learn jump transitions and note lengths.
  local chain, lengths = {}, {}
  for i = 2, #times - 1 do
    local a = sc.index(tops[times[i]].pitch) - sc.index(tops[times[i - 1]].pitch)
    local b = sc.index(tops[times[i + 1]].pitch) - sc.index(tops[times[i]].pitch)
    a = math.max(-MAXJUMP, math.min(MAXJUMP, a))
    b = math.max(-MAXJUMP, math.min(MAXJUMP, b))
    chain[a] = chain[a] or {}
    chain[a][b] = (chain[a][b] or 0) + 1
  end
  for i = 1, #times - 1 do lengths[#lengths + 1] = times[i + 1] - times[i] end
  local fallback = { [-2] = 1, [-1] = 4, [0] = 1, [1] = 4, [2] = 1, [3] = 0.5, [-3] = 0.5 }

  local sixteenth = score.ppq // 4
  local function nextLength()
    local r = p["Rhythm"]
    if r == 1 then return 2 * sixteenth end
    if r == 2 then return sixteenth end
    if r == 4 and #lengths > 0 then return lengths[math.random(#lengths)] end
    return math.random() < 0.6 and 2 * sixteenth or (math.random() < 0.5 and sixteenth or 4 * sixteenth)
  end

  local startAt = 0
  if p["Replace notes"] then
    score:clear(true)
  else
    for _, n in score:notes() do startAt = math.max(startAt, n.time + n.length) end
    for _, n in score:notes() do n.selected = false end
  end
  local lo, hi = math.min(p["Lowest"], p["Highest"]), math.max(p["Lowest"], p["Highest"])
  local deg = sc.index(math.max(lo, math.min(hi, p["Start note"])))
  local jump = 0
  local t, stop = 0, score.bar * p["Bars"]
  while t < stop do
    local len = math.max(1, math.min(nextLength(), stop - t))
    if math.random() >= p["Rests"] then
      score:add {
        time = startAt + t,
        length = math.max(1, math.floor(len * 0.92)),
        pitch = sc.pitch(deg),
        velocity = math.max(1, math.min(127, p["Velocity"] + math.random(-8, 8))),
        selected = true,
      }
    end
    local nextJump = pick(chain[jump] or fallback)
    local candidate = sc.pitch(deg + nextJump)
    if candidate < lo or candidate > hi then nextJump = -nextJump end
    deg = deg + nextJump
    jump = nextJump
    t = t + len
  end
end
