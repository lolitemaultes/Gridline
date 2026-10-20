-- name: Route drums to a bus
-- category: Mixer
-- description: Finds the drum channels by name, gives each one its own mixer insert (when it has none) and sends those inserts to one drum bus, which goes to the master.
-- author: Gridline

function form(f)
  f:text("Bus name", "Drum bus")
  f:text("Drum words", "kick snare clap hat tom perc cymbal ride crash rim shaker drum", "Channel names containing one of these words count as drums")
  f:check("Own insert per drum", true, "Off: the channels are routed to the bus directly")
end

local P = project

local function isDrum(name, words)
  name = name:lower()
  for w in words:gmatch("%S+") do
    if name:find(w:lower(), 1, true) then return true end
  end
  return false
end

local function findMixer(name)
  for m = 1, P.mixer.count() - 1 do
    if P.mixer.name(m) == name then return m end
  end
end

function apply(p)
  local drums = {}
  for i = 1, P.channels.count() do
    if isDrum(P.channels.name(i), p["Drum words"]) then drums[#drums + 1] = i end
  end
  if #drums == 0 then
    print("No drum channels found.")
    return
  end
  local bus = findMixer(p["Bus name"])
  if not bus then
    bus = P.mixer.add()
    P.mixer.setName(bus, p["Bus name"])
  end
  P.mixer.routeTo(bus, 0)
  for _, ch in ipairs(drums) do
    if p["Own insert per drum"] then
      local m = P.channels.route(ch)
      if m == 0 or m == bus then
        m = P.mixer.add()
        P.mixer.setName(m, P.channels.name(ch))
        P.mixer.setColor(m, P.channels.color(ch))
        P.channels.setRoute(ch, m)
      end
      P.mixer.routeTo(m, bus)
    else
      P.channels.setRoute(ch, bus)
    end
  end
  print(("%d drum channels go to '%s' (insert %d)"):format(#drums, p["Bus name"], bus))
end
