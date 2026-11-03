-- HelloWorld: the smallest useful mod.
-- Edit this file while the game runs; H7ModEngine reloads it automatically.

local data = mod.load()            -- persistent table (H7ModEngine\data\HelloWorld.lua)
data.launches = (data.launches or 0) + 1
mod.save(data)

h7.log("Hello from " .. mod.name .. "! Loaded " .. data.launches .. " time(s).")

h7.on("map_loaded", function(map)
    h7.log("Entered map: " .. map)
end)

-- Report how long the session has been running, once a minute.
local minutes = 0
h7.every(60, function()
    minutes = minutes + 1
    h7.log(("Session time: %d min"):format(minutes))
end)

function on_unload()
    h7.log("Goodbye from " .. mod.name)
end
