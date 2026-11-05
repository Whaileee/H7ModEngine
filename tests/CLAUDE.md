# tests/ — in-game tests and drivers

Everything runs inside the live game, except `offline/`: `build.bat nettest [online]` builds `nethost.exe` (netlib + `src/net` with stubbed runtime) and runs `offline/net.lua` (JSON always; HTTP/UPnP with `online`; CI runs the JSON part). The user must have the game running (launched via
Ubisoft Connect) and, for UI tests, visible and focused. Script list and what each checks:
`docs/DEVELOPMENT.md` "Testing in game".

## Running

- Copy a script into `<Game>\H7ModEngine\exec\`; it runs once (~1 s), logs to `logs\engine.log`, and is renamed
  to `.done` (even on error, overwriting an older `.done`). Keep sources here, never edit them in `exec\`.
- Automated: `tests/run_ingame.sh <test.lua> <shotdir> [timeout]` (Git Bash). Prints the test's log lines, saves
  a screenshot for each `... SHOT <name>` line, exits 0 on `... DONE`, 1 on `exec: ERROR` or timeout. Put shot
  dirs in the scratchpad, not the repo.
- Exec scripts run in the global environment (`_G` is shared with mods), not a mod environment.
- Minimum before an engine PR: `selftest.lua` (expects all OK) and `hooktest.lua` at the main menu; report
  the results.

## Writing a test

- Drive steps with `h7.after`, log the state you check with one prefix (`[selftest] name  OK  value`), emit
  `SHOT <name>` / `DONE` lines for `run_ingame.sh`. Assert on logged state; screenshots of a covered window show
  a stale frame.
- `*lab.lua` files are experiments kept for reference, not regression tests.

## Input drivers (PowerShell)

- `shot.ps1 <out.png>`: PrintWindow capture of the game window (any monitor).
- `click.ps1 <x> <y> [<toX> <toY>]`: click/drag in window pixels (same frame as screenshots); brings the game to
  the front. The game reads raw mouse input, so the script uses relative/VIRTUALDESK moves — `SetCursorPos`
  alone does nothing. Wait ~2 s after a list refill before clicking.
- `keys.ps1 <key>...`: keybd_event typing (0-9, BACK, RETURN, ESCAPE, PERIOD, MINUS, letters).
- Known positions (2048x1080 window, Polish UI): main menu "Jeden gracz" 1730,330; Mods 1730,890; sub-menu back
  1730,970; map window Back 240,1015, Continue 1810,1015, Generate 1025,978; list rows from y=224 every 49 px.
