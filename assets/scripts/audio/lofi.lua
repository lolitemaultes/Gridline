-- name: Lo-fi
-- category: Effects
-- description: Reduces the bit depth, darkens the top end and drives the result gently into saturation.
-- author: Gridline

function form(f)
  f:knobInt("Bits", 8, 2, 16, "Bit depth")
  f:knob("Tone", 6000, 500, 20000, "Low-pass cutoff in Hz")
  f:knob("Drive", 1.5, 0.1, 10, "Saturation amount")
  f:knob("Mix", 1, 0, 1, "Dry / wet")
end

function apply(audio, p)
  for _, buf in audio:buffers() do
    local wet = buf:copy()
    wet:quantize(p["Bits"])
    wet:biquad("lowpass", p["Tone"], 0.707, 0, audio.rate)
    wet:softclip(p["Drive"])
    buf:scale(1 - p["Mix"])
    buf:add(wet, p["Mix"])
  end
end
