# Multiplayer desync — analysis and fix plan

Status: analysis from the decompiled sources (`N:\UDK\Custom\Development\Src\MMH7Game\Classes`); nothing tested in
game yet. Companion to [ONLINE.md](ONLINE.md).

## How Heroes VII keeps peers in sync

The game uses **command lockstep**, not state replication. Every peer runs the full simulation; only player
commands travel over the network:

- `H7MultiplayerCommandManager` queues `MPCommand`s (move, attack, cast, ...) and instant commands; each carries a
  `unitActionCounter` (UAC), and every peer executes them in UAC order.
- Randomness that must match goes through a shared, seeded generator: `H7ReplicationInfo.GetSynchRNG()`
  (`H7SynchRNG`), used in about 60 classes. Example: `H7GameProcessor` line 424 uses `FRand()` for damage
  *forecasts* and `GetSynchRNG()` for real hits.
- **Out-of-sync detection** already exists: after a human command with `doOOSCheck`,
  `H7AdventurePlayerController.CheckOutOfSynch` sends `{UAC, SynchRNG counter, IDCounter, total units, resources}`
  (`ServerCheckOutOfSynch` → `AddOutOfSynchData` → `UpdateOutOfSynchQueue`).
- **A repair step** also exists: on a mismatch `ServerGameWentOOS` → `ClientStartSynchUp` → `SendSynchUpData`
  pushes RNG counter, ID counter, army stack IDs/sizes and, in combat, every stack's size and top creature
  health (`ServerSynchUp`, `ServerSynchUpArmy`, `ServerSynchUpCombatUnit`).

So the design is sound. Desyncs come from **code that breaks the lockstep rules**, and the repair step only fixes
a few numbers (stack sizes, counters), not positions, buffs, artifacts, skills, buildings or fog of war. Once a
peer diverges in anything else, the game keeps diverging.

## Likely causes found in the sources

### 1. Gameplay decisions made with the unsynced RNG (`Rand` / `FRand`)

These calls use the engine's local RNG, so each peer can roll a different result. Each one still has to be checked:
it's only a bug if **every** peer runs the code. If one peer rolls and sends the result inside a command, it's fine.

| Where | What it decides | Risk |
|---|---|---|
| `H7AdventureController` ~3235, ~3312 | neutral creatures' **negotiation roll** (join / flee) | high: one peer sees the stack join, the other fights it |
| `H7RndSkillManager` 484–711 | skill/ability offers with the **random skill system** | high when that option is on |
| `H7ArcaneAcademy` 162, `H7SchoolOfWar` 179, `H7PermanentBonusSite` 153 | which skill/bonus a site grants | high |
| `H7TownFurnace` 116–119 | resources and gold produced | high |
| `H7Town` 3782 | hero pool in the tavern | medium |
| `H7Town` 3367 | random magic school, passed into a `Command.Init` | probably safe (rolled once, sent) |
| `H7GameProcessor` 1377 | random magic school in combat | to check |
| `H7AiAdventureMap`, `H7AiCombatMap`, `H7AiUtilityHeAbility*`, `H7AiSensorGoodTimeToFleeSurrender` | AI choices | high if AI runs on every peer, safe if only the host runs it and sends commands |

Camera, animation and effects (`*CameraAction`, `H7FracturedMeshActor`, `H7CreatureAnimControl`) use `FRand` too,
and that's harmless.

### 2. The synced RNG consumed by local-only code

The reverse bug: UI, forecasts, tooltips or effects on one peer pull from `H7SynchRNG`, so its counter moves on
that peer only and every later roll differs. The OOS check compares the RNG counter, so this shows up as an
`RNG` mismatch.

### 3. Other classic lockstep breakers (to audit)

- Results that depend on iteration order, e.g. `AllActors` / `DynamicActors` or arrays filled in load order.
- Local settings that change the simulation: quick combat, animation speed, timers that use `RealTimeSeconds`.
- Hidden information handled per peer: fog of war, or "visible to local player" checks inside gameplay code.
- Commands applied before the previous one finished: the latent/timer-driven flow around `UpdateCommandQueue`,
  `UpdateApplyDamageEndTurn` and `IsExpiredApplyDamageEndTurn`.
- Different content: other mods or a different game build on each peer. The lobby can prevent this (see below).

### 4. Host alt-tab (reported by the user, no log yet)

The user remembers desyncs right after the **host** alt-tabbed and came back. What the sources and earlier runs show
(analysis 2026-11-06, not yet reproduced with logging):

- **The host's whole simulation freezes while its window is unfocused.** `bPauseOnLossOfFocus=FALSE` and
  `H7PlayerController.SetPause` is stubbed (always `return false`), so it isn't the UE3 pause: the main thread sits
  in a `SleepEx` loop (seen with `cdb` in run 5, see RESUME.md). The world, the net driver and every timer stop on
  the host; clients keep ticking and keep sending reliable RPCs. On return the host gets one long frame and
  processes the whole backlog at once.
- **Lockstep ordering relies on low latency.** Every command carries the sender's current UAC, and the sender of an
  instant command executes it at once (`H7InstantCommandManager.StartCommand` → `ExecuteInstantCommand`, which
  increments the UAC). Receivers run a queued command only when their UAC equals its stamp. If two peers stamp
  commands with the same UAC, one of them is never run on the other peer (the game's own
  `"!!!!!! Queued command had illegal uac"` path in `H7MultiplayerCommandManager` drops it). A frozen host turns a
  ~100 ms collision window into the whole alt-tab length.
