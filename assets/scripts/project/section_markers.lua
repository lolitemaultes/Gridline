-- name: Section markers
-- category: Arrangement
-- description: Adds a marker every few bars over the song (or the time selection), numbered or named after song parts.
-- author: Gridline

function form(f)
  f:knobInt("Every", 8, 1, 64, "Bars between markers")
  f:combo("Names", { "Numbered", "Song parts" }, 1)
  f:check("Replace markers", false, "Remove the existing markers first")
end

local P = project
local parts = { "Intro", "Verse", "Chorus", "Verse", "Chorus", "Bridge", "Chorus", "Outro" }

function apply(p)
  if p["Replace markers"] then
    for i = P.markers.count(), 1, -1 do P.markers.remove(i) end
  end
  local a, b = P.selection.time()
  if not a then a, b = 0, math.max(P.song.length(), P.song.bar()) end
  local span = P.song.bar() * p["Every"]
  local k, t = 0, a
  while t < b and k < 500 do
    k = k + 1
    local name = p["Names"] == 2 and (parts[k] or ("Part " .. k)) or ("Section " .. k)
    P.markers.add { time = t, name = name }
    t = t + span
  end
  print(("Added %d markers"):format(k))
end
