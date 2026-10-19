-- name: Bass from chords
-- category: Harmony
-- description: Writes a bass line under the chords: the lowest note of each chord becomes the root, played in the rhythm you choose. The new notes end up selected, ready to move to a bass channel.
-- author: Gridline

function form(f)
  f:combo("Rhythm", { "Held", "Quarter notes", "Eighth notes", "Octave bounce", "Root and fifth", "Walk to next chord" }, 2)
  f:knobInt("Octaves down", 1, 0, 3, "How far below the chord's lowest note")
  f:knobInt("Velocity", 104, 1, 127)
  f:knob("Gate", 0.85, 0.1, 1)
end

function apply(score, p)
  -- Chords: notes grouped by start; root = lowest pitch; a chord lasts until the next one.
  local roots, starts, ends = {}, {}, {}
  for _, n in ipairs(score:targets()) do
    if not roots[n.time] then starts[#starts + 1] = n.time end
    roots[n.time] = math.min(roots[n.time] or 127, n.pitch)
    ends[n.time] = math.max(ends[n.time] or 0, n.time + n.length)
  end
  table.sort(starts)
  for _, n in score:notes() do n.selected = false end
  local beat, eighth = score.ppq, score.ppq // 2
  local shift = 12 * p["Octaves down"]
  local function add(t, len, pitch)
    score:add {
      time = t,
      length = math.max(1, math.floor(len * p["Gate"] + 0.5)),
      pitch = math.max(0, math.min(127, pitch)),
      velocity = p["Velocity"],
      selected = true,
    }
  end
  for i, t in ipairs(starts) do
    local stop = starts[i + 1] or ends[t]
    local root = roots[t] - shift
    local nextRoot = starts[i + 1] and roots[starts[i + 1]] - shift or root
    local r = p["Rhythm"]
    if r == 1 then
      add(t, stop - t, root)
    else
      local step = (r == 3 or r == 4) and eighth or beat
      local k = 0
      local pos = t
      while pos < stop do
        local len = math.min(step, stop - pos)
        local pitch = root
        if r == 4 and k % 2 == 1 then pitch = root + 12 end
        if r == 5 and k % 2 == 1 then pitch = root + 7 end
        if r == 6 and pos + step >= stop and nextRoot ~= root then
          pitch = nextRoot + (nextRoot > root and -1 or 1)  -- approach note
        end
        add(pos, len, pitch)
        pos = pos + step
        k = k + 1
      end
    end
  end
end