- **Real-time timers that run on every peer:** the sync-up window (`mIsSynchingUp` cleared after 1 s of
  `RealTimeSeconds`; a frozen host can't answer within it), the sim-turn start-combat timeout
  (`H7SimTurnCommandManager.Update`, 60 s), and the combat turn timer in the tactics phase (`NextPlayer()` on
  expiry). Combat commands also wait `CreationTime + 2 s` of game time. The adventure and combat turn timers only
  act for the local player and send a normal command, so they look safe.
- **The game's own MP diagnostics are invisible:** `H7ReplicationInfo.PrintLogMessage` is native and writes
  nothing in the shipping build (no "illegal uac" or OOS lines in any `Launch-backup-*.log`).

To prove or rule out: run the MPSyncDebug traces below with a deliberate host alt-tab (adventure turn, combat,
tactics phase, sim turns), and log tick gaps (real-time frame delta > 1 s) next to the UAC trace.

## Fix plan

### Phase A: find the bugs (instrumentation mod `MPSyncDebug`)

1. **RNG caller trace:** hook `H7SynchRNG`'s getters (script functions, so the trampoline works) and log
   `UAC, counter, calling function` on every call. Diff the host and client logs, and the first line that differs
   names the culprit.
2. **Unsynced RNG trace:** Func-swap hook on `Core.Object.Rand` / `FRand` (natives) that logs the calling script
   function while an MP game is running, filtered to gameplay classes. This confirms the table above in game.
3. **State hash per command:** after each executed command, hash heroes (position, movement points, XP, skills),
   armies (stack IDs + sizes), towns (buildings), player resources and the RNG counter. Peers exchange hashes,
   either through a mod-only reliable function on an existing replicated actor or through the lobby server, and log
   the first UAC where they differ, with a full state dump from both sides.
4. A small script (in `tools/`) that merges the two peers' logs and prints the first divergence.

**Status (2026-11-06): MPSyncDebug 0.1.0 written, Lua only** (`dist/mods/MPSyncDebug`, bundled; category
Developer, so the lobby's compatibility check ignores it; ships off). Smoke-tested offline with stubbed `h7`, **not
yet run in game**. What it covers, by the items above:

- 1: done without the caller: every `GetRandomFloat` / `GetRandomInt` / `GetRandomIntRange` draw with the counter, the
  pool refills and `SetCounter`. Hooks can't see the calling function yet; the lines around a draw (which command
  was running) stand in for it. A `h7.caller()` engine API would add it (DLL change).
- 2: not done (native hooks get no arguments or caller; needs the same engine work).
- 3: done locally instead of exchanged: while nothing runs and the UAC moved, a snapshot of players' resources and
  every hero (position, movement points, army), or every combat stack (size, top HP, cell), with a hash. Towns,
  skills and buffs are not in it yet.
- 4: `tools/mpsync_diff.py host.log client.log`: alerts per log, UAC collisions (one UAC used for different commands
  on the two PCs) and commands one PC never queued, the first differing RNG draw with context, the first differing
  counter step, and the first differing snapshot line by line.

For the alt-tab case (section 4) it also logs the command flow around the UAC: `SEND`/`ISEND` (stamped UAC),
`RELAY` on the host, `RECV`/`IRECV` with `LATE` when the stamp is already behind this PC's counter (the command will
be dropped), `DROP?` from `UnqueuePreparedCommands`, `STUCK`/`WAIT` from a 2 s watchdog over the MP queues, `GAP`
when no frame ran for 0.5 s or more (the background freeze shows up when the window comes back) and `BURST` (three or more
commands in one frame). The game's own OOS check and repair are logged too (`CHECK`, `RCHECK`, `OOS`, `SYNCH*`).

**Test run:** both players turn MPSyncDebug on in the Mods menu, play a LAN or lobby game, and the host alt-tabs
for 20 to 30 s in each situation: during their own adventure turn, during the client's turn, in combat, during
tactics and with sim turns. Each player sends their newest `H7ModEngine\logs\mpsync-*.log` (one per game session),
and the diff script compares the two logs.

### Phase B: fix the causes (`MPSyncFix` mod)

- **Unsynced gameplay rolls:** pre/post hooks on the affected script functions set a "synced scope" flag. While
  it's set, the `Rand`/`FRand` native hooks return values from `H7SynchRNG` instead. Nothing in the game's code
  changes; only those call sites get the synced generator. Where the roll should be made once and sent (like the
  negotiation result), hook it so the deciding peer rolls and the others take the value from the command.
- **Local code eating the synced RNG:** the opposite scope: in forecast/UI paths found by trace 1, serve
  `H7SynchRNG` calls from a local generator.
- Order and setting bugs are fixed one by one as the traces find them.

### Phase C: a stronger recovery

The built-in SynchUp only patches a few numbers. A reliable fallback is a **host-authoritative resync**: when
the mod's state hash differs, the host saves (MP saves already exist and carry `mSaveGameCheckSum`, and the lobby
flow can load an MP save), sends the save to the clients, and everyone reloads into the same turn. That costs a
few seconds, but the game can be finished. Large save transfers may need to go through the lobby server rather than
UE3 replication.

### Prevention through the lobby

The lobby entry from ONLINE.md carries the game build, the enabled mods and their versions, and a hash of
gameplay-relevant content (e.g. archetype packages). The browser refuses mismatched joins, so content
differences can't cause desyncs.

## Order of work

1. Phase A traces 1 and 2. They need only the existing engine, and a LAN game is enough to start.
2. Reproduce known desync cases (community reports: neutral negotiation, random skills, sim turns) with the
   traces running.
3. Phase B fixes, starting with the negotiation roll.
4. Phase C once the online lobby (ONLINE.md) exists, since it needs a transport for saves and hashes.

Testing needs two game instances. Over the internet that needs the online lobby, so the first runs use LAN or a
virtual LAN.
