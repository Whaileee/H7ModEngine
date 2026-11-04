# Values and types

How UnrealScript values are converted when Lua reads them (properties, function results, hook arguments) and
writes them (property assignment, function arguments, hook argument changes, post-hook return values).

| UnrealScript | Read as | Written from |
|---|---|---|
| `int` | integer | integer, or a float with an integer value (`3.0`). `3.5` raises `number has no integer representation` |
| `byte`, enum | integer (enums: the value's index; see [`h7.enum`](h7.md#h7enum)) | integer (only the low 8 bits are kept) |
| `float` | number | number |
| `bool` | boolean | any value: `nil` and `false` are false, everything else true (including `0`) |
| `string` | string | string or number |
| `name` | string | string or number. A name the engine doesn't know yet is added to its name table |
| object | object reference or `nil` | object reference or `nil`. Must be an instance of the property's class: `object X is not a Y` |
| `class<T>`, interface, component | object reference or `nil` | object reference or `nil` (not type-checked) |
| struct | [struct reference](objects.md#struct-references) | struct reference of the same type (copied), or a table of fields |
| `array<T>` | [array reference](objects.md#array-references) | Lua sequence or array reference (replaces the contents) |
| fixed array (`int X[4]`) | Lua table copy (1-based) | Lua table; `nil` entries leave the element unchanged |
| delegate | `{ object = obj, func = "Name" }` | the same shape, or `nil` to clear |
| `qword` | integer | integer |

Other property types read as `nil` and can't be written (`assigning <type> properties is not supported`).

## Function results

Function calls and hooks convert in the same way, with one difference: **structs and arrays returned by a function
call** (return value and `out` parameters) are plain Lua tables, since the engine's copy only lives during the
call. In hooks, `args` and `result` are references valid while the hook runs (see
[Hook arguments](objects.md#hook-arguments)).

## Zero values

Missing or `nil` function arguments, zero elements added with `arr:add()`, and the result of a blocked call
use the type's zero value:

| Type | Zero |
|---|---|
| numbers, enums | `0` |
| `bool` | `false` |
| `string`, `name` | `""` / `None` |
| object, class | `None` (`nil` in Lua) |
| struct | every field zero |
| array | empty |

## Structs from tables

Assigning a table to a struct writes the fields it names, converting each by its type; fields it doesn't name
keep their values. Nested structs and arrays take nested tables:

```lua
actor.Location = { X = 0, Y = 0, Z = 100 }
widget:Invoke("setTextColor", { { Type = h7.enum("ASType").AS_Number, n = 0x9be37a } })  -- array<ASValue>
```

A key that isn't a field raises `struct <Name> has no field '<key>'`. Keys that aren't strings are ignored.

## Delegates

A delegate value is the object and the name of the function to call:

```lua
widget:AddEventListener("CLIK_click", { object = cntl, func = "Options" })
```

Writing one checks that the object's class has a function called `func`
(`delegate target has no function '<func>'`). Lua functions can't be used as delegates; hook the target function
instead.

## Strings

Strings are converted between Lua (UTF-8 bytes as given) and the engine's UTF-16 `FString`s. Text read from the
game (localised names, descriptions) comes back as UTF-8.

## Integers and enums

Lua 5.4 distinguishes integers and floats. `int`, `byte` and enum values read as integers; `float` values read as
floats even when whole (`2.0`). Use `math.floor`/`math.tointeger` before writing a computed number to an `int`.
