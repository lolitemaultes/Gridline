-- name: Humanize
-- category: Timing
-- description: Nudges note starts, lengths and velocities by small random amounts so a programmed part feels played. Works on the selected notes, or on all notes.
-- author: Gridline

function form(f)
  f:knob("Timing", 0.15, 0, 1, "Largest start shift, as a share of a 1/16 step")
  f:knob("Velocity", 0.12, 0, 1, "Largest velocity change, as a share of the full range")
  f:knob("Length", 0.1, 0, 1, "Largest length change, as a share of each note")
  f:combo("Feel", { "Centred", "Laid back", "Pushed" }, 1, "Lean the timing later or earlier")
  f:check("Keep notes on beats", true, "Notes that start exactly on a beat stay where they are")
end

-- Uniform random number in -1..1.
local function wobble() return math.random() * 2 - 1 end

local function round(x) return math.floor(x + 0.5) end

function apply(score, p)
  local step = score.ppq / 4
  local lean = ({ 0, 0.5, -0.5 })[p["Feel"]]
  for _, n in ipairs(score:targets()) do
    local onBeat = n.time % score.ppq == 0
    if not (p["Keep notes on beats"] and onBeat) then
      local shift = (wobble() + lean) * p["Timing"] * step
      n.time = math.max(0, round(n.time + shift))
    end
    n.velocity = math.max(1, math.min(127, round(n.velocity + wobble() * p["Velocity"] * 127)))
    n.length = math.max(1, round(n.length + wobble() * p["Length"] * n.length))
  end
end
