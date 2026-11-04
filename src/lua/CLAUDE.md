# src/lua/ — Lua runtime and `h7` bindings

| File | Owns |
|---|---|
| `runtime.cpp/.h` | Lua state, mod discovery/order (`load_after`)/environments, hot reload, enable/disable via ini, mod settings, events, timers, saved data, exec folder, `kVersion` |
| `bindings.cpp` | The `h7` table; `ObjRef`/`StructRef`/`ArrayRef` proxies; marshaling (`PushElem` engine→Lua, `WriteElem` Lua→engine); `CallUFunction`; hook API |

## Adding or changing API

1. Implement as `H_name` and add `{"name", H_name}` to the registration table at the end of the file. Object
   helper methods go in `H7ObjectMethods` and are **snake_case** so they never collide with UnrealScript members.
2. Validate inputs (`CheckValidObject`), raise misuse with `luaL_error` and a message saying what was expected.
3. Document in `docs/API.md` (section + reference table) **and** `docs/reference/*.md`; add a check to
   `tests/selftest.lua`.

## Rules

- Anything a mod registers (hooks, handlers, timers) must be tagged with its mod id so unload removes it. Run
  callbacks inside `ModScope(mod)` so what they register belongs to that mod and log lines carry its name.
- Callback errors are caught with a traceback (`Report`) and logged; the hook/handler stays installed.
- Mod load/unload (e.g. `h7.set_mod_enabled`) happens on the next tick, never inside the caller.
- Values pointing into a hook's parameters carry a frame id and die when the hook returns; struct/array return
  values are copied to tables. Unhooking during dispatch must be safe (listeners have a `removed` flag).
- Call parameters start zeroed (missing args are 0, not UnrealScript defaults). Strings/arrays left in the
  parameter buffer are freed after the call (`FreeOwned`). Names must already exist in the name table unless
  created through `h7.name`.
- Lua is compiled as C++: Lua errors unwind as exceptions, so RAII in binding code is safe.
- Changes here need a rebuild and a game restart; verify with `tests/selftest.lua` (and `settingstest.lua` for
  mod/settings code).
