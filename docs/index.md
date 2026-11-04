# H7ModEngine

A Lua 5.4 modding engine for **Might & Magic Heroes VII** (v2.2.1, 64-bit).

Mods are plain Lua files. They can read and change any game object, call any UnrealScript function, and hook game
functions to watch, change or block what they do. Mods reload automatically while the game runs, and players turn
them on and off from a **Mods** entry in the main menu.

```lua
-- mods\DailyGold\main.lua: the human player gets 500 gold every new day
h7.hook_post("MMH7Game.H7Calendar.NextDay", function(calendar)
    local controller = h7.first("H7AdventureController")
    local player = controller and controller:GetLocalPlayer(true)
    if not player then return end
    player:GetResourceSet():ModifyCurrency(500, true)
    h7.log("+500 gold for " .. player:GetName())
end)
```

## Where to start

| | |
|---|---|
| **[Modding guide](API.md)** | Installation, your first mod, the manifest, finding what to change, hooks, events, saved data. Read this first. |
| **[API reference](reference/index.md)** | Every function, method, event and value of the Lua API, with parameters, return values and errors. |
| **[Engine development](DEVELOPMENT.md)** | Building the engine, its architecture, testing in game and extending the API. |

This site is built from the documentation in the release it names (top left). The documentation for the code
on `main` lives in the [repository](https://github.com/Whaileee/H7ModEngine/tree/main/docs).

## Getting the engine

Download the latest release from [GitHub](https://github.com/Whaileee/H7ModEngine/releases) and follow
[Installation](API.md#installation).

H7ModEngine is an unofficial fan project. It is not affiliated with or endorsed by Ubisoft. Might & Magic and
Heroes are trademarks of Ubisoft Entertainment. Use mods at your own risk and keep backups of your saves.
