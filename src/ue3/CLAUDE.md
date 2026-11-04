# src/ue3/ — UE3 internals

The most fragile code in the repo: it reads and rewrites live engine structures of a packed, protected exe.
Full design: `docs/DEVELOPMENT.md` "Architecture" / "Hooks in detail"; history and verified facts: `RESUME.md`.

| File | Notes |
|---|---|
| `core.*` | GNames/GObjects discovery, names, object iteration, FindObject/FindClass |
| `layout.h` | Calibrated offsets (`ue3::L`). Confirmed values are tabulated in `RESUME.md` |
| `reflection.*` | Calibration (only after `Engine.Actor.Touch` exists; function pointers only after the viewport exists), property/function/enum helpers, `EngineRealloc` |
| `hooks.*` | ProcessEvent discovery/calls; script hooks = bytecode trampoline; natives = `UFunction::Func` swap |
| `factory.*` | `StaticConstructObject`, `FName(TCHAR*, FNAME_Add)`, `GetTransientPackage` at fixed v2.2.1 RVAs, verified by first bytes |
| `dumper.*` | names/objects/probe/unpacked-image dumps (`[Dev]` ini settings) |

## Invariants — break one and the game crashes with `Unknown code token`

- Trampoline layout: `[original optional-param entries] 04 1C <fake native UFunction*> 16 04 0B 53`.
  The optional-param entries (`49 <u16 len> <expr> 15` or a lone `0B`) **must stay at the front**: the VM steps
  one per optional param while binding the call. If the parser can't recognise them, fall back to the Func swap —
  never guess.
- Script→script calls bypass ProcessEvent (the VM calls ProcessInternal directly); only the trampoline sees them.
- Out params on script→script calls are reached through `FFrame::OutParms`, not the callee's locals.
- Native hooks get no params and cannot block. Hooking a bodiless probe event (e.g. `GameViewportClient.Tick`)
  never fires.
- `hooks::Maintain()` re-applies hooks the engine re-bound; keep it called every tick.

## Verifying addresses

Addresses in the log are `game+<rva>`. Disassemble the unpacked image (the on-disk exe is packed and useless):

```bash
cdb -z "<Game>\H7ModEngine\dumps\MMH7Game_unpacked.exe" -c "u game+<rva>; q"
```

(The image comes from `DumpImage=1`; a copy may be in `re/dump/`.) Record any new fixed RVA, how it was found and
its check bytes in `RESUME.md`.
