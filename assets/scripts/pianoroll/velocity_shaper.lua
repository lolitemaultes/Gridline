-- name: Velocity shaper
-- category: Velocity
-- description: Redraws the velocities of the notes along a shape over time: ramp up, ramp down, swell, dip, or accents on the beats or off-beats.
-- author: Gridline

function form(f)
  f:combo("Shape", { "Ramp up", "Ramp down", "Swell", "Dip", "Accent beats", "Accent off-beats" }, 1)
  f:knobInt("Low", 50, 1, 127)
  f:knobInt("High", 115, 1, 127)
  f:knob("Curve", 1, 0.25, 4, "Below 1 rises fast, above 1 rises late")
  f:knob("Mix", 1, 0, 1, "Blend with the current velocities")
end

function apply(score, p)
  local notes = score:targets()
  if #notes == 0 then return end
  local t0, t1 = math.huge, -math.huge
  for _, n in ipairs(notes) do
    t0 = math.min(t0, n.time)
    t1 = math.max(t1, n.time)
  end
  local shape = p["Shape"]
  for _, n in ipairs(notes) do
    local x = t1 > t0 and (n.time - t0) / (t1 - t0) or 0
    local y
    if shape == 1 then y = x
    elseif shape == 2 then y = 1 - x
    elseif shape == 3 then y = math.sin(math.pi * x)
    elseif shape == 4 then y = 1 - math.sin(math.pi * x)
    elseif shape == 5 then y = n.time % score.ppq == 0 and 1 or 0
    else y = n.time % score.ppq == 0 and 0 or 1
    end
    y = y ^ p["Curve"]
    local v = p["Low"] + (p["High"] - p["Low"]) * y
    v = p["Mix"] * v + (1 - p["Mix"]) * n.velocity
    n.velocity = math.max(1, math.min(127, math.floor(v + 0.5)))
  end
end
