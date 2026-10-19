-- name: Mixer and transport (8 strips)
-- description: Eight faders, eight knobs and four rows of buttons drive a bank of eight mixer inserts (volume, pan, mute, solo, arm, select) plus the transport. Button LEDs follow the mixer state. Bank buttons move the strips by eight.
-- match: Gridline 8-strip
-- author: Gridline
--
-- Layout (MIDI channel 1):
--   faders   CC 20..27  -> volume of strips 1..8
--   knobs    CC 30..37  -> pan of strips 1..8
--   buttons  notes 0..7 mute, 8..15 solo, 16..23 arm, 24..31 select
--   transport notes 40 play/pause, 41 stop, 42 record, 43 song/pattern
--   bank     notes 46 (previous eight), 47 (next eight)
-- LEDs are note messages back to the device (velocity 127 = lit).

local bank = 0  -- first insert of the bank minus one

local function strip(i) return bank + i + 1 end  -- mixer index of strip i (0..7)

local function lights()
  for i = 0, 7 do
    local t = strip(i)
    local ok = t < mixer.count()
    device.led(i, ok and mixer.muted(t))
    device.led(8 + i, ok and mixer.solo(t))
    device.led(16 + i, ok and mixer.armed(t))
    device.led(24 + i, ok and mixer.selected() == t)
  end
end

local function transportLights()
  device.led(40, transport.isPlaying())
  device.led(42, transport.isRecording())
  device.led(43, transport.songMode())
end

function onInit()
  lights()
  transportLights()
end

function onMidiMsg(m)
  if m.channel ~= 1 then return false end
  if m.kind == "cc" then
    local c = m.cc
    if c >= 20 and c <= 27 and strip(c - 20) < mixer.count() then
      mixer.setVolume(strip(c - 20), m.value / 127)
      return true
    end
    if c >= 30 and c <= 37 and strip(c - 30) < mixer.count() then
      mixer.setPan(strip(c - 30), m.value / 127)
      return true
    end
    return false
  end
  if m.kind ~= "noteOn" then return m.kind == "noteOff" and m.note < 48 end
  local n = m.note
  if n < 32 then
    local row, t = n // 8, strip(n % 8)
    if t >= mixer.count() then return true end
    if row == 0 then mixer.setMuted(t, not mixer.muted(t))
    elseif row == 1 then mixer.setSolo(t, not mixer.solo(t))
    elseif row == 2 then mixer.setArmed(t, not mixer.armed(t))
    else mixer.select(t)
    end
    lights()
    ui.hint(mixer.name(t))
    return true
  end
  if n == 40 then transport.toggle()
  elseif n == 41 then transport.stop()
  elseif n == 42 then transport.record()
  elseif n == 43 then transport.setSongMode(not transport.songMode())
  elseif n == 46 then bank = math.max(0, bank - 8)
  elseif n == 47 then if bank + 8 < mixer.count() - 1 then bank = bank + 8 end
  else return false
  end
  if n >= 46 then
    lights()
    ui.hint("Strips " .. strip(0) .. "-" .. strip(7))
  end
  transportLights()
  return true
end

function onRefresh(what)
  if what.mixer or what.selection then lights() end
  if what.transport then transportLights() end
end
