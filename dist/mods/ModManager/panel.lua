-- The right side of the Mods window: the selected mod's description in the picture frame, and its settings
-- below it as controls (sliders for numbers, check boxes for on/off, arrows for choices).
--
-- The controls are clips from the map window's own library (H7Label, H7Slider_BG_Gold, H7Slider_Gold_Thumb,
-- H7GUICommandButton, H7LeftArrow/H7RightArrow, H7TooltipBG as the number box) attached to the window. Flash doesn't
-- tell Unreal which clip was clicked, so mouse and keyboard input come from Engine.Console.InputKey: clicks are
-- hit-tested against the controls' rectangles in window coordinates, and a number box being edited takes the keys.

local P = {}

local Obj = h7.find_class("GFxUI.GFxObject")
local AS = h7.enum("ASType")
local PRESSED, RELEASED = 0, 1

-- Layout in window coordinates (the window is 1920x1080 inside the movie).
local FRAME = { x = 1366, y = 128, w = 452, h = 456 }           -- inside the picture frame
local AREA = { x = 1342, y = 640, w = 470, h = 300 }            -- the details panel below it
local ROW_H, ROWS = 48, 5
local LABEL_W = 210
local CTRL_X = AREA.x + LABEL_W + 20
local VALUE_W = 70
local TRACK_W = AREA.x + AREA.w - VALUE_W - 10 - CTRL_X

local state = { mod = nil, rows = {}, drag = nil }
P.state = state

local function window()
    local sel = h7.first("H7MapSelectCntl")
    return sel and sel.mMapSelection
end

-- Clips are looked up by name every time: the engine frees GFxObject wrappers nobody in UnrealScript holds.
local function get(name)
    local w = window()
    return w and w:GetObject(name, Obj)
end

local function ensure(name, linkage)
    local o = get(name)
    if not o then o = window():AttachMovie(linkage, name, -1, Obj) end
    return o
end

local function hide(name)
    local o = get(name)
    if o then o:SetBool("visible", false) end
end

local function place(o, x, y, w, h)
    o:SetFloat("x", x)
    o:SetFloat("y", y)
    if w then o:SetFloat("width", w) end
    if h then o:SetFloat("height", h) end
    o:SetBool("visible", true)
end

local function label(name, text, x, y, w, size, multiline)
    local o = ensure(name, "H7Label")
    if multiline then o:SetBool("multiline", true) end
    if size then o:SetFloat("size", size) end
    o:SetFloat("width", w)
    o:SetString("text", text)
    place(o, x, y)
    return o
end

local function num(v)
    if v == math.floor(v) and math.abs(v) < 1e15 then return ("%d"):format(v) end
    return (("%.3f"):format(v):gsub("0+$", ""):gsub("%.$", ""))
end

local function value_text(s, v)
    if v == nil then v = s.value end
    if s.type == "bool" then return v and "On" or "Off" end
    if s.type == "number" then return num(v) end
    return tostring(v)
end

-- ---------------------------------------------------------------------------------------------------------
-- Rows
-- ---------------------------------------------------------------------------------------------------------

local ROW_PARTS = { "lbl", "track", "thumb", "val", "check", "left", "right", "frame", "boxtext", "hint" }
local BOX_W = 150

local function hide_row(i)
    for _, part in ipairs(ROW_PARTS) do hide(("mModSet%d_%s"):format(i, part)) end
end

-- Position of the slider thumb for value v.
local function slider_fraction(s, v)
    if not (s.min and s.max) or s.max <= s.min then return 0 end
    return math.max(0, math.min(1, (v - s.min) / (s.max - s.min)))
end

