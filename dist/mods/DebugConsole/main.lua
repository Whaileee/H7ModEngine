-- DebugConsole: an in-game Lua console drawn over the game.
--
-- Open/close with the key in H7ModEngine.ini, [Console] Key= (Unreal key name, default Tilde; re-read every
-- second, so it can be changed while the game runs). Enter runs the input as Lua; "/command" runs a game console
-- command. Input can span several lines (scripts): Enter on an unfinished chunk adds a line, Shift+Enter always
-- adds one, Ctrl+Enter always runs; Ctrl+V pastes. A popup suggests commands, globals, fields and object members
-- (suggest.lua). The output area shows the engine log (h7.log_lines), so print, h7.log and errors from every mod
-- appear.
--
-- Drawing: a post-hook on GameViewportClient.PostRender draws with the frame's Canvas.
-- Input: Engine.Console.InputKey (keys) and InputChar (typed text, so keyboard layouts and Polish letters work);
-- while the console is open both report keys as handled so the game doesn't act on them.

local suggest = mod.require("suggest")

local VIEWPORT = "Engine.GameViewportClient."
local CONSOLE = "Engine.Console."
local PRESSED, RELEASED, REPEAT = 0, 1, 2
local MAX_LINES, MAX_HISTORY = 1000, 100
local MAX_INPUT_ROWS, POPUP_ROWS = 12, 8
local INDENT = "    "
local OWN_PREFIX = "[" .. mod.name .. "] "

local con = {
    open = false,
    input = {},        -- characters (UTF-8 strings, "\n" between lines) of the text being typed
    cursor = 0,        -- characters before the cursor
    rev = 0,           -- bumped on every edit or cursor move (suggestions are recomputed when it changes)
    lines = {},        -- output: log lines
    last = 0,          -- newest log line number already in `lines`
    scroll = 0,        -- lines scrolled up from the bottom
    hist_pos = nil,    -- index into history while browsing with Up/Down
    skip_char = false, -- the toggle key's character must not be typed
    ctrl = false,
    shift = false,
    used = false,      -- the current key was taken by the console (post-hook reports it as handled)
    blink = 0,
    sugg = nil,        -- current suggestions (suggest.compute result) with .sel
    sugg_rev = -1,
    dismissed = -1,    -- rev at which Esc hid the popup
}

local saved = mod.load()
local history = saved.history or {}
-- Commands the game doesn't recognize (lower-case): the list known for v2.2.1, plus what this install's probe
-- and failed commands found. They are left out of the suggestions.
local rejected = saved.rejected or {}
for _, cmd in ipairs(mod.require("known")) do rejected[suggest.key_of(cmd)] = true end
suggest.set_rejected(rejected)

-- Console code runs in its own environment: variables persist between lines, everything else comes from _G.
local env = setmetatable({}, { __index = _G })

-- ---------------------------------------------------------------------------------------------------------
-- Toggle key
-- ---------------------------------------------------------------------------------------------------------

local toggle_key = "tilde"
local function read_key()
    toggle_key = tostring(h7.config("Console", "Key", "Tilde")):lower()
end
read_key()
h7.every(1, read_key)

-- ---------------------------------------------------------------------------------------------------------
-- Output
-- ---------------------------------------------------------------------------------------------------------

