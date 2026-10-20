-- name: Stereo width
-- category: Effects
-- description: Narrows or widens the stereo image by scaling the side (L - R) part against the mid (L + R) part.
-- author: Gridline

function form(f)
  f:knob("Width", 1.5, 0, 3, "0 = mono, 1 = unchanged, above 1 = wider")
  f:check("Keep level", true, "Scale down when widening would raise the peak level")
end

function apply(audio, p)
  if audio.channels < 2 then
    print("The file is mono.")
    return
  end
  local L, R = audio:channel(1), audio:channel(2)
  local before = math.max((L:peak()), (R:peak()))
  local mid, side = L:copy(), L:copy()
  mid:add(R)
  mid:scale(0.5)        -- (L + R) / 2
  side:add(R, -1)
  side:scale(0.5 * p["Width"])  -- (L - R) / 2 * width
  local newL, newR = mid:copy(), mid:copy()
  newL:add(side)
  newR:add(side, -1)
  local after = math.max((newL:peak()), (newR:peak()))
  if p["Keep level"] and after > before and after > 0 then
    newL:scale(before / after)
    newR:scale(before / after)
  end
  audio:setChannel(1, newL)
  audio:setChannel(2, newR)
end
