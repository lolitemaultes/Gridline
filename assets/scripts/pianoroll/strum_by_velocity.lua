-- name: Strum by velocity
-- category: Timing
-- description: Spreads the notes of each chord in time by their velocity: the loudest note lands first (or last) and softer notes trail behind, like a hand strum.
-- author: Gridline

function form(f)
  f:knob("Spread", 0.1, 0, 0.5, "Delay of the softest note, in beats")
  f:combo("Order", { "Loud first", "Soft first" }, 1)
  f:knob("Curve", 1, 0.25, 4, "Below 1 bunches the delays early, above 1 late")
  f:check("Keep ends", true, "Shorten delayed notes so they still end together")
end

function apply(score, p)
  -- Group the targeted notes by start time (chords).
  local groups, order = {}, {}
  for _, n in ipairs(score:targets()) do
    if not groups[n.time] then
      groups[n.time] = {}
      order[#order + 1] = n.time
    end
    table.insert(groups[n.time], n)
  end
  table.sort(order)
  local maxDelay = p["Spread"] * score.ppq
  for _, t in ipairs(order) do
    local g = groups[t]
    if #g > 1 then
      local lo, hi = 127, 0
      for _, n in ipairs(g) do
        lo = math.min(lo, n.velocity)
        hi = math.max(hi, n.velocity)
      end
      for _, n in ipairs(g) do
        local x = hi > lo and (hi - n.velocity) / (hi - lo) or 0
        if p["Order"] == 2 then x = 1 - x end
        local delay = math.floor((x ^ p["Curve"]) * maxDelay + 0.5)
        n.time = n.time + delay
        if p["Keep ends"] then n.length = math.max(1, n.length - delay) end
      end
    end
  end
end
