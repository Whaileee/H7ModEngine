-- Lab (kept for reference): layout of the LAN session record (FSessionInfo) behind
-- OnlineGameInterfaceImpl.SessionInfo, for building lobby search results. Run at the main menu (exec folder):
-- dumps the record, then hosts a LAN duel (like the menu's Duel > LAN > Create) and dumps it again once hosted.
local function P(...) h7.log("[onlinelab] " .. string.format(...)) end
local base = h7.dev.game_base()
local gi = h7.first("OnlineGameInterfaceUPlay")
local SESSIONINFO_OFF = 336 -- h7.properties: UPlay.SessionInfo Pointer off=336 (2026-11-05)

local function hex(s)
    local out = {}
    for i = 1, #s, 16 do
        local row = {}
        for j = i, math.min(i + 15, #s) do row[#row + 1] = ("%02X"):format(s:byte(j)) end
        out[#out + 1] = ("+%03X  %s"):format(i - 1, table.concat(row, " "))
    end
    return out
end

local function dump(tag)
    local si = h7.dev.u64(gi:get_address() + SESSIONINFO_OFF)
    P("%s: SessionInfo = %s", tag, si and ("%X"):format(si) or "unreadable")
    if not si or si == 0 then return end
    local bytes = h7.dev.read(si, 0x60)
    if not bytes then return P("  unreadable") end
    for _, line in ipairs(hex(bytes)) do P("  %s", line) end
    -- Qwords that point into the game image are vtables or code: report their RVA.
    for off = 0, 0x58, 8 do
        local q = h7.dev.u64(si + off)
        if q and q >= base and q < base + 0x7000000 then
            local first = h7.dev.u64(q)
            P("  +%02X -> game+%X (first entry game+%X)", off, q - base, first and first - base or 0)
        end
    end
    -- sockaddr_in: family 2, port 27015 big-endian (69 87)
    local at = bytes:find("\2\0\105\135", 1, true)
    P("  sockaddr_in(AF_INET, 27015) at %s", at and ("+%X"):format(at - 1) or "not found")
    P("  local ip %s", tostring(h7.net.local_ip()))
end

dump("main menu")
local cntl = h7.first("H7MainMenuCntl")
if not cntl then return P("not at the main menu") end
P("hosting a LAN duel...")
cntl:MPDuelCreateLAN()
for _, t in ipairs({ 3, 8, 15 }) do
    h7.after(t, function()
        dump(("hosted +%ds"):format(t))
        local ok, r1, url = pcall(function() return gi:GetResolvedConnectString(h7.name("H7GameSession"), "") end)
        P("  GetResolvedConnectString: %s %s %s", tostring(ok), tostring(r1), tostring(url))
        if t == 15 then P("DONE") end
    end)
end
