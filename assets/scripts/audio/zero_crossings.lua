-- name: Snap selection to zero crossings
-- category: Edit
-- description: Moves both edges of the selection to the nearest rising zero crossing of the left channel (clean loop and cut points) and can mark every crossing inside it.
-- author: Gridline

function form(f)
  f:knob("Search", 10, 1, 100, "How far to look from each edge, in milliseconds")
  f:check("Mark crossings", false, "Adds a marker at every rising zero crossing in the selection (at most 200)")
end

-- Nearest rising crossing to file position `pos`, searched in the original audio.
local function nearest(audio, pos, radius)
  local from = math.max(0, pos - radius)
  local ctx = audio:read(1, from, math.min(audio.fileFrames, pos + radius + 1))
  local best, dist = nil, math.huge
  for _, i in ipairs(ctx:zeroCrossings("up")) do
    local at = from + i - 1
    local d = math.abs(at - pos)
    if d < dist then best, dist = at, d end
  end
  return best or pos
end

function apply(audio, p)
  if not audio.selected then
    print("Select a range first.")
    return
  end
  local radius = math.floor(p["Search"] * audio.rate / 1000)
  local a = nearest(audio, audio.start, radius)
  local b = nearest(audio, audio.start + audio.frames, radius)
  if p["Mark crossings"] then
    local list = audio:channel(1):zeroCrossings("up", 200)
    for k, i in ipairs(list) do audio:addMarker(i - 1, "Zero " .. k) end
  end
  -- select() positions count from the selection start and may reach outside it.
  audio:select(a - audio.start, b - audio.start)
  print(("Selection %d .. %d"):format(a, b))
end
