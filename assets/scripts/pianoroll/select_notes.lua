-- name: Select notes
-- category: Selection
-- description: Selects notes by a rule: every Nth note, a pitch or velocity range, notes on or off the beat, the top or bottom note of each chord, or a random share. The result can replace, narrow or extend the current selection.
-- author: Gridline

function form(f)
  f:combo("Rule", { "Every Nth note", "Pitch range", "Velocity range", "On the beat", "Off the beat",
                    "Top of each chord", "Bottom of each chord", "Random" }, 1)
  f:knobInt("N", 2, 1, 16, "Every Nth note")
  f:knobInt("Offset", 0, 0, 15, "Every Nth note: start counting here")
  f:knobInt("From", 48, 0, 127, "Lower end of the pitch / velocity range")
  f:knobInt("To", 72, 0, 127, "Upper end of the pitch / velocity range")
  f:knob("Chance", 0.5, 0, 1, "Random: share of notes selected")
  f:combo("Mode", { "Replace selection", "Narrow selection", "Add to selection" }, 1)
end

function apply(score, p)
  local rule = p["Rule"]
  local lo, hi = math.min(p["From"], p["To"]), math.max(p["From"], p["To"])
  -- Top / bottom pitch per start time.
  local top, bottom = {}, {}
  for _, n in score:notes() do
    top[n.time] = math.max(top[n.time] or 0, n.pitch)
    bottom[n.time] = math.min(bottom[n.time] or 127, n.pitch)
  end
  local hits = {}
  for i, n in score:notes() do
    local hit
    if rule == 1 then hit = (i - 1 - p["Offset"]) % p["N"] == 0 and i - 1 >= p["Offset"]
    elseif rule == 2 then hit = n.pitch >= lo and n.pitch <= hi
    elseif rule == 3 then hit = n.velocity >= lo and n.velocity <= hi
    elseif rule == 4 then hit = n.time % score.ppq == 0
    elseif rule == 5 then hit = n.time % score.ppq ~= 0
    elseif rule == 6 then hit = n.pitch == top[n.time]
    elseif rule == 7 then hit = n.pitch == bottom[n.time]
    else hit = math.random() < p["Chance"]
    end
    hits[i] = hit
  end
  local mode = p["Mode"]
  for i, n in score:notes() do
    if mode == 1 then n.selected = hits[i]
    elseif mode == 2 then n.selected = n.selected and hits[i]
    else n.selected = n.selected or hits[i]
    end
  end
end
