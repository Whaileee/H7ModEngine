-- Suggestions ("intellisense") and parameter hints for the console's input.
--
-- S.compute(line_before_cursor, whole_text, env, text_before_cursor) -> nil or
--   { items = { item, ... }, replace = characters before the cursor an accepted item replaces,
--     hint = signature hint, command = bool }
--   item = { text, kind = "function" | "method" | "property" | "table" | "value" | "keyword" | "command",
--            detail = signature or type, owner = declaring class (optional) }
--   hint = { label, params = { "name?: type", ... }, ret = "type", active = index of the argument being typed,
--            doc = one-line description (optional) }
--
-- Signatures use one notation everywhere: (name: type, optional?: type) -> results. For game functions, `out`
-- parameters are listed after -> as well, because Lua receives them as extra return values.
--
-- Sources: h7/mod/object helper signatures come from api.lua, generated from docs/reference by
-- tools/docs/console_api.py. Game commands ("/..."): exec functions of the objects console commands are routed
-- to (viewport, console and interactions, local player controller, cheat manager, HUD, pawn, input, game info),
-- read by reflection, plus the console's own list (Console.ManualAutoCompleteList: engine commands such as
-- Stat FPS). Lua: globals, keywords, table fields, and engine objects' properties and functions.

local S = {}

local API = mod.require("api")

local FUNC_EXEC = 0x200
local MAX_ITEMS = 300

local KEYWORDS = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
                   "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while" }

local function lower(s) return s:lower() end

-- ---------------------------------------------------------------------------------------------------------
-- Signatures
-- ---------------------------------------------------------------------------------------------------------

function S.format_sig(params, ret)
    local s = "(" .. table.concat(params or {}, ", ") .. ")"
    if ret and ret ~= "" then s = s .. " -> " .. ret end
    return s
end

