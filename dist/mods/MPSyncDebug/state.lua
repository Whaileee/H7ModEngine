-- Game state snapshot for MPSyncDebug: one line per player, hero army or combat stack, in a fixed order so two
-- peers' snapshots at the same action counter can be compared line by line.

local M = {}

local function try(f)
    local ok, v = pcall(f)
    if ok then return v end
end

local function id_of(o)
    return try(function() return o:GetID() end) or -1
end

local function pos(p)
    if type(p) ~= "table" then return "?" end
    return ("%s,%s"):format(tostring(p.X), tostring(p.Y))
end

local function stacks(list, combat)
    local out = {}
    for i = 1, #list do
        local s = list[i]
        if s then
            local name = try(function() return s:GetStackType():GetName() end) or "?"
            local size = try(function() return s:GetStackSize() end) or -1
            if combat then
                local hp = try(function() return s:GetTopCreatureHealth() end) or -1
                out[#out + 1] = ("%s#%d x%d hp%d @%s"):format(name, id_of(s), size, hp,
                    pos(try(function() return s:GetGridPosition() end)))
            elseif size > 0 then
                out[#out + 1] = ("%s x%d"):format(name, size)
            end
        end
    end
    return table.concat(out, ", ")
end

local function adventure(adv, lines)
    local players = adv.mPlayers
    for i = 1, #players do
        local pl = players[i]
        local num = try(function() return pl:GetPlayerNumber() end) or -1
        local res = try(function() return pl:GetResourceSet():GetAllResourceAmounts() end)
        lines[#lines + 1] = ("player %d res=%s"):format(num, type(res) == "table" and table.concat(res, "/") or "?")
    end
    local armies = adv.mArmies
    local heroes = {}
    for i = 1, #armies do
        local army = armies[i]
        local hero = army and army.mHero
        if hero and try(function() return hero:IsHero() end) then
            local owner = try(function() return army:GetPlayer():GetPlayerNumber() end) or -1
            local cell = try(function() return army.mCell:GetCellPosition() end)
            local mp = try(function() return hero:GetCurrentMovementPoints() end) or -1
            heroes[#heroes + 1] = ("hero %d p%d %s @%s mp=%.2f | %s"):format(id_of(hero), owner,
                try(function() return hero:GetName() end) or "?", pos(cell), mp,
                stacks(army.mBaseCreatureStacks, false))
        end
    end
    table.sort(heroes)
    for _, l in ipairs(heroes) do lines[#lines + 1] = l end
end

local function combat(cc, lines)
    for _, side in ipairs({ "mArmyAttacker", "mArmyDefender" }) do
        local army = cc[side]
        if army then
            lines[#lines + 1] = ("%s %s"):format(side == "mArmyAttacker" and "attacker" or "defender",
                stacks(army.mCreatureStacks, true))
        end
    end
end

-- Lines describing the state, or nil when nothing could be read.
function M.snapshot(adv, cc, in_combat)
    local lines = {}
    if in_combat and cc then
        pcall(combat, cc, lines)
    elseif adv then
        pcall(adventure, adv, lines)
    end
    if #lines == 0 then return nil end
    return lines
end

-- 32-bit FNV-1a of the lines, as 8 hex digits.
function M.hash(lines)
    local h = 2166136261
    for _, l in ipairs(lines) do
        for i = 1, #l do
            h = ((h ~ l:byte(i)) * 16777619) & 0xffffffff
        end
        h = ((h ~ 10) * 16777619) & 0xffffffff
    end
    return ("%08x"):format(h)
end

return M
