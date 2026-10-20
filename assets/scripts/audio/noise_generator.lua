-- name: Noise generator
-- category: Generate
-- description: Writes white, pink or brown noise over the selection, or inserts it at the cursor. Left and right get different noise unless mono is ticked.
-- author: Gridline
-- target: cursor

function form(f)
  f:combo("Colour", { "White", "Pink", "Brown" }, 2)
  f:knob("Level", -18, -60, 0, "Level in dB")
  f:knob("Length", 2, 0.05, 30, "Seconds (used when inserting at the cursor)")
  f:check("Mono", false, "The same noise on both channels")
  f:check("Mix with the audio", false, "Adds the noise instead of replacing the selection")
end

local colours = { "white", "pink", "brown" }

function apply(audio, p)
  if not audio.selected then audio:resize(math.floor(p["Length"] * audio.rate + 0.5)) end
  local amp = dsp.dbToGain(p["Level"])
  for c, buf in audio:buffers() do
    local noise = buffer.new(audio.frames)
    noise:noise(colours[p["Colour"]], amp, p["Mono"] and audio.seed or audio.seed + c)
    if p["Mix with the audio"] then
      buf:add(noise)
    else
      audio:setChannel(c, noise)
    end
  end
end
