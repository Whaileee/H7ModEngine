# re/ — reverse-engineering helpers

| Path | In git | What |
|---|---|---|
| `scripts/pe_scan.py <exe>` | yes | Sections, anchor strings, UE3 native tables (`execFoo` pairs) |
| `swfinfo.py <movie.swf> [--strings]` | yes | Tags, symbols, AS3 classes + members of Scaleform movies (dump them with `tests/swfdump.lua`) |
| `dump/` | **no** | Unpacked game image (`MMH7Game_unpacked.exe`, ~100 MB) + early names/objects/probe dumps |
| `ghidra_proj/`, `*.log` | **no** | Ghidra project of the unpacked image |

## Rules

- Never commit anything from `dump/` or `ghidra_proj/` (Ubisoft's binaries; ignored in `.gitignore`).
- The on-disk game exe is packed (`.xtext` decrypted at runtime): only analyse the unpacked image
  (`DumpImage=1` → `<Game>\H7ModEngine\dumps\`).
- Log addresses are `game+<rva>`; disassemble with `cdb -z <unpacked exe> -c "u <addr>; q"`. Image base 0x140000000.
- Before reaching for disassembly, grep the decompiled UnrealScript (`N:\UDK\Custom\Development\Src`) and the
  engine's `dumps\objects.txt` — most questions are answered there.
- Write verified findings (RVAs, layouts, opcodes) into `RESUME.md`; things the engine relies on also go in
  `docs/DEVELOPMENT.md`.
