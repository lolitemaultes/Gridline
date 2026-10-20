-- name: Name tracks after their clips
-- category: Playlist
-- description: Renames each playlist track after the clip that covers most of it (pattern, sample channel or automation) and can take over that clip's colour.
-- author: Gridline

function form(f)
  f:check("Only unnamed tracks", false, "Keep tracks that already have a name of their own")
  f:check("Colour tracks", true, "Tracks take the colour of their main pattern or channel")
end

local P = project

local function isDefault(t) return P.playlist.trackName(t) == ("Track " .. t) end

function apply(p)
  local cover, colour = {}, {}
  for _, c in ipairs(P.playlist.clips()) do
    local t = c.track
    cover[t] = cover[t] or {}
    if c.name ~= "" then
      cover[t][c.name] = (cover[t][c.name] or 0) + c.length
      if c.kind == "pattern" and c.pattern then
        colour[t .. "|" .. c.name] = P.patterns.color(c.pattern)
      elseif c.kind == "audio" and c.channel then
        colour[t .. "|" .. c.name] = P.channels.color(c.channel)
      end
    end
  end
  local renamed = 0
  for t = 1, P.playlist.trackCount() do
    local best, most = nil, -1
    for name, len in pairs(cover[t] or {}) do
      if len > most or (len == most and name < best) then best, most = name, len end
    end
    if best and (not p["Only unnamed tracks"] or isDefault(t)) then
      P.playlist.setTrackName(t, best)
      if p["Colour tracks"] and colour[t .. "|" .. best] then P.playlist.setTrackColor(t, colour[t .. "|" .. best]) end
      renamed = renamed + 1
    end
  end
  print(("Renamed %d tracks"):format(renamed))
end