local function pull_log()
    local new, newest = h7.log_lines(con.last)
    con.last = newest
    for _, line in ipairs(new) do
        -- The console's own output is logged with the mod prefix; show it without.
        if line:sub(1, #OWN_PREFIX) == OWN_PREFIX then line = line:sub(#OWN_PREFIX + 1) end
        line = line:gsub("\t", INDENT)
        for part in (line .. "\n"):gmatch("(.-)\r?\n") do -- tracebacks and scripts span several lines
            con.lines[#con.lines + 1] = part
            if con.scroll > 0 then con.scroll = con.scroll + 1 end -- keep the view still while scrolled up
        end
    end
    local extra = #con.lines - MAX_LINES
    if extra > 0 then
        table.move(con.lines, extra + 1, #con.lines, 1)
        for i = #con.lines, #con.lines - extra + 1, -1 do con.lines[i] = nil end
    end
end
pull_log()

local function out(...) h7.log(...) end

-- Readable text for a value: tables are expanded two levels deep, engine objects use their full name.
local function dump(v, depth, seen)
    depth, seen = depth or 2, seen or {}
    if type(v) == "string" then return string.format("%q", v) end
    if type(v) ~= "table" then return tostring(v) end
    if seen[v] then return "<cycle>" end
    if depth <= 0 then return tostring(v) end
    seen[v] = true
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b)
        if type(a) == type(b) and (type(a) == "number" or type(a) == "string") then return a < b end
        return type(a) < type(b)
    end)
    local parts = {}
    for i, k in ipairs(keys) do
        if i > 40 then parts[#parts + 1] = "... (" .. #keys .. " entries)" break end
        local ks = (type(k) == "string" and k:match("^[%a_][%w_]*$")) and k or "[" .. dump(k, 0) .. "]"
        parts[#parts + 1] = ks .. " = " .. dump(v[k], depth - 1, seen)
    end
    seen[v] = nil
    return "{ " .. table.concat(parts, ", ") .. " }"
end

local function clear()
    con.lines, con.scroll = {}, 0
end

local function help()
    out("Lua: type an expression or statement and press Enter; results are printed.")
    out("  /command          a game console command; the popup lists the commands this screen accepts")
    out("  Scripts: Enter on an unfinished chunk (for ... do) adds a line; Shift+Enter adds one, Ctrl+Enter runs")
    out("  Popup: Up/Down choose, Tab accepts, Esc hides. Tab with no popup indents.")
    out("  Up/Down (on the first/last line) history; PgUp/PgDn or the mouse wheel scroll")
    out("  Ctrl+V paste, Ctrl+C copy the input, Ctrl+U clear the input, Ctrl+L clear the output, Esc close")
    out("  dump(v [, depth]) prints a value expanded; commands([filter]) lists the game commands; clear()")
    out("  Variables you set stay for the session. Hooks/timers made here belong to DebugConsole and are")
    out("  removed when the mod is turned off or reloaded.")
end

env.dump = function(v, depth) out(dump(v, depth or 3)) end
env.clear = clear
env.help = help
env.commands = function(filter)
    local n = 0
    for _, c in ipairs(suggest.commands()) do
        if not filter or c.text:lower():find(filter:lower(), 1, true) then
            out(c.text .. "  " .. c.desc)
            n = n + 1
        end
    end
    out(n .. " commands")
end

-- Each Stat/Show entry of the game's list is tested once per install (see suggest.probe); needs a player controller.
local function probe()
    saved.probed = saved.probed or {}
    local ok, n = pcall(suggest.probe, rejected, saved.probed)
    if not ok then h7.warn("command probe failed: " .. tostring(n)) return end
    if n > 0 then
        saved.rejected = rejected
        pcall(mod.save, saved)
        suggest.invalidate()
    end
end
if h7.first("PlayerController") then probe() end

h7.on("map_loaded", function()
    suggest.invalidate()
    suggest.clear_cache()
    probe()
end)

-- ---------------------------------------------------------------------------------------------------------
-- Running the input
-- ---------------------------------------------------------------------------------------------------------

-- Error handler: the traceback without the console's own frames (everything from xpcall down).
local function traceback(err)
    local tb = debug.traceback(tostring(err), 2)
    local cut = tb:find("\n%s*%[C%]: in function 'xpcall'")
    return cut and tb:sub(1, cut - 1) or tb
end

local function compile(src)
    -- An expression prints its value; otherwise run it as statements. In a script whose last line is an
    -- expression ("x = 5" then "x * 2"), that line's value is printed.
    local fn = load("return " .. src, "=console", "t", env)
    if fn then return fn end
    local fn2, err = load(src, "=console", "t", env)
    if fn2 then return fn2 end
    local head, last = src:match("^(.*\n)([^\n]*%S[^\n]*)$")
    if head then
        local fn3 = load(head .. "return " .. last, "=console", "t", env)
        if fn3 then return fn3 end
    end
    return nil, err
end

-- True when src is a chunk that isn't finished yet ("for i = 1, 3 do" without its end).
local function incomplete(src)
    local fn, err = compile(src)
    return not fn and tostring(err):find("<eof>$") ~= nil
end

local function run(src)
    out("> " .. (src:gsub("\n", "\n>> ")))
    if src:sub(1, 1) == "/" then
        local cmd = src:sub(2):match("^%s*(.-)%s*$")
        local ok, res = pcall(h7.exec, cmd)
        if not ok then h7.error(res) return end
        res = tostring(res or ""):gsub("%s+$", "")
        if res ~= "" then out(res) end
        if res:find("^Command not recognized") then
            -- Not in this build: stop suggesting it (remembered across sessions).
            rejected[suggest.key_of(cmd)] = true
            saved.rejected = rejected
            pcall(mod.save, saved)
        end
        return
    end
    local fn, err = compile(src)
    if not fn then h7.error(err) return end
    local res = table.pack(xpcall(fn, traceback))
    if not res[1] then h7.error(res[2]) return end
    if res.n > 1 then
        local parts = {}
        for i = 2, res.n do parts[#parts + 1] = dump(res[i]) end
        out(table.concat(parts, ", "))
    end
end

-- ---------------------------------------------------------------------------------------------------------
-- Editing
-- ---------------------------------------------------------------------------------------------------------

local function text() return table.concat(con.input) end
local function changed() con.rev = con.rev + 1 con.blink = 0 end
local function edited() con.hist_pos = nil changed() end

local function set_text(s)
    con.input = {}
    for _, c in utf8.codes(s) do con.input[#con.input + 1] = utf8.char(c) end
    con.cursor = #con.input
    changed()
end

local function insert(s)
    s = s:gsub("\r\n", "\n"):gsub("\r", "\n"):gsub("\t", INDENT)
    for _, c in utf8.codes(s) do
        if c == 10 or c >= 32 then
            con.cursor = con.cursor + 1
            table.insert(con.input, con.cursor, utf8.char(c))
        end
    end
    edited()
end

-- Index of the first character of the line containing cursor position `pos` (pos = characters before it).
local function line_start(pos)
    local i = pos
    while i > 0 and con.input[i] ~= "\n" do i = i - 1 end
    return i + 1
end
-- Index of the last character of that line (before its "\n").
local function line_end(pos)
    local i = pos + 1
    while i <= #con.input and con.input[i] ~= "\n" do i = i + 1 end
    return i - 1
end
local function current_line_before()
    return table.concat(con.input, "", line_start(con.cursor), con.cursor)
end

-- A line that closes a block (end, else, until, }) loses one level of indentation when the player leaves it.
local function dedent_line()
    local start = line_start(con.cursor)
    local line = table.concat(con.input, "", start, line_end(con.cursor))
    if not line:match("^" .. INDENT) then return end
    local code = line:match("^%s*(.-)%s*$")
    if not (code:match("^end%f[^%w_]") or code:match("^else") or code:match("^until%f[^%w_]") or code:match("^[})]")) then
        return
    end
    for _ = 1, #INDENT do table.remove(con.input, start) end
    con.cursor = con.cursor - #INDENT
    edited()
end

local function new_line()
    dedent_line()
    -- Keep the current line's indentation, one more after a line that opens a block.
    local line = table.concat(con.input, "", line_start(con.cursor), con.cursor)
    local indent = line:match("^(%s*)")
    local code = line:gsub("%-%-.*$", ""):gsub("%s+$", "")
    if code:match("%f[%w]do$") or code:match("%f[%w]then$") or code:match("%f[%w]else$")
        or code:match("%f[%w]repeat$") or code:match("%f[%w]function%s*[%w_%.:]*%b()$") or code:match("[{(]$") then
        indent = indent .. INDENT
    end
    insert("\n" .. indent)
end

-- Up/Down inside multi-line input; false at the first/last line.
local function move_line(dir)
    local start = line_start(con.cursor)
    local col = con.cursor - (start - 1)
    local target
    if dir < 0 then
        if start <= 1 then return false end
        target = line_start(start - 2)
    else
        local e = line_end(con.cursor)
        if e >= #con.input then return false end
        target = e + 2
    end
    con.cursor = math.min(target - 1 + col, line_end(target - 1))
    changed()
    return true
end

local function submit()
    local s = text()
    set_text("")
    con.hist_pos, con.scroll = nil, 0
    if not s:match("%S") then return end
    if history[#history] ~= s then
        history[#history + 1] = s
        if #history > MAX_HISTORY then table.remove(history, 1) end
        saved.history = history
        pcall(mod.save, saved)
    end
    run(s)
end

local function browse(dir)
    if #history == 0 then return end
    local pos = (con.hist_pos or (#history + 1)) + dir
    if pos < 1 then pos = 1 end
    if pos > #history then
        set_text("")
        con.hist_pos = nil
        return
    end
    set_text(history[pos])
    con.hist_pos = pos
end

local function page() return math.max(1, (con.visible_lines or 10) - 2) end
local function scroll(n)
    con.scroll = math.max(0, math.min(con.scroll + n, #con.lines - 1))
end

-- ---------------------------------------------------------------------------------------------------------
-- Suggestions
-- ---------------------------------------------------------------------------------------------------------

local function popup()
    local s = con.sugg
    if not s or con.dismissed == con.rev or con.hist_pos or not (s.items or s.hint) then return nil end
    return s
end

local function update_suggestions()
    if con.sugg_rev == con.rev then return end
    con.sugg_rev = con.rev
    local ok, s = pcall(suggest.compute, current_line_before(), text(), env,
        table.concat(con.input, "", 1, con.cursor))
    if not ok then
        h7.error("suggestions: " .. tostring(s))
        s = nil
    end
    if s then s.sel = 1 end
    con.sugg = s
end

local function accept()
    local s = popup()
    local item = s and s.items and s.items[s.sel]
    if not item then return false end
    for _ = 1, s.replace do
        table.remove(con.input, con.cursor)
        con.cursor = con.cursor - 1
    end
    insert(item.text .. ((s.command and item.args) and " " or ""))
    return true
end

local function move_sel(dir)
    local s = popup()
    if not (s and s.items) then return false end
    s.sel = (s.sel - 1 + dir) % #s.items + 1
    return true
end

-- ---------------------------------------------------------------------------------------------------------
-- Keys
-- ---------------------------------------------------------------------------------------------------------

local KEYS = {
    enter = function()
        if con.shift then return new_line() end
        dedent_line()
        local s = text()
        if not con.ctrl and s:sub(1, 1) ~= "/" and s:match("%S") and incomplete(s) then return new_line() end
        submit()
    end,
    escape = function()
        if popup() then con.dismissed = con.rev else con.open = false end
    end,
    backspace = function()
        if con.cursor > 0 then table.remove(con.input, con.cursor) con.cursor = con.cursor - 1 edited() end
    end,
    delete = function()
        if con.cursor < #con.input then table.remove(con.input, con.cursor + 1) edited() end
    end,
    left = function() con.cursor = math.max(0, con.cursor - 1) changed() end,
    right = function() con.cursor = math.min(#con.input, con.cursor + 1) changed() end,
    home = function() con.cursor = line_start(con.cursor) - 1 changed() end,
    ["end"] = function() con.cursor = line_end(con.cursor) changed() end,
    up = function()
        if move_sel(-1) then return end
        if con.hist_pos or not move_line(-1) then browse(-1) end
    end,
    down = function()
        if move_sel(1) then return end
        if con.hist_pos or not move_line(1) then browse(1) end
    end,
    pageup = function() scroll(page()) end,
    pagedown = function() scroll(-page()) end,
    mousescrollup = function() scroll(3) end,
    mousescrolldown = function() scroll(-3) end,
    tab = function()
        if accept() then return end
        if not current_line_before():match("%S") then insert(INDENT) end
    end,
}
local CTRL_KEYS = {
    l = clear,
    u = function() set_text("") con.hist_pos = nil end,
    v = function() if h7.clipboard then insert(h7.clipboard()) else out("Ctrl+V needs a newer dinput8.dll") end end,
    c = function() if h7.clipboard then h7.clipboard(text()) end end,
    home = function() con.cursor = 0 changed() end,
    ["end"] = function() con.cursor = #con.input changed() end,
}

-- A key event while the console is open (or the toggle key); returns true when the console used it.
local function key(name, event)
    name = tostring(name):lower()
    if name == "leftcontrol" or name == "rightcontrol" then
        con.ctrl = event ~= RELEASED
        return con.open
    end
    if name == "leftshift" or name == "rightshift" then
        con.shift = event ~= RELEASED
        return con.open
    end
    if event == PRESSED then con.skip_char = false end
    if name == toggle_key then
        if event == PRESSED then
            con.open = not con.open
            con.skip_char = true -- the character this key types arrives next, through InputChar
            if con.open then pull_log() end
        end
        return true
    end
    if not con.open then return false end
    if name:find("mouse") and not name:find("scroll") then return false end -- clicks still reach the game
    if event == PRESSED or event == REPEAT then
        update_suggestions() -- the popup the player sees decides what Up/Down/Tab do
        local fn = (con.ctrl and CTRL_KEYS[name]) or KEYS[name]
        if fn then fn() end
    end
    return true
end

local function char(s)
    if con.skip_char then con.skip_char = false return true end
    if not con.open then return false end
    if con.ctrl then return true end
    s = tostring(s or "")
    local b = s:byte(1)
    if b and b >= 32 and b ~= 127 then insert(s) end
    return true
end

h7.hook(CONSOLE .. "InputKey", function(_, args)
    local ok, used = pcall(key, args.Key, args.Event)
    if not ok then h7.error(used) end
    con.used = ok and used
end)
h7.hook_post(CONSOLE .. "InputKey", function()
    if con.used then con.used = false return true end
end)
h7.hook(CONSOLE .. "InputChar", function(_, args)
    local ok, used = pcall(char, args.Unicode)
    if not ok then h7.error(used) end
    con.char_used = ok and used
end)
h7.hook_post(CONSOLE .. "InputChar", function()
    if con.char_used then con.char_used = false return true end
end)

-- ---------------------------------------------------------------------------------------------------------
-- Drawing
-- ---------------------------------------------------------------------------------------------------------

local COLORS = {
    input = { 120, 200, 255 },
    error = { 255, 110, 100 },
    warning = { 255, 210, 90 },
    text = { 225, 225, 225 },
    title = { 255, 220, 140 },
    dim = { 150, 155, 165 },
    selected = { 255, 255, 255 },
}

local function line_color(line)
    if line:sub(1, 1) == ">" then return COLORS.input end
    if line:find("ERROR", 1, true) then return COLORS.error end
    if line:find("^WARNING") or line:find("warning:", 1, true) then return COLORS.warning end
    return COLORS.text
end

local font
local function get_font()
    if font == nil then
        font = h7.find("Font EngineFonts.SmallFont") or false
    end
    return font or nil
end

-- Long lines are cut into pieces of `cols` characters (the font is not monospaced, so `cols` is measured on
-- typical text and errs on the short side).
local function wrap(line, cols, into)
    local n = utf8.len(line) or #line
    if n <= cols then into[#into + 1] = line return end
    local i = 1
    while i <= n do
        local a = utf8.offset(line, i) or #line + 1
        local b = (utf8.offset(line, i + cols) or #line + 1) - 1
        into[#into + 1] = line:sub(a, b)
        i = i + cols
    end
end

local function clip(s, cols)
    if (utf8.len(s) or #s) <= cols then return s end
    return s:sub(1, (utf8.offset(s, cols) or #s + 1) - 1) .. "..."
end

local function set_color(c, col, a) c:SetDrawColor(col[1], col[2], col[3], a or 255) end

local KIND_COLS = 9

-- The signature hint: label(param, PARAM, param) -> ret, the argument being typed highlighted and underlined.
local function draw_hint(c, hint, x, y, max_x, lh, scale, tex)
    local function seg(str, col)
        if x >= max_x then return end
        set_color(c, col)
        c:SetPos(x, y, 0)
        c:DrawText(str, false, scale, scale)
        local w = c:TextSize(str, 0, 0, scale, scale) or 0
        return w
    end
    local function put(str, col)
        local w = seg(str, col)
        if w then x = x + w end
        return w
    end
    put(hint.label, COLORS.input)
    if not hint.params then return end
    put("(", COLORS.text)
    for i, p in ipairs(hint.params) do
        if i > 1 then put(", ", COLORS.text) end
        local active = i == hint.active
        local x0 = x
        local w = put(p, active and COLORS.title or COLORS.dim)
        if active and w then
            c:SetPos(x0, y + lh - 3, 0)
            set_color(c, COLORS.title)
            c:DrawRect(w, 2, tex)
        end
    end
    put(")", COLORS.text)
    if hint.ret and hint.ret ~= "" then put(" -> " .. hint.ret, COLORS.text) end
end

local function draw_popup(c, x, y, W, lh, cw, scale, tex)
    local s = popup()
    if not s then return end
    local rows, first = {}, 1
    if s.items then
        first = math.max(1, math.min(s.sel - math.floor(POPUP_ROWS / 2), #s.items - POPUP_ROWS + 1))
        for i = first, math.min(#s.items, first + POPUP_ROWS - 1) do
            pcall(suggest.detail, s.items[i])
            rows[#rows + 1] = s.items[i]
        end
    end
    local hint = s.hint
    local hint_rows = hint and ((hint.doc and hint.doc ~= "") and 2 or 1) or 0
    local name_cols = 0
    for _, r in ipairs(rows) do name_cols = math.max(name_cols, utf8.len(r.text) or #r.text) end
    name_cols = math.min(name_cols, 36)
    local width = math.min(W - x - 8, math.max(W * 0.5, (name_cols + KIND_COLS + 80) * cw))
    local more = s.items and #s.items > POPUP_ROWS
    local h = (hint_rows + #rows + (more and 1 or 0)) * lh + 8
    c:SetPos(x, y, 0)
    c:SetDrawColor(20, 24, 34, 240)
    c:DrawRect(width, h, tex)
    c:SetPos(x, y, 0)
    c:SetDrawColor(255, 220, 140, 160)
    c:DrawRect(width, 1, tex)
    local ry = y + 4
    local max_cols = math.floor((width - 12) / math.max(cw, 1))
    if hint then
        draw_hint(c, hint, x + 6, ry, x + width - 6, lh, scale, tex)
        ry = ry + lh
        if hint_rows == 2 then
            set_color(c, COLORS.dim)
            c:SetPos(x + 6, ry, 0)
            c:DrawText(clip(hint.doc, max_cols), false, scale, scale)
            ry = ry + lh
        end
        if #rows > 0 then
            c:SetPos(x + 2, ry - 1, 0)
            c:SetDrawColor(255, 220, 140, 60)
            c:DrawRect(width - 4, 1, tex)
        end
    end
    local kind_x = x + 6 + (name_cols + 2) * cw
    local detail_x = kind_x + KIND_COLS * cw
    local detail_cols = math.max(10, math.floor((x + width - detail_x) / math.max(cw, 1)) - 1)
    for i, r in ipairs(rows) do
        local sel = (first + i - 1) == s.sel
        if sel then
            c:SetPos(x + 2, ry, 0)
            c:SetDrawColor(60, 90, 140, 255)
            c:DrawRect(width - 4, lh, tex)
        end
        set_color(c, sel and COLORS.selected or COLORS.input)
        c:SetPos(x + 6, ry, 0)
        c:DrawText(clip(r.text, name_cols), false, scale, scale)
        if r.kind then
            set_color(c, COLORS.dim)
            c:SetPos(kind_x, ry, 0)
            c:DrawText(r.kind, false, scale, scale)
        end
        -- Signature or type, then the declaring class (dimmed) when there is room.
        local detail = clip(r.detail or "", detail_cols)
        local dw = 0
        if detail ~= "" then
            set_color(c, sel and COLORS.selected or COLORS.text)
            c:SetPos(detail_x, ry, 0)
            c:DrawText(detail, false, scale, scale)
            dw = c:TextSize(detail, 0, 0, scale, scale) or 0
        end
        if r.owner and r.owner ~= "" then
            local ox = detail_x + dw + (dw > 0 and 3 * cw or 0)
            if ox + (#r.owner) * cw < x + width - 6 then
                set_color(c, COLORS.dim)
                c:SetPos(ox, ry, 0)
                c:DrawText(r.owner, false, scale, scale)
            end
        end
        ry = ry + lh
    end
    if more then
        set_color(c, COLORS.dim)
        c:SetPos(x + 6, ry, 0)
        c:DrawText(("%d/%d   Up/Down choose, Tab accepts, Esc hides"):format(s.sel, #s.items), false, scale, scale)
    end
end

local function draw(c)
    local W, H = c.ClipX, c.ClipY
    if not W or W <= 0 then return end
    local scale = mod.settings.scale or 1.5
    local f = get_font()
    if f then c.Font = f end
    local cw, lh = c:TextSize("abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123456789", 0, 0, scale, scale)
    cw = (cw or 0) / 46
    lh = math.max(lh or 0, 8 * scale) + 2
    local pad = 8
    local height = math.floor(H * (mod.settings.height or 0.45))
    local tex = c.DefaultTexture

    -- Input lines (scripts can have several; the view follows the cursor).
    local input_lines = { "" }
    local cur_line = 1
    for i, ch in ipairs(con.input) do
        if ch == "\n" then input_lines[#input_lines + 1] = "" else input_lines[#input_lines] = input_lines[#input_lines] .. ch end
        if i == con.cursor then cur_line = #input_lines end
    end
    local first_row = math.max(1, cur_line - MAX_INPUT_ROWS + 1)
    local nrows = math.min(#input_lines - first_row + 1, MAX_INPUT_ROWS)
    local bar_h = nrows * lh + pad
    local function prompt(row) return row == 1 and "> " or ".. " end

    -- Background and the input bar.
    c:SetPos(0, 0, 0)
    c:SetDrawColor(10, 12, 18, 215)
    c:DrawRect(W, height, tex)
    c:SetPos(0, height - bar_h, 0)
    c:SetDrawColor(30, 36, 50, 235)
    c:DrawRect(W, bar_h, tex)
    c:SetPos(0, height, 0)
    c:SetDrawColor(255, 220, 140, 200)
    c:DrawRect(W, 2, tex)

    -- Title.
    set_color(c, COLORS.title)
    c:SetPos(pad, pad / 2, 0)
    c:DrawText("H7ModEngine " .. tostring(h7.version) .. " console   (help() for keys, Esc closes)", false, scale, scale)

    -- Output, newest at the bottom.
    local cols = math.max(20, math.floor((W - 2 * pad) / math.max(cw, 1)) - 2)
    local top, bottom = lh + pad, height - bar_h - pad / 2
    local rows = math.max(1, math.floor((bottom - top) / lh))
    con.visible_lines = rows
    local shown = {}
    local i = #con.lines - con.scroll
    while i >= 1 and #shown < rows do
        local pieces = {}
        wrap(con.lines[i], cols, pieces)
        for k = #pieces, 1, -1 do
            if #shown >= rows then break end
            shown[#shown + 1] = { pieces[k], con.lines[i] }
        end
        i = i - 1
    end
    local y = bottom - lh
    for _, s in ipairs(shown) do
        set_color(c, line_color(s[2]))
        c:SetPos(pad, y, 0)
        c:DrawText(s[1], false, scale, scale)
        y = y - lh
    end
    if con.scroll > 0 then
        set_color(c, COLORS.title)
        c:SetPos(W - pad - cw * 24, bottom - lh, 0)
        c:DrawText(("-- %d more below --"):format(con.scroll), false, scale, scale)
    end

    -- Input and cursor.
    set_color(c, COLORS.input)
    local iy0 = height - bar_h + pad / 2
    for r = 0, nrows - 1 do
        c:SetPos(pad, iy0 + r * lh, 0)
        c:DrawText(prompt(first_row + r) .. input_lines[first_row + r], false, scale, scale)
    end
    local before = current_line_before()
    con.blink = con.blink + 1
    if con.blink % 60 < 36 then
        local bw = c:TextSize(prompt(cur_line) .. before, 0, 0, scale, scale) or 0
        c:SetPos(pad + bw, iy0 + (cur_line - first_row) * lh, 0)
        c:SetDrawColor(255, 255, 255, 255)
        c:DrawRect(math.max(2, scale * 1.5), lh - 2, tex)
    end

    -- Suggestions below the console, starting under the word being completed.
    update_suggestions()
    local s = popup()
    if s then
        local stem = before
        local n = utf8.len(stem) or #stem
        if s.replace and s.replace > 0 then stem = stem:sub(1, (utf8.offset(stem, n - s.replace + 1) or #stem + 1) - 1) end
        local wx = pad + (c:TextSize(prompt(cur_line) .. stem, 0, 0, scale, scale) or 0)
        draw_popup(c, math.min(wx, W * 0.5), height + 4, W, lh, cw, scale, tex)
    end
end

local draw_failed = false
h7.hook_post(VIEWPORT .. "PostRender", function(_, args)
    if not con.open then return end
    pull_log()
    local ok, err = pcall(draw, args.Canvas)
    if not ok and not draw_failed then
        draw_failed = true -- report once, not every frame
        h7.error("drawing failed: " .. tostring(err))
    end
end)

h7.log(("Debug console ready; press %s to open it (H7ModEngine.ini, [Console] Key=)"):format(
    h7.config("Console", "Key", "Tilde")))

function on_unload()
    saved.history = history
    pcall(mod.save, saved)
end