local function draw_slider(i, s, v, cy)
    local track = ensure(("mModSet%d_track"):format(i), "H7Slider_BG_Gold")
    -- The gold bar is vertical: rotated -90 degrees it runs to the right of its origin, its thickness upwards.
    track:SetFloat("rotation", 0)
    track:SetFloat("height", TRACK_W)
    track:SetFloat("rotation", -90)
    place(track, CTRL_X, cy + 7)
    local thumb = ensure(("mModSet%d_thumb"):format(i), "H7Slider_Gold_Thumb")
    local tw, th = thumb:GetFloat("width"), thumb:GetFloat("height")
    place(thumb, CTRL_X + slider_fraction(s, v) * TRACK_W - tw / 2, cy - th / 2)
    label(("mModSet%d_val"):format(i), value_text(s, v), CTRL_X + TRACK_W + 10, cy - 12, VALUE_W, 20)
end

-- How a setting is edited: the manifest's `control` when it makes sense for the type, else a slider for numbers
-- with a range, a number box for other numbers.
local function kind_of(s)
    if s.type == "bool" then return "check" end
    if s.type == "choice" then return "arrows" end
    if s.type == "number" then
        if s.control == "input" or s.control == "arrows" then return s.control end
        return (s.min and s.max) and "slider" or "input"
    end
    return "text"
end

-- The number box: the game's tooltip background as the frame, a label with the value (or the text being typed
-- followed by a cursor) on top of it.
local function draw_box(i, s, text, editing, cy)
    local box = ensure(("mModSet%d_frame"):format(i), "H7TooltipBG")
    place(box, CTRL_X, cy - 17, BOX_W, 34)
    box:SetFloat("alpha", editing and 1 or 0.6)
    label(("mModSet%d_boxtext"):format(i), editing and (text .. "_") or text, CTRL_X + 10, cy - 12, BOX_W - 20, 20)
    local range = (s.min and s.max) and (num(s.min) .. " - " .. num(s.max)) or (s.min and (">= " .. num(s.min)))
        or (s.max and ("<= " .. num(s.max))) or ""
    label(("mModSet%d_hint"):format(i), editing and "Enter: save   Esc: cancel" or range, CTRL_X + BOX_W + 10,
        cy - 10, AREA.x + AREA.w - CTRL_X - BOX_W - 10, 16)
end

local function draw_row(i, s)
    local y = AREA.y + 44 + (i - 1) * ROW_H
    local cy = y + ROW_H / 2
    hide_row(i)
    label(("mModSet%d_lbl"):format(i), s.label, AREA.x, cy - 12, LABEL_W, 20)
    local r = { setting = s, cy = cy }
    local kind = kind_of(s)
    if kind == "input" then
        local editing = state.edit and state.edit.row == i
        draw_box(i, s, editing and state.edit.text or value_text(s), editing, cy)
        r.kind = "input"
        r.box = { x = CTRL_X, y = cy - 18, w = BOX_W, h = 36 }
    elseif kind == "slider" then
        draw_slider(i, s, s.value, cy)
        r.kind = "slider"
        r.box = { x = CTRL_X - 10, y = cy - 18, w = TRACK_W + 20, h = 36 }
    elseif kind == "check" then
        local c = ensure(("mModSet%d_check"):format(i), "H7GUICommandButton")
        place(c, CTRL_X, cy - 18, 36, 36)
        c:Invoke("DisplaySelectState", { { Type = AS.AS_Boolean, b = s.value } })
        c:SetFloat("alpha", s.value and 1 or 0.35) -- the button's own selected look is subtle
        label(("mModSet%d_val"):format(i), value_text(s), CTRL_X + 46, cy - 12, VALUE_W, 20)
        r.kind = "check"
        r.box = { x = CTRL_X, y = cy - 18, w = 46 + VALUE_W, h = 36 }
    elseif kind == "arrows" then
        local left = ensure(("mModSet%d_left"):format(i), "H7LeftArrow")
        local right = ensure(("mModSet%d_right"):format(i), "H7RightArrow")
        place(left, CTRL_X, cy - 14, 28, 28)
        place(right, AREA.x + AREA.w - 28, cy - 14, 28, 28)
        label(("mModSet%d_val"):format(i), value_text(s), CTRL_X + 36, cy - 12, AREA.x + AREA.w - 36 - CTRL_X - 36, 20)
        r.kind = "arrows"
        r.left = { x = CTRL_X, y = cy - 16, w = 32, h = 32 }
        r.right = { x = AREA.x + AREA.w - 32, y = cy - 16, w = 32, h = 32 }
    else
        label(("mModSet%d_val"):format(i), value_text(s) .. "  (edit in H7ModEngine.ini)", CTRL_X, cy - 12,
            AREA.x + AREA.w - CTRL_X, 18)
        r.kind = "text"
    end
    state.rows[i] = r
