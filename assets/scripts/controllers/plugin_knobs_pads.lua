-- name: Plugin knobs and channel pads
-- description: Eight endless encoders edit the focused plugin window's parameters a page of eight at a time; sixteen pads select channels and light up for the selected one. While recording, the record pad blinks.
-- match: Gridline pads
-- author: Gridline
--
-- Layout (MIDI channel 1 unless noted):
--   encoders CC 70..77, relative (1..63 = up, 65..127 = down)
--   page buttons CC 78 (previous page) / CC 79 (next page), value > 0 = press
--   pads notes 36..51 on channel 10 -> select channels 1..16 (LED = note back)
--   record pad note 52 on channel 10

local page = 0
local blink, ticks = false, 0

local function padLights()
  local sel = channels.selected() or 0
  for i = 1, 16 do device.led(35 + i, i == sel, 10) end
end

local function showParam(k)
  if plugin.paramCount() >= k then
    ui.hint(plugin.name() .. ": " .. plugin.paramName(k) .. " = " .. plugin.paramText(k))
  end
end

function onInit() padLights() end

function onMidiMsg(m)
  if m.kind == "cc" and m.channel == 1 then
    if m.cc >= 70 and m.cc <= 77 then
      local k = page * 8 + (m.cc - 70) + 1
      if plugin.name() == nil or k > plugin.paramCount() then return true end
      local delta = m.value < 64 and m.value or m.value - 128
      plugin.setParam(k, math.max(0, math.min(1, plugin.param(k) + delta / 128)))
      showParam(k)
      return true
    end
    if (m.cc == 78 or m.cc == 79) and m.value > 0 then
      local pages = math.max(1, (plugin.paramCount() + 7) // 8)
      page = (page + (m.cc == 79 and 1 or -1)) % pages
      ui.hint("Knob page " .. (page + 1) .. " of " .. pages)
      return true
    end
  end
  if m.channel == 10 and (m.kind == "noteOn" or m.kind == "noteOff") then
    if m.kind == "noteOn" then
      if m.note >= 36 and m.note <= 51 and m.note - 35 <= channels.count() then
        channels.select(m.note - 35)
        padLights()
        ui.hint(channels.name(m.note - 35))
      elseif m.note == 52 then
        transport.record()
      end
    end
    return true
  end
  return false
end

function onIdle()
  ticks = ticks + 1
  if ticks % 25 == 0 then  -- about twice a second
    if transport.isRecording() then
      blink = not blink
      device.led(52, blink, 10)
    elseif blink then
      blink = false
      device.led(52, false, 10)
    end
  end
end

function onRefresh(what)
  if what.selection or what.channels then padLights() end
  if what.plugin then page = 0 end
end
