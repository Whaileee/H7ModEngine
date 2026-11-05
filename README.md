# H7ModEngine

A Lua 5.4 modding engine for **Might & Magic Heroes VII** (v2.2.1, 64-bit).

Mods are plain Lua files. They can read and change any game object, call any UnrealScript function, and hook game
functions to watch, change or block what they do. Mods reload automatically while the game runs, and players turn
them on and off from a **Mods** entry in the main menu.

The engine loads as a `dinput8.dll` next to the game executable. It does not patch game files or game code: it finds
the engine's internals in memory at startup and works through them.

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

## Features

- **Full access to the game's objects**: properties (including structs and arrays) read and written in place,
  UnrealScript functions called with normal Lua arguments, every class, enum and function listed by name.
- **Hooks** on any UnrealScript function, including calls made from script to script: run code before or after a
  function, read and change its arguments, block it or replace its return value.
- **Events and timers**: every frame, map changes, mods loaded or toggled; timers on game time.
- **Mod management**: per-mod manifests, load order, isolated globals, saved data, hot reload, and a Mods menu
  in the game's main menu (built on the game's own map selection screen) to enable and disable mods live.
- **Development tools**: an `exec` folder that runs Lua in the running game, a log of everything mods do, and an
  index of every engine object written at startup.
- **Safe failures**: Lua errors are logged with a stack trace and never take the game down.

## Requirements

- Might & Magic Heroes VII **v2.2.1**, 64-bit, Windows (the Ubisoft Connect version). Other versions may not work.
- Single player. Multiplayer is untested and will likely desync when a mod changes gameplay.

## Installation

1. Download `dinput8.dll` from the [latest release](https://github.com/Whaileee/H7ModEngine/releases/latest).
2. Put it in `<Game>\Binaries\Win64\` (next to `MMH7Game-Win64-Shipping.exe`).
3. Start the game through Ubisoft Connect as usual.

On the first start the engine creates `<Game>\H7ModEngine\` with its configuration and the Mods menu. For the
example mods, also download `H7ModEngine-examples.zip` from the release and unzip it into the game folder (the
folder that contains `Binaries`).

Development builds of `main` are the `H7ModEngine` artifact of the latest
[Build run](https://github.com/Whaileee/H7ModEngine/actions/workflows/build.yml); copy its contents into the game
folder.

To check that it works, open `<Game>\H7ModEngine\logs\engine.log`: it should end with lines like
`Loaded mod HelloWorld` and `Map: councilhub`. The main menu gets a **Mods** entry.

To uninstall, delete `<Game>\Binaries\Win64\dinput8.dll` (and the `H7ModEngine` folder if you don't want to keep
your mods and their saved data).

## Using mods

Each mod is a folder in `<Game>\H7ModEngine\mods\`. To install one, copy its folder there; the engine picks it up
on the next start (or at once after `h7.reload()`).

Open **Mods** in the main menu to see every installed mod with its version, category, author and description.
Select a mod and press **Enable** or **Disable**: the change applies immediately and is remembered in
`H7ModEngine.ini`.

The engine ships with these example mods:

| Mod | What it does | Shipped |
|---|---|---|
| HelloWorld | Logs a greeting, counts launches and session time | on |
| DailyGold | The human player gets bonus gold every new day | off |
| CombatXP | Heroes earn more experience from combat | off |
| DamageScale | Scales combat damage | off |
| HeroMovement | The human player's heroes get more movement points per day | off |
| ModManager | The Mods entry in the main menu (hidden from its own list) | on |
| DebugConsole | In-game Lua console and live log for mod makers; key in `H7ModEngine.ini`, `[Console] Key=` (default Tilde) | off |

## Writing mods

Start with **[docs/API.md](docs/API.md)**, the modder's reference: your first mod in five lines, the manifest,
how to find the game function or property you want to change, hooks, events, saved data and the full `h7` API.
The same guide and a complete **[API reference](https://whaileee.github.io/H7ModEngine/reference/)** are published
for each release at <https://whaileee.github.io/H7ModEngine/>.

A mod is at least a `main.lua`:

```
<Game>\H7ModEngine\mods\MyMod\
    mod.lua    -- optional manifest: name, version, author, description, category, load_after, enabled
    main.lua   -- runs when the mod loads
```

Save `main.lua` while the game runs and the mod reloads within a second.

## Building from source

You need Windows, Visual Studio 2022 (or the Build Tools) with the C++ x64 tools, and Git.

```bash
git clone --recursive https://github.com/Whaileee/H7ModEngine.git
```

```bash
build.bat
```

`build.bat` writes `bin\dinput8.dll`. `build.bat deploy` also copies it, the ini, ModManager and DebugConsole into the game
folder; `build.bat deploy examples` adds the example mods. Set `H7_GAME_DIR` to point it at your installation. See **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)**
for the architecture, the development loop, in-game testing and the reverse-engineering notes.

## How it works

1. Windows loads `dinput8.dll` from the game folder. The engine's copy forwards every DirectInput call to the
   system DLL and starts an init thread.
2. The game executable is packed, so the init thread waits until it has unpacked and built its name and object
   tables, then finds them in memory and works out the engine's data layout by inspecting known objects.
3. It hooks the game viewport's per-frame `PostRender` call and starts Lua there, on the game thread.
4. Mods call into the engine through reflection: properties by offset, functions through `ProcessEvent`, and
   hooks by rewriting a function's bytecode into a small trampoline that calls back into the engine.

The details are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#architecture).

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Engine source (C++20): DLL proxy, UE3 discovery and reflection, hooks, Lua runtime and bindings |
| `dist/` | What gets installed into `<Game>\H7ModEngine\`: default configuration and the example mods |
| `docs/` | [API.md](docs/API.md) (modders), [DEVELOPMENT.md](docs/DEVELOPMENT.md) (engine developers) |
| `tests/` | In-game test and tracing scripts, and the tools that run them |
| `re/` | Reverse-engineering helpers (PE and Flash movie inspection) |
| `third_party/lua` | Lua 5.4.7 (git submodule) |
| `RESUME.md` | Running engineering notes: findings, verified layouts, history of in-game runs |

## Status

Version 0.1.0. The engine, hooks and all example mods are verified in game on v2.2.1. The API may still change.

## Disclaimer

H7ModEngine is an unofficial fan project. It is not affiliated with or endorsed by Ubisoft. Might & Magic and
Heroes are trademarks of Ubisoft Entertainment. Use mods at your own risk and keep backups of your saves.
