-- name: Chord progression
-- category: Harmony
-- description: Writes a progression of diatonic chords in a key, one chord per bar (or half bar), with a choice of voicing, sevenths and a bass note.
-- author: Gridline

local progressions = {
  { "I - V - vi - IV", { 1, 5, 6, 4 } },
  { "vi - IV - I - V", { 6, 4, 1, 5 } },
  { "I - vi - IV - V", { 1, 6, 4, 5 } },
  { "ii - V - I - I", { 2, 5, 1, 1 } },
  { "I - IV - V - IV", { 1, 4, 5, 4 } },
  { "I - iii - IV - V", { 1, 3, 4, 5 } },
  { "i - VI - III - VII", { 1, 6, 3, 7 } },
  { "i - iv - v - i", { 1, 4, 5, 1 } },
}
local modes = { { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 } }
local keys = { "Piano roll key", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }

function form(f)
  local names = {}
  for i, pr in ipairs(progressions) do names[i] = pr[1] end
  f:combo("Progression", names, 1)
  f:combo("Key", keys, 1, "Root of the key")
  f:combo("Mode", { "Major", "Minor" }, 1)
  f:combo("Chord length", { "1 bar", "2 bars", "1/2 bar" }, 1)
  f:knobInt("Bars", 4, 1, 32, "Total length; the progression repeats")
  f:knobInt("Octave", 5, 3, 7, "Octave of the chord roots (C5 = middle C)")
  f:combo("Voicing", { "Close", "First inversion", "Open" }, 1)
  f:check("Sevenths", false, "Add the diatonic seventh")
  f:check("Bass note", true, "Add the root an octave below")
  f:knobInt("Velocity", 96, 1, 127)
  f:check("Replace notes", true)
end

function apply(score, p)
  local root = p["Key"] == 1 and score.root or p["Key"] - 2
  local steps = modes[p["Mode"]]
  local base = 12 * p["Octave"] + root
  -- Pitch of scale degree d (1 = root; 8 = an octave up).
  local function degree(d)
    local i = d - 1
    return base + 12 * (i // 7) + steps[i % 7 + 1]
  end
  local chordLen = ({ score.bar, score.bar * 2, score.bar // 2 })[p["Chord length"]]
  local total = score.bar * p["Bars"]
  local prog = progressions[p["Progression"]][2]
  if p["Replace notes"] then score:clear(true) end
  local t, k = 0, 0
  while t < total do
    local d = prog[k % #prog + 1]
    local pitches = { degree(d), degree(d + 2), degree(d + 4) }
    if p["Sevenths"] then pitches[#pitches + 1] = degree(d + 6) end
    if p["Voicing"] == 2 then
      pitches[1] = pitches[1] + 12
    elseif p["Voicing"] == 3 then
      pitches[2] = pitches[2] + 12
    end
    if p["Bass note"] then pitches[#pitches + 1] = degree(d) - 12 end
    local len = math.min(chordLen, total - t)
    for _, pitch in ipairs(pitches) do
      score:add { time = t, length = len, pitch = pitch, velocity = p["Velocity"], selected = true }
    end
    t = t + chordLen
    k = k + 1
  end
end
