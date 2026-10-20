-- name: Humanize playlist selection
-- category: Playlist
-- description: Nudges the selected playlist clips (or every clip starting inside the time selection) by small random amounts and varies the gain of audio clips, so a copied arrangement feels less mechanical.
-- author: Gridline

function form(f)
  f:knob("Timing", 0.25, 0, 1, "Largest start shift, as a share of a 1/16 step")
  f:knob("Gain", 0.1, 0, 0.5, "Largest gain change of audio clips (share of the clip gain)")
  f:check("Keep bar starts", true, "Clips that start exactly on a bar stay where they are")
end

local P = project
local function wobble() return math.random() * 2 - 1 end

-- The clips to work on: the selection, else the clips starting in the time selection.
local function targets()
  local ids = P.selection.clips()
  if #ids > 0 then return ids end
  local a, b = P.selection.time()
  local out = {}
  if a then
    for _, c in ipairs(P.playlist.clips()) do
      if c.start >= a and c.start < b then out[#out + 1] = c.id end
    end
  end
  return out
end

function apply(p)
  local ids = targets()
  if #ids == 0 then
    print("Select some playlist clips (or a time range) first.")
    return
  end
  local step = P.song.beat() / 4
  local bar = P.song.bar()
  for _, id in ipairs(ids) do
    local c = P.playlist.clip(id)
    if not (p["Keep bar starts"] and c.start % bar == 0) then
      local start = math.max(0, math.floor(c.start + wobble() * p["Timing"] * step + 0.5))
      P.playlist.moveClip(id, c.track, start)
    end
    if c.kind == "audio" and p["Gain"] > 0 then
      P.playlist.setClip(id, { gain = math.max(0, c.gain * (1 + wobble() * p["Gain"])) })
    end
  end
  print(("Humanized %d clips"):format(#ids))
end
