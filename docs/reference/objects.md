# Engine objects, structs and arrays

Values from the engine reach Lua as three kinds of reference. They are **live**: reading one always gives the
engine's current value, and writing one changes the game immediately. How individual values are converted is in
[Values and types](types.md).

## Object references

An object reference points at an engine object: an actor, hero, player, army, class, function, archetype, ... You
get them from [`h7.first`](h7.md#h7first), [`h7.find`](h7.md#h7find) and friends, from properties and function
results, and as `self` in hooks.

```lua
local hero = h7.first("H7AdventureHero")
h7.log(hero)                       -- H7AdventureHero sce_sephinroth.TheWorld.PersistentLevel.H7AdventureHero_0
h7.log(hero.mName)                 -- read a property (declared in H7Unit)
hero.mName = "Sir Lua"             -- write a property
local player = hero:GetPlayer()    -- call a function
```

Looking up `obj.key` checks, in order:

1. the [helper methods](#helper-methods) below (`get_name`, `is_a`, ...): lowercase, so they never collide with
   UnrealScript members, which are spelled as declared;
2. the object's class and its parents: a **function** gives a callable, a **property** gives its value;
3. otherwise `nil`.

| Operation | Result |
|---|---|
| `tostring(obj)` | Class and path, as [`get_full_name`](#objget_full_name); `"<invalid object>"` once destroyed |
| `a == b` | `true` when both refer to the same engine object |
| `obj.Missing` | `nil` |
| `obj.Missing = v` | Error: `<Class> has no property 'Missing'` |

UnrealScript's `private`, `protected` and `const` don't apply: every property can be read and written.

### Properties

```lua
value = obj.Name
obj.Name = value
```

Reads give the value converted as described in [Values and types](types.md): numbers, strings and booleans are
copies, structs and arrays are live [struct](#struct-references) and [array](#array-references) references into
the object. Writes convert the Lua value to the property's type and raise an error if they can't, e.g.
`object H7Player ... is not a H7AdventureHero` or `number has no integer representation`.

Writing a property only stores the value. If the game caches something derived from it (a GUI text, a combat
stat), call the game function that refreshes it, or change the value through the game's own function instead.

### Calling functions

```lua
r, out1, out2 = obj:FunctionName(arg1, arg2, ...)
```

Calls the UnrealScript function on `obj`, exactly as the game would: the call goes through the engine's
`ProcessEvent`, runs the function's code (or the native code behind it) and any hooks on it.

- **Arguments** are taken in declaration order and converted to the parameter types. Missing or `nil` arguments
  are passed as zero (`0`, `false`, `""`, `None`, empty struct or array). UnrealScript **default values for
  `optional` parameters are not applied**: pass them explicitly when the default isn't zero. Extra arguments are
  ignored.
- **Results**: the return value (if the function has one) comes first, then the final values of the `out`
  parameters in declaration order. Structs and arrays come back as plain Lua tables, since the call's copy is gone
  once it returns.
- Use `:` (`obj:F(...)`). `obj.F` alone is a function that expects the object as its first argument.

```lua
-- function void H7EditorHero.GetDamageRangeBase(out int minDamage, out int maxDamage)
local min_damage, max_damage = hero:GetDamageRangeBase()   -- no return value: only the out parameters
```

**Errors:** `engine object ... is no longer valid`; `function calls are not available yet (ProcessEvent not
discovered)` (only very early during startup); argument conversion errors.

Static functions are called on any instance of the class or on its default object
(`h7.find_class("X"):get_default():StaticFunc()`).

### Helper methods

#### obj:get_name

```lua
obj:get_name() → string
```

The object's own name, e.g. `H7AdventureHero_0` or `NextDay`.

#### obj:get_full_name

```lua
obj:get_full_name() → string
```

Class and path: `H7AdventureHero sce_sephinroth.TheWorld.PersistentLevel.H7AdventureHero_0`. This is what
`tostring` gives, and the form [`h7.find`](h7.md#h7find) accepts.

#### obj:get_path_name

```lua
obj:get_path_name() → string
```

The path without the class: `sce_sephinroth.TheWorld.PersistentLevel.H7AdventureHero_0`.

#### obj:get_class

```lua
obj:get_class() → Class
```

The object's class, as an object reference (`Class MMH7Game.H7AdventureHero`).

#### obj:get_outer

```lua
obj:get_outer() → Object?
```

The object that contains this one (its level, package or owning class), or `nil` for a top-level package.

#### obj:get_default

```lua
class:get_default() → Object
```

On a class: its default object (`Default__ClassName`), which holds the class's default property values. Changing
a property there changes the default for objects created afterwards.

#### obj:get_super

```lua
obj:get_super() → Object?
```

On a class, struct or function: its parent (the class it extends, the function it overrides), or `nil`.

#### obj:get_address

```lua
obj:get_address() → int
```

The object's memory address. Unique while the object lives, so it makes a good table key for per-object caches.
Addresses are reused after an object is destroyed: clear such caches on [`map_loaded`](events.md#map_loaded).

#### obj:is_valid

```lua
obj:is_valid() → bool
```

`false` once the game has destroyed the object (for example when its map unloads). This and `tostring` are the only things
you can use on a destroyed object; everything else raises `engine object 0x... is no longer valid`.

#### obj:is_a

```lua
obj:is_a(class: string | Class) → bool
```

Whether the object's class is `class` or a subclass of it. `class` is a class name or class object.

**Errors:** `unknown class '<name>'`.

```lua
if unit:is_a("H7CreatureStack") then ... end
```

#### obj:call

```lua
obj:call(name: string, ...: any) → any
```

Calls the function `name` like `obj:Name(...)`. Use it when a property and a function share a name (the lookup
finds only one of them) or when the name is in a variable.

**Errors:** `<Class> has no function '<name>'`.

## Struct references

A struct reference is a view of a struct stored inside an object, inside another struct, or in hook arguments.
Reading a field reads the engine's current value; writing one changes it in place.

```lua
local loc = actor.Location          -- struct reference (Vector)
h7.log(loc.X, loc.Y, loc.Z)
loc.Z = loc.Z + 100                 -- moves the stored value; same as actor.Location.Z = ...
```

| Operation | Result |
|---|---|
| `s.Field` | The field's value, or `nil` for an unknown field |
| `s.Field = v` | Writes the field. Error for an unknown field: `<Struct> has no field 'X'` |
| `tostring(s)` | `"<StructName>: 0x..."` |
| `h7.to_table(s)` | Plain table copy of all fields |
| `h7.properties(s)` | The struct's fields |

Assigning to a struct property replaces the whole struct, from a struct reference of the same type or from a
table of fields (fields not in the table keep their values):

```lua
actor.Location = { X = 0, Y = 0, Z = 100 }
other.Location = actor.Location     -- copies the struct
```

A struct reference into an object is only as alive as the object: don't keep one after the object may have been
destroyed.

### Hook arguments

The `args` passed to a [hook](h7.md#h7hook) is a struct reference over the function's parameters: read them by
name, assign them to change what the function receives (in a pre-hook) or what the caller gets back through an
`out` parameter (in a post-hook).

`args`, and every struct or array reference read from it, is **only valid while the hook runs**. Using one later
raises `this value referred to hook parameters and is no longer valid`. Copy what you need with
[`h7.to_table`](h7.md#h7to_table). Object references read from `args` stay valid as long as the object.

## Array references

An array reference is a view of an UnrealScript dynamic array (`array<T>`) stored in an object, a struct or hook
arguments. Indices are **1-based**, like Lua tables.

```lua
local obelisks = player.mVisitedObelisks   -- array<H7Obelisk>
h7.log(#obelisks)
for i = 1, #obelisks do h7.log(obelisks[i]) end
for i, obelisk in ipairs(obelisks) do h7.log(i, obelisk) end
```

| Operation | Result |
|---|---|
| `#arr` | Number of elements |
| `arr[i]` | Element `i`, or `nil` outside `1..#arr` |
| `arr[i] = v` | Replaces element `i`. Error outside `1..#arr`: `array index i out of range (1..n); use :add()` |
| `ipairs(arr)` | Iterates the elements |
| `h7.to_table(arr)` | Plain table copy |

`pairs(arr)` doesn't work; use `ipairs` or a numeric `for`.

Assigning a Lua sequence or another array reference to an array property (or array parameter) replaces its
contents: `obj.mNames = { "a", "b" }`. `nil` holes in the sequence become zero elements.

Arrays **returned by a function call** are plain Lua tables, not references.

### arr:add

```lua
arr:add(value?: any) → int
```

Appends `value` (or a zero element when it's omitted) and returns the new length. Memory comes from the engine's
allocator, so the game can grow, shrink and free the array as usual.

### arr:remove

```lua
arr:remove(i: int)
```

Removes element `i`, shifting the following elements down.

**Errors:** `array index out of range`.
