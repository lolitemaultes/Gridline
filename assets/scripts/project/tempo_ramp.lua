-- name: Tempo ramp
-- category: Automation
-- description: Creates a tempo automation clip that moves from one tempo to another over the time selection (or the first bars) and places it on a free playlist track.
-- author: Gridline

function form(f)
  f:knob("From", 120, 40, 240, "Tempo at the start (BPM)")
  f:knob("To", 140, 40, 240, "Tempo at the end (BPM)")
  f:knobInt("Bars", 8, 1, 64, "Length when nothing is selected")
  f:knob("Curve", 0, -1, 1, "Bend of the ramp")
end

local P = project

local function freeTrack()
  local used = {}
  for _, c in ipairs(P.playlist.clips()) do used[c.track] = true end
  for t = 1, P.playlist.trackCount() do
    if not used[t] then return t end
  end
  return P.playlist.trackCount()
end

function apply(p)
  local tempo = P.params.song("tempo")
  local a, b = P.selection.time()
  if not a then a, b = 0, P.song.bar() * p["Bars"] end
  local clip = P.automation.add(tempo, "Tempo ramp", {
    { time = 0, value = P.params.toNormalized(tempo, p["From"]) },
    { time = b - a, value = P.params.toNormalized(tempo, p["To"]), tension = p["Curve"] },
  })
  P.playlist.addAutomationClip(clip, freeTrack(), a, b - a)
end
