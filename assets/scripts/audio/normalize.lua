-- name: Normalize to level
-- category: Amplitude
-- description: Scales the selection so its loudest sample (or its RMS level) reaches the target; both channels together or each on its own.
-- author: Gridline

function form(f)
  f:knob("Target", -1, -30, 0, "Target level in dB")
  f:combo("Measure", { "Peak", "RMS" }, 1)
  f:check("Channels separately", false, "Normalize left and right independently")
end

local function level(buf, rms)
  if rms then return buf:rms() end
  return (buf:peak())
end

function apply(audio, p)
  local target = dsp.dbToGain(p["Target"])
  local rms = p["Measure"] == 2
  if p["Channels separately"] then
    for _, buf in audio:buffers() do
      local l = level(buf, rms)
      if l > 0 then buf:scale(target / l) end
    end
    return
  end
  local l = 0
  for _, buf in audio:buffers() do l = math.max(l, level(buf, rms)) end
  if l == 0 then
    print("The selection is silent.")
    return
  end
  for _, buf in audio:buffers() do buf:scale(target / l) end
end