-- "static native function bool Core.Object.GetAngularDistance(out Vector2D OutDist, Vector Dir)" ->
-- { params = { "out OutDist: Vector2D", "Dir: Vector" }, ret = "bool, OutDist: Vector2D" }
local function from_unrealscript(sig)
    local ret, args = sig:match("(%S+)%s+[%w_%.]+(%b())%s*$")
    if not args then return nil end
    local params, rets = {}, {}
    if ret ~= "void" then rets[1] = ret end
    for raw in args:sub(2, -2):gmatch("[^,]+") do
        local words = {}
        for w in raw:gmatch("%S+") do words[#words + 1] = w end
        local out, opt = false, false
        while #words > 2 do
            local w = table.remove(words, 1)
            if w == "out" then out = true elseif w == "optional" then opt = true end
        end
        local ty, name = words[1], words[2]
        if name then
            params[#params + 1] = (out and "out " or "") .. name .. (opt and "?" or "") .. ": " .. ty
            if out then rets[#rets + 1] = name .. ": " .. ty end
        end
    end
    return { params = params, ret = table.concat(rets, ", ") }
end

local game_sigs = {} -- ["Owner.Name"] = parsed signature or false
local function game_sig(owner, name)
    local key = owner .. "." .. name
    if game_sigs[key] ~= nil then return game_sigs[key] or nil end
    game_sigs[key] = false
    local cls = h7.find_class(owner)
    if not cls then return nil end
    local ok, path = pcall(cls.get_path_name, cls)
    if not ok then return nil end
    local oks, sig = pcall(h7.signature, path .. "." .. name)
    if oks and sig then game_sigs[key] = from_unrealscript(sig) or false end
    return game_sigs[key] or nil
end

-- Engine object helper methods (obj:get_name, class:get_default, ...) by name, from api.lua.
local HELPERS = {}
for key, overloads in pairs(API) do
    local name = key:match("^obj:([%w_]+)$") or key:match("^class:([%w_]+)$")
    if name then HELPERS[name] = overloads[1] end
end

-- Function values of the documented API (h7.first, print, mod.save, ...) -> their api.lua entry, so aliases
-- (local f = h7.first) and table fields find their signature. Built per environment on first use.
local by_value = setmetatable({}, { __mode = "k" })
local function api_of_value(fn, env)
    if not by_value[env] then
        local map = {}
        for key, overloads in pairs(API) do
            if key:match("^[%a_][%w_]*$") or key:match("^[%a_][%w_]*%.[%a_][%w_]*$") then
                local f = load("return " .. key, "=api", "t", env)
                local ok, v = pcall(f or error)
                if ok and type(v) == "function" then map[v] = overloads end
            end
        end
        by_value[env] = map
    end
    return by_value[env][fn]
end

-- ---------------------------------------------------------------------------------------------------------
-- Game commands
-- ---------------------------------------------------------------------------------------------------------

local commands -- { item, ... } sorted (item.sig = parsed signature of exec functions), or nil until built

local function exec_objects()
    local objs, seen = {}, {}
    local function add(o)
        if o and not seen[o] then
            local ok, valid = pcall(function() return o:is_valid() end)
            if ok and valid then seen[o] = true objs[#objs + 1] = o end
        end
    end
    local function field(o, name)
        if not o then return nil end
        local ok, v = pcall(function() return o[name] end)
        return ok and v or nil
    end
    local vp = h7.first("GameViewportClient")
    add(vp)
    add(field(vp, "ViewportConsole"))
    local interactions = field(vp, "GlobalInteractions") or {}
    for i = 1, #interactions do add(interactions[i]) end
    local engine = field(vp, "Outer")
    if not engine and vp then
        local ok, o = pcall(function() return vp:get_outer() end)
        engine = ok and o or nil
    end
    local players = field(engine, "GamePlayers")
    local pc = players and #players > 0 and field(players[1], "Actor") or h7.first("PlayerController")
    add(pc)
    for _, name in ipairs({ "CheatManager", "myHUD", "Pawn", "PlayerInput" }) do add(field(pc, name)) end
    add(field(field(pc, "WorldInfo"), "Game"))
    return objs
end

local function build_commands()
    local list, index = {}, {}
    local function put(item)
        local k = lower(item.text)
        if index[k] then return end
        index[k] = true
        item.kind = "command"
        list[#list + 1] = item
    end
    for _, obj in ipairs(exec_objects()) do
        local ok, fns = pcall(h7.functions, obj)
        if ok then
            for _, f in ipairs(fns) do
                if f.flags and f.flags & FUNC_EXEC ~= 0 and not index[lower(f.name)] then
                    local sig = game_sig(f.owner, f.name) or { params = {}, ret = "" }
                    put({ text = f.name, detail = S.format_sig(sig.params), owner = f.owner, sig = sig,
                          args = #sig.params > 0 })
                end
            end
        end
    end
    -- The console's own list (commands handled natively, which reflection can't see).
    local vp = h7.first("GameViewportClient")
    local ok, manual = pcall(function() return vp and h7.to_table(vp.ViewportConsole.ManualAutoCompleteList) end)
    if ok and manual then
        for _, e in ipairs(manual) do
            if e.Command and e.Command ~= "" then
                local d = e.Desc or ""
                -- Many entries repeat the command in the description: "Stat FPS (Shows FPS counter)".
                if lower(d):sub(1, #e.Command) == lower(e.Command) then d = d:sub(#e.Command + 1):match("^%s*(.-)%s*$") end
                put({ text = e.Command, detail = d:gsub("^%((.*)%)$", "%1"), owner = "engine" })
            end
        end
    end
    table.sort(list, function(a, b) return lower(a.text) < lower(b.text) end)
    commands = list
    return list
end

-- Commands the game answered with "Command not recognized" (the shipping build lacks many entries of the
-- console's list, e.g. Stat GAME while Stat FPS works): not suggested. Keys are lower-case commands.
local rejected = {}
function S.set_rejected(set) rejected = set or {} end

local function key_of(cmd) return (lower(cmd):match("^%s*(.-)%s*$"):gsub("%s+", " ")) end
S.key_of = key_of

-- Tests the toggle entries of the console's list (Stat ..., Show ...) that `probed` doesn't have yet: each is run
-- twice in the same call (on, then off again), so nothing changes on screen. Unrecognized ones go into
-- `rejected_set`. Needs a PlayerController (h7.exec). Returns how many were tested.
function S.probe(rejected_set, probed)
    local vp = h7.first("GameViewportClient")
    local list = vp and h7.to_table(vp.ViewportConsole.ManualAutoCompleteList) or {}
    local n = 0
    for _, e in ipairs(list) do
        local cmd = tostring(e.Command or ""):match("^%s*(.-)%s*$")
        local first = lower(cmd):match("^(%S+)")
        local k = key_of(cmd)
        if (first == "stat" or first == "show") and not probed[k] then
            local res = tostring(h7.exec(cmd) or "")
            if res:find("Command not recognized", 1, true) then
                rejected_set[k] = true
            else
                h7.exec(cmd) -- toggle back
            end
            probed[k] = true
            n = n + 1
        end
    end
    return n
end

function S.invalidate() commands = nil end
function S.commands() return commands or build_commands() end

local function command_suggestions(before)
    local typed = before:sub(2)
    local list = S.commands()
    -- Entries starting with what was typed, spaces included, so multi-word engine entries ("stat fps")
    -- keep completing after "stat ". Without a space, entries containing it follow the prefix matches.
    local w = lower(typed)
    local items, rest = {}, {}
    for _, c in ipairs(list) do
        local t = lower(c.text)
        if rejected[key_of(t)] then -- skip
        elseif t:sub(1, #w) == w then items[#items + 1] = c
        elseif #w >= 2 and not w:find(" ", 1, true) and t:find(w, 1, true) then rest[#rest + 1] = c end
        if #items >= MAX_ITEMS then break end
    end
    for _, c in ipairs(rest) do
        if #items >= MAX_ITEMS then break end
        items[#items + 1] = c
    end
    local exact = #items == 1 and lower(items[1].text) == w
    if #items > 0 and not exact then return { items = items, replace = utf8.len(typed) or #typed, command = true } end
    -- Typed in full or typing arguments: the command's parameters, the current one highlighted. Longest command
    -- that is a prefix of the line wins.
    local best
    for _, c in ipairs(list) do
        local t = lower(c.text)
        local nxt = w:sub(#t + 1, #t + 1)
        if w:sub(1, #t) == t and (nxt == " " or nxt == "") and (not best or #t > #best.text) then best = c end
    end
    if not best then return nil end
    local args = w:sub(#best.text + 1)
    local _, words = args:gsub("%S+", "")
    local active = words + ((args:match("%s$") or words == 0) and 1 or 0)
    if best.sig then
        if #best.sig.params == 0 then return nil end
        return { hint = { label = best.text, params = best.sig.params, ret = "", active = active,
                          doc = best.owner }, command = true }
    end
    if best.detail ~= "" then return { hint = { label = best.text, doc = best.detail }, command = true } end
end

-- ---------------------------------------------------------------------------------------------------------
-- Lua names and members
-- ---------------------------------------------------------------------------------------------------------

local function value_item(name, v, api_key, env)
    local t = type(v)
    if t == "function" then
        local overloads = (api_key and API[api_key]) or api_of_value(v, env)
        local o = overloads and overloads[1]
        return { text = name, kind = "function", detail = o and S.format_sig(o.params, o.ret) or "" }
    elseif t == "table" then
        return { text = name, kind = "table", detail = "" }
    elseif t == "string" then
        return { text = name, kind = "value", detail = string.format("string %q", #v > 40 and v:sub(1, 40) .. "..." or v) }
    elseif t == "userdata" then
        local ok, s = pcall(tostring, v)
        return { text = name, kind = "value", detail = ok and s or "userdata" }
    end
    return { text = name, kind = "value", detail = t .. " " .. tostring(v) }
end

local member_cache = {} -- [class .. sep] = items (reflection lists are big; reused per class)

local function object_members(obj, sep)
    local okc, cls = pcall(obj.get_class, obj)
    local key = (okc and cls and tostring(cls) or tostring(obj)) .. sep
    if member_cache[key] then return member_cache[key] end
    local items, ranks = {}, {}
    -- Members of the object's own class first, then each super class in turn (h7.functions/properties list
    -- them in that order; the rank is the owner's position).
    local function rank(owner)
        if not ranks[owner] then ranks[#ranks + 1] = owner ranks[owner] = #ranks end
        return ranks[owner]
    end
    local okf, fns = pcall(h7.functions, obj)
    if okf then
        for _, f in ipairs(fns) do
            items[#items + 1] = { text = f.name, kind = "method", detail = "", owner = f.owner, rank = rank(f.owner),
                                  game = true }
        end
    end
    if sep == "." then
        local okp, props = pcall(h7.properties, obj)
        if okp then
            for _, p in ipairs(props) do
                -- type_name: "ViewportClient", "array<H7Creature>"; older engines only have "StructProperty".
                local t = p.type_name or tostring(p.type):gsub("Property$", "")
                items[#items + 1] = { text = p.name, kind = "property", detail = t, owner = p.owner, rank = rank(p.owner) }
            end
        end
    else
        for name, o in pairs(HELPERS) do
            items[#items + 1] = { text = name, kind = "method", detail = S.format_sig(o.params, o.ret), owner = "h7" }
        end
    end
    member_cache[key] = items
    return items
end

-- A game function's signature, looked up only for rows that are shown (h7.signature is slow on big classes).
function S.detail(item)
    if not item.game or item.looked_up then return end
    item.looked_up = true
    local sig = game_sig(item.owner, item.text)
    if sig then item.detail = S.format_sig(sig.params, sig.ret) end
end

-- The expression that ends at the end of s: names, ".", ":", and balanced (...) / [...] (strings inside them
-- are skipped). "x = h7.first("A"):Get" -> h7.first("A"):Get.
local function trailing_expr(s)
    local i = #s
    while i >= 1 do
        local c = s:sub(i, i)
        if c == ")" or c == "]" then
            local depth, j = 0, i
            while j >= 1 do
                local cj = s:sub(j, j)
                if cj == '"' or cj == "'" then
                    local k = j - 1
                    while k >= 1 and not (s:sub(k, k) == cj and s:sub(k - 1, k - 1) ~= "\\") do k = k - 1 end
                    if k < 1 then return nil end
                    j = k
                elseif cj == ")" or cj == "]" then
                    depth = depth + 1
                elseif cj == "(" or cj == "[" then
                    depth = depth - 1
                    if depth == 0 then break end
                end
                j = j - 1
            end
            if j < 1 then return nil end
            i = j - 1
        elseif c:match("[%w_%.:]") then
            i = i - 1
        else
            break
        end
    end
    local e = s:sub(i + 1)
    return e:match("^[%a_]") and e or nil
end

-- Calls the popup may make while the player types: lookups that only read. Methods named Get*/Is* are
-- assumed to be getters (UnrealScript convention).
local SAFE_CALLS = { ["h7.first"] = true, ["h7.find"] = true, ["h7.find_class"] = true, ["h7.find_all"] = true,
                     ["h7.enum"] = true, ["h7.mods"] = true, ["h7.mod_settings"] = true }
local function safe_to_eval(expr)
    for callee in expr:gmatch("([%w_%.:]*)%s*%(") do
        local method = callee:match(":([%w_]+)$")
        if method then
            if not (method:match("^[Gg]et") or method:match("^[Ii]s")) then return false end
        elseif not SAFE_CALLS[callee] then
            return false
        end
    end
    return true
end

local eval_cache = {} -- [expression] = { value, time }: the popup recomputes on every key
local function eval_path(expr, env)
    if not safe_to_eval(expr) then return nil end
    local now = os.clock()
    local hit = eval_cache[expr]
    if hit and now - hit[2] < 1 then return hit[1] end
    local f = load("return " .. expr, "=suggest", "t", env)
    local ok, v = false, nil
    if f then ok, v = pcall(f) end
    v = ok and v or nil
    eval_cache[expr] = { v, now }
    return v
end

local function is_object(v)
    return type(v) == "userdata" and pcall(function() return v:is_valid() end)
end

local function lua_suggestions(before, env)
    -- In a string or comment nothing is suggested.
    local _, quotes = before:gsub("[\"']", "")
    if quotes % 2 == 1 or before:find("%-%-") then return nil end
    local part = before:match("([%w_]*)$")
    local sep = before:sub(-#part - 1, -#part - 1)
    local path = (sep == "." or sep == ":") and trailing_expr(before:sub(1, -#part - 2)) or nil
    if path and path:match("^%d") then path = nil end
    local pool = {}
    if path then
        local base = eval_path(path, env)
        if type(base) == "table" then
            local seen = {}
            for k, v in pairs(base) do
                if type(k) == "string" then seen[k] = true pool[#pool + 1] = value_item(k, v, path .. "." .. k, env) end
            end
            local mt = getmetatable(base)
            if type(mt) == "table" and type(mt.__index) == "table" then
                for k, v in pairs(mt.__index) do
                    if type(k) == "string" and not seen[k] then pool[#pool + 1] = value_item(k, v, nil, env) end
                end
            end
        elseif is_object(base) then
            pool = object_members(base, sep)
        else
            return nil
        end
    else
        part = before:match("([%a_][%w_]*)$")
        if not part then return nil end
        local prev = before:sub(1, #before - #part)
        if prev:match("[%w_%.:]$") then return nil end
        local seen = {}
        for _, t in ipairs({ env, _G }) do
            for k, v in pairs(t) do
                if type(k) == "string" and not seen[k] then
                    seen[k] = true
                    pool[#pool + 1] = value_item(k, v, k, env)
                end
            end
        end
        for _, k in ipairs(KEYWORDS) do
            if not seen[k] then pool[#pool + 1] = { text = k, kind = "keyword", detail = "" } end
        end
    end
    local p = lower(part)
    local items = {}
    for _, it in ipairs(pool) do
        if lower(it.text):sub(1, #p) == p then items[#items + 1] = it end
    end
    if #items == 0 or (#items == 1 and items[1].text == part) then return nil end
    table.sort(items, function(a, b)
        local ea, eb = a.text:sub(1, #part) == part, b.text:sub(1, #part) == part -- exact-case prefix first
        if ea ~= eb then return ea end
        local ra, rb = a.rank or 0, b.rank or 0
        if ra ~= rb then return ra < rb end
        return lower(a.text) < lower(b.text)
    end)
    for i = MAX_ITEMS + 1, #items do items[i] = nil end
    return { items = items, replace = utf8.len(part) or #part }
end

-- ---------------------------------------------------------------------------------------------------------
-- Parameter hints
-- ---------------------------------------------------------------------------------------------------------

-- The innermost call still open at the end of s: position of its "(" and how many commas (at its level) follow.
local function open_call(s)
    local stack, i, n = {}, 1, #s
    while i <= n do
        local c = s:sub(i, i)
        if c == '"' or c == "'" then
            local j = i + 1
            while j <= n and s:sub(j, j) ~= c do
                if s:sub(j, j) == "\\" then j = j + 1 end
                j = j + 1
            end
            if j > n then break end -- inside an unfinished string: still inside the call
            i = j
        elseif c == "-" and s:sub(i + 1, i + 1) == "-" then
            local j = s:find("\n", i, true)
            if not j then return nil end
            i = j
        elseif c == "(" or c == "{" or c == "[" then
            stack[#stack + 1] = { pos = i, ch = c, commas = 0 }
        elseif c == ")" or c == "}" or c == "]" then
            stack[#stack] = nil
        elseif c == "," and #stack > 0 then
            stack[#stack].commas = stack[#stack].commas + 1
        end
        i = i + 1
    end
    local top = stack[#stack]
    if top and top.ch == "(" then return top.pos, top.commas end
end

local function signature_of(callee, env)
    if API[callee] then return API[callee] end
    local base, sep, name = callee:match("^(.-)([%.:])([%a_][%w_]*)$")
    if name then
        if sep == ":" and HELPERS[name] then return { HELPERS[name] } end
        local obj = eval_path(base, env)
        if is_object(obj) then
            for _, it in ipairs(object_members(obj, ":")) do
                if it.text == name and it.game then
                    local sig = game_sig(it.owner, it.text)
                    return sig and { { params = sig.params, ret = sig.ret, doc = it.owner } } or nil
                end
            end
            return nil
        end
    end
    local fn = eval_path(callee, env)
    if type(fn) == "function" then return api_of_value(fn, env) end
end

function S.signature_help(before, env)
    local pos, commas = open_call(before)
    if not pos then return nil end
    local callee = trailing_expr(before:sub(1, pos - 1):gsub("%s+$", ""))
    if not callee or not safe_to_eval(callee:gsub("[%w_]+$", "")) then return nil end
    local overloads = signature_of(callee, env)
    if not overloads then return nil end
    local active = commas + 1
    local o = overloads[#overloads]
    for _, cand in ipairs(overloads) do
        if #cand.params >= active then o = cand break end
    end
    local params = o.params
    if active > #params and #params > 0 and params[#params]:match("^%.%.%.") then active = #params end
    return { label = callee:match("([%a_][%w_]*)$") or callee, params = params, ret = o.ret, active = active,
             doc = o.doc }
end

-- ---------------------------------------------------------------------------------------------------------

function S.compute(before, text, env, before_all)
    if text:sub(1, 1) == "/" then
        if text:find("\n", 1, true) then return nil end
        return command_suggestions(before)
    end
    local r = lua_suggestions(before, env)
    local ok, hint = pcall(S.signature_help, before_all or before, env)
    if ok and hint then
        r = r or {}
        r.hint = hint
    end
    return r
end

function S.clear_cache() member_cache = {} end

return S
