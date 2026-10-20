-- name: Build a drum pattern
-- category: Patterns
-- description: Makes a new pattern with a basic beat. Channels named kick, snare, clap or hat get step sequencer hits; without them a drum kit channel is added and the beat is written as notes.
-- author: Gridline

function form(f)
  f:combo("Style", { "Four on the floor", "Breakbeat", "Half time", "Shuffle" }, 1)
  f:knobInt("Bars", 1, 1, 8, "Length of the new pattern")
  f:check("Place in playlist", true, "Adds a clip at the time selection (or after the song)")
end

local P = project

-- 16-step rows per style (x = hit, o = soft hit).
local styles = {
  { kick = "x...x...x...x...", snare = "....x.......x...", hat = "..x...x...x...x." },
  { kick = "x.........x.....", snare = "....x.......x..o", hat = "x.x.x.x.x.x.x.x." },
  { kick = "x.....x.........", snare = "........x.......", hat = "x.x.x.x.x.x.x.x." },
  { kick = "x.....x...x.....", snare = "....x.......x...", hat = "x..ox..ox..ox..o" },
}
local roles = { "kick", "snare", "hat" }
local notes = { kick = 36, snare = 38, hat = 42 }
local words = { kick = { "kick", "bd" }, snare = { "snare", "clap", "sd" }, hat = { "hat", "hh" } }

local function findChannel(role)
  for i = 1, P.channels.count() do
    local n = P.channels.name(i):lower()
    for _, w in ipairs(words[role]) do
      if n:find(w, 1, true) then return i end
    end
  end
end

local function velocity(ch) return ch == "x" and 100 or (ch == "o" and 60 or 0) end

function apply(p)
  local style = styles[p["Style"]]
  local bars = p["Bars"]
  local pat = P.patterns.add("Beat", bars)
  local found, any = {}, false
  for _, role in ipairs(roles) do
    found[role] = findChannel(role)
    any = any or found[role] ~= nil
  end
  if any then
    for _, role in ipairs(roles) do
      local ch = found[role]
      if ch then
        local row = {}
        for b = 0, bars - 1 do
          for s = 1, 16 do row[b * 16 + s] = velocity(style[role]:sub(s, s)) end
        end
        P.patterns.setSteps(pat, ch, row)
      end
    end
  else
    local ch = P.channels.add("Drum kit", "gl.drumkit")
    local list, step = {}, P.song.beat() // 4
    for b = 0, bars - 1 do
      for _, role in ipairs(roles) do
        for s = 1, 16 do
          local v = velocity(style[role]:sub(s, s))
          if v > 0 then
            list[#list + 1] = { time = (b * 16 + s - 1) * step, length = step, pitch = notes[role], velocity = v }
          end
        end
      end
    end
    P.patterns.setNotes(pat, ch, list)
  end
  if p["Place in playlist"] then
    local at = P.selection.time() or P.song.length()
    P.playlist.addPatternClip(pat, 1, at)
  end
  P.selection.setPattern(pat)
end