end

-- ---------------------------------------------------------------------------------------------------------
-- Showing a mod
-- ---------------------------------------------------------------------------------------------------------

function P.show(m)
    if not window() then return end
    state.mod, state.rows, state.drag, state.pending, state.edit = m and m.name, {}, nil, false, nil
    for i = 1, ROWS do hide_row(i) end
    if not m then
        for _, n in ipairs({ "mModDesc", "mModSetTitle", "mModSetMore" }) do hide(n) end
        return
    end
    local about = m.description ~= "" and m.description or "No description."
    local by = (m.author ~= "" and ("by " .. m.author) or "") .. (m.version ~= "" and ("   version " .. m.version) or "")
    label("mModDesc", m.name .. "\n" .. by .. "\n\n" .. about, FRAME.x, FRAME.y, FRAME.w, 22, true)

    local settings = h7.mod_settings(m.name)
    label("mModSetTitle", #settings > 0 and "Settings" or "This mod has no settings.", AREA.x, AREA.y, AREA.w, 24)
    for i, s in ipairs(settings) do
        if i > ROWS then break end
        draw_row(i, s)
    end
    if #settings > ROWS then
        label("mModSetMore", ("+%d more in H7ModEngine.ini, section [Mod.%s]"):format(#settings - ROWS, m.name),
            AREA.x, AREA.y + 44 + ROWS * ROW_H, AREA.w, 16)
    else
        hide("mModSetMore")
    end
end

function P.hide_all()
    for i = 1, ROWS do hide_row(i) end
    for _, n in ipairs({ "mModDesc", "mModSetTitle", "mModSetMore" }) do hide(n) end
    state.mod, state.rows, state.drag, state.pending, state.edit = nil, {}, nil, false, nil
end

-- ---------------------------------------------------------------------------------------------------------
-- Input
-- ---------------------------------------------------------------------------------------------------------

local function inside(b, x, y) return b and x >= b.x and y >= b.y and x < b.x + b.w and y < b.y + b.h end

local function mouse()
    local w = window()
    if not w then return nil end
    return w:GetFloat("mouseX"), w:GetFloat("mouseY")
end

local function slider_value(s, mx)
    local t = math.max(0, math.min(1, (mx - CTRL_X) / TRACK_W))
    local v = s.min + t * (s.max - s.min)
    if s.step then v = s.min + math.floor((v - s.min) / s.step + 0.5) * s.step end
    return math.max(s.min, math.min(s.max, v))
end

local function set(i, s, v)
    local stored = h7.set_mod_setting(state.mod, s.key, v)
    s.value = stored
    draw_row(i, s)
end

-- Number box editing. The text is checked when it is saved: h7.set_mod_setting clamps it to the range and rounds
-- it to the step; text that isn't a number is dropped.
local function stop_edit(save)
    local e = state.edit
    if not e then return end
    state.edit = nil
    local r = state.rows[e.row]
    if not r then return end
    local v = tonumber(e.text)
    if save and v then
        set(e.row, r.setting, v)
    else
        draw_row(e.row, r.setting)
    end
end

local function start_edit(i)
    stop_edit(true)
    local r = state.rows[i]
    state.edit = { row = i, text = value_text(r.setting), fresh = true }
    draw_row(i, r.setting)
end

-- Key names as the engine reports them, lower-cased (names compare case-insensitively in Unreal, and the engine
-- keeps the spelling a name was first created with: "seven", "Enter").
local KEY_CHARS = {
    zero = "0", one = "1", two = "2", three = "3", four = "4", five = "5", six = "6", seven = "7", eight = "8",
    nine = "9", numpadzero = "0", numpadone = "1", numpadtwo = "2", numpadthree = "3", numpadfour = "4",
    numpadfive = "5", numpadsix = "6", numpadseven = "7", numpadeight = "8", numpadnine = "9",
    period = ".", decimal = ".", comma = ".", subtract = "-", underscore = "-", minus = "-",
}

-- A key while a number box is being edited; returns true when the key was used (the game shouldn't see it).
function P.key(key, event)
    local e = state.edit
    if not e then return false end
    if event ~= PRESSED and event ~= 2 then return true end -- releases and repeats of other keys: swallowed
    key = tostring(key):lower()
    if key == "enter" then stop_edit(true) return true end
    if key == "escape" then stop_edit(false) return true end
    if key == "backspace" or key == "delete" then
        e.text = e.fresh and "" or e.text:sub(1, -2)
        e.fresh = false
    else
        local c = KEY_CHARS[key]
        if not c or event ~= PRESSED then return c ~= nil end
        if e.fresh then e.text = "" e.fresh = false end
        if c == "-" and e.text ~= "" then return true end
        if c == "." and e.text:find(".", 1, true) then return true end
        if #e.text < 12 then e.text = e.text .. c end
    end
    local r = state.rows[e.row]
    if r then draw_row(e.row, r.setting) end
    return true
end

function P.editing() return state.edit ~= nil end

local function cycle(s, dir)
    if s.type == "choice" then
        for k, o in ipairs(s.options) do
            if o == s.value then return s.options[(k - 1 + dir) % #s.options + 1] end
        end
        return s.options[1]
    end
    return s.value + dir * (s.step or 1) -- numbers without a range
end

-- The button event arrives before Flash has seen the mouse move that came with it, so a press is handled on the
-- next frame (P.tick), when the window's mouse position is current.
function P.press()
    if state.mod then state.pending = true end
end

local function handle_press()
    if not state.mod then return false end
    local mx, my = mouse()
    if not mx then return false end
    for i, r in pairs(state.rows) do
        if r.kind == "input" and inside(r.box, mx, my) then
            if not (state.edit and state.edit.row == i) then start_edit(i) end
            return true
        end
    end
    stop_edit(true) -- a click anywhere else saves the number being typed
    for i, r in pairs(state.rows) do
        local s = r.setting
        if r.kind == "slider" and inside(r.box, mx, my) then
            state.drag = { row = i, value = slider_value(s, mx) }
            draw_slider(i, s, state.drag.value, r.cy)
            return true
        elseif r.kind == "check" and inside(r.box, mx, my) then
            set(i, s, not s.value)
            return true
        elseif r.kind == "arrows" and (inside(r.left, mx, my) or inside(r.right, mx, my)) then
            set(i, s, cycle(s, inside(r.left, mx, my) and -1 or 1))
            return true
        end
    end
    return false
end

function P.release()
    if state.pending then
        state.pending = false
        handle_press()
    end
    local d = state.drag
    if not d then return end
    state.drag = nil
    local r = state.rows[d.row]
    if r then set(d.row, r.setting, d.value) end
end

-- While a slider is held, its thumb and value follow the mouse; the setting is saved on release.
function P.tick()
    if state.pending then
        state.pending = false
        handle_press()
    end
    local d = state.drag
    if not d then return end
    local r = state.rows[d.row]
    local mx = mouse()
    if not (r and mx) then state.drag = nil return end
    local v = slider_value(r.setting, mx)
    if v ~= d.value then
        d.value = v
        draw_slider(d.row, r.setting, v, r.cy)
    end
end

P.PRESSED, P.RELEASED = PRESSED, RELEASED
return P
