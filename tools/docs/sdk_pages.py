"""Generates the "Game classes" section of the docs site from tools/docs/sdk/*.json (see extract_sdk.py).

One page per class at sdk/<Package>/<Class>.md, an index per package and an overview at sdk/index.md.
Member tables are written as HTML so that types inside declarations can link to their classes.
"""

import html
import json
import os
import re
from collections import defaultdict

SDK_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sdk")

# Heroes VII's own packages first, then the Unreal Engine 3 packages the game ships.
GAME_PACKAGES = ["MMH7Game", "MMH7Engine"]
PACKAGE_INFO = {
    "MMH7Game": "Heroes VII gameplay: heroes, creatures, towns, combat, abilities, the adventure map, GUI controllers",
    "MMH7Engine": "Heroes VII additions to the engine: map data holders, terrain and editor support",
    "Core": "Unreal base classes: Object, structs and math used everywhere",
    "Engine": "Unreal Engine 3 (as modified for Heroes VII): actors, components, controllers, rendering, sound",
    "GameFramework": "Unreal game framework classes",
    "GFxUI": "Scaleform (Flash) UI: GFxMoviePlayer, GFxObject",
    "IpDrv": "Networking and online services",
    "OnlineSubsystemUPlay": "Ubisoft Connect (Uplay) online subsystem",
    "WinDrv": "Windows platform layer",
    "AkAudio": "Wwise audio (native only)",
    "XAudio2": "XAudio2 audio device (native only)",
    "UnrealEd": "Editor classes the game loads",
}
BUILTIN = {"int", "float", "bool", "byte", "string", "name", "pointer", "qword", "double", "map", "delegate",
           "array", "class", "struct", "interface", "void"}
HIDDEN_PROP = re.compile(r"^(VfTable_|__\w+__Delegate$)")
NOTABLE_PROP_MODS = ["const", "config", "globalconfig", "localized", "transient", "native", "editconst",
                     "private", "protected", "privatewrite", "protectedwrite", "repnotify", "instanced"]
FUNC_MOD_ORDER = ["private", "protected", "static", "final", "native", "simulated", "latent", "iterator",
                  "singular", "exec", "reliable", "unreliable", "server", "client", "const"]


class Sdk:
    def __init__(self):
        self.packages = {}  # pkg -> [class]
        self.classes = {}  # name -> class (first package wins on clashes)
        self.by_lower = {}
        self.members = defaultdict(list)  # struct/enum name -> [(class, kind)]
        if not os.path.isdir(SDK_DIR):
            return
        for fn in sorted(os.listdir(SDK_DIR)):
            if fn.endswith(".json"):
                with open(os.path.join(SDK_DIR, fn), encoding="utf-8") as f:
                    data = json.load(f)
                self.packages[data["package"]] = data["classes"]
        order = [p for p in GAME_PACKAGES if p in self.packages] + sorted(
            (p for p in self.packages if p not in GAME_PACKAGES),
            key=lambda p: (p not in PACKAGE_INFO, list(PACKAGE_INFO).index(p) if p in PACKAGE_INFO else 0, p))
        self.order = order
        for pkg in order:
            for c in self.packages[pkg]:
                self.classes.setdefault(c["name"], c)
                self.by_lower.setdefault(c["name"].lower(), c)
                for s in c.get("structs", ()):
                    self.members[s["name"].lower()].append((c, "s", s["name"]))
                for e in c.get("enums", ()):
                    self.members[e["name"].lower()].append((c, "e", e["name"]))
        self.children = defaultdict(list)
        for c in self.classes.values():
            sup = self.resolve_class(c.get("super"))
            if sup:
                self.children[id(sup)].append(c)

    def __bool__(self):
        return bool(self.packages)

    def resolve_class(self, name):
        if not name:
            return None
        return self.by_lower.get(name.split(".")[-1].lower())

    def ancestors(self, c):
        out, seen = [], set()
        sup = self.resolve_class(c.get("super"))
        while sup and id(sup) not in seen:
            seen.add(id(sup))
            out.append(sup)
            sup = self.resolve_class(sup.get("super"))
        return out

# ---------------------------------------------------------------------------------------------------------------
# Links (all class pages live at sdk/<Package>/<Class>/, so links between them are ../../<Package>/<Class>/)
# ---------------------------------------------------------------------------------------------------------------

def class_url(c, from_class_page=True):
    return ("../../" if from_class_page else "../") + f"{c['package']}/{c['name']}/"


def type_html(sdk, t, ctx, from_class_page=True):
    """A type string with every class, struct and enum name linked."""
    chain = [ctx] + sdk.ancestors(ctx) if ctx else []

    def link(m):
        word = m.group(0)
        parts = word.split(".")
        target = None
        if len(parts) >= 2:
            owner = sdk.resolve_class(parts[-2]) if parts[-2] not in sdk.packages else None
            if owner:
                target = member_anchor(owner, parts[-1])
            if not target:
                c = sdk.resolve_class(parts[-1]) if parts[0] in sdk.packages else None
                if c and c["name"].lower() == parts[-1].lower():
                    target = (c, "")
        else:
            if word.lower() in BUILTIN:
                return html.escape(word)
            for c in chain:
                target = member_anchor(c, word)
                if target:
                    break
            if not target:
                c = sdk.by_lower.get(word.lower())
                if c:
                    target = (c, "")
                elif len(sdk.members.get(word.lower(), ())) >= 1:
                    owner, kind, declared = sdk.members[word.lower()][0]
                    target = (owner, f"#{kind}-{declared}")
        if not target:
            return html.escape(word)
        c, anchor = target
        href = (anchor if c is ctx and anchor else class_url(c, from_class_page) + anchor)
        return f'<a href="{href}">{html.escape(word)}</a>'

    return re.sub(r"[A-Za-z_][\w.]*", link, t)


def member_anchor(c, name):
    for s in c.get("structs", ()):
        if s["name"].lower() == name.lower():
            return (c, f"#s-{s['name']}")
    for e in c.get("enums", ()):
        if e["name"].lower() == name.lower():
            return (c, f"#e-{e['name']}")
    return None


def esc(s):
    return html.escape(str(s))


def decl_html(sdk, f, c):
    mods = [m for m in FUNC_MOD_ORDER if m in f.get("mods", ())]
    head = " ".join(mods + [f["kind"]])
    ret = type_html(sdk, f["ret"], c) + " " if f.get("ret") else ""
    params = []
    for p in f.get("params", ()):
        s = " ".join(p.get("mods", ()))
        s = (s + " " if s else "") + type_html(sdk, p["type"], c) + " " + esc(p["name"])
        if p.get("dim"):
            s += f"[{esc(p['dim'])}]"
        if p.get("default"):
            s += f" = {esc(p['default'])}"
        params.append(s)
    return f'<code>{esc(head)} {ret}<strong>{esc(f["name"])}</strong>({", ".join(params)})</code>'


def sorted_by_name(items):
    return sorted(items, key=lambda m: m["name"].lower())

# ---------------------------------------------------------------------------------------------------------------
# Pages
# ---------------------------------------------------------------------------------------------------------------

def class_page(sdk, c):
    pkg, name = c["package"], c["name"]
    path = f"{pkg}.{name}"
    out = [f"# {name}", ""]
    kind = "Interface" if c.get("kind") == "interface" else "Class"
    meta = [f"{kind} in [{pkg}](index.md)", f"`{path}`"]
    flags = [f for f in c.get("flags", ()) if f in ("native", "abstract", "placeable", "transient", "config",
                                                     "perobjectconfig", "editinlinenew", "deprecated")]
    if flags:
        meta.append(", ".join(flags))
    out += [" · ".join(meta), ""]

    anc = sdk.ancestors(c)
    if anc or c.get("super"):
        chain = [f'<a href="{class_url(a)}">{esc(a["name"])}</a>' for a in reversed(anc)]
        if c.get("super") and not anc:
            chain = [esc(c["super"])]
        out += ['<p class="sdk-chain">' + " › ".join(chain + [f"<strong>{esc(name)}</strong>"]) + "</p>", ""]
    if c.get("within"):
        out += [f'<p>Within: {type_html(sdk, c["within"], c)}</p>', ""]
    if c.get("implements"):
        out += ["<p>Implements: " + ", ".join(type_html(sdk, i, c) for i in c["implements"]) + "</p>", ""]
    kids = sorted_by_name(sdk.children.get(id(c), ()))
    if kids:
        out += ["<p>Subclasses: " + ", ".join(f'<a href="{class_url(k)}">{esc(k["name"])}</a>' for k in kids)
                + "</p>", ""]

    if c.get("native_only"):
        out += ['!!! note "Native class"', "    This class has no UnrealScript source. Its members are listed from "
                "the running game; types are the engine's property kinds and function parameters may be "
                "incomplete.", ""]

    funcs = [f for f in c.get("funcs", ())]
    props = [p for p in c.get("props", ()) if not HIDDEN_PROP.match(p["name"])]
    usage = [f'h7.find_all("{name}")            -- live instances',
             f'h7.find_class("{path}")   -- the class']
    if funcs:
        f0 = sorted_by_name(funcs)[0]["name"]
        usage.append(f'h7.hook("{path}.{f0}", fn)')
    out += ['??? tip "Using it from Lua"', "    ```lua"] + [f"    {u}" for u in usage] + ["    ```", ""]

    if c.get("consts"):
        out += ["## Constants", "", "<table><thead><tr><th>Name</th><th>Value</th></tr></thead><tbody>"]
        for k in sorted_by_name(c["consts"]):
            out.append(f'<tr><td id="c-{esc(k["name"])}"><code>{esc(k["name"])}</code></td>'
                       f'<td><code>{esc(k.get("value", ""))}</code></td></tr>')
        out += ["</tbody></table>", ""]

    if c.get("enums"):
        out += ["## Enums", ""]
        for e in sorted_by_name(c["enums"]):
            vals = e.get("values", ())
            out += [f'<h3 id="e-{esc(e["name"])}">{esc(e["name"])}</h3>', ""]
            if vals:
                out.append("<p>" + ", ".join(f"<code>{esc(v)}</code>&nbsp;=&nbsp;{i}" for i, v in enumerate(vals))
                           + "</p>")
            else:
                out.append("<p>Values not available (native enum); read them with <code>h7.enum(\""
                           + esc(e["name"]) + "\")</code>.</p>")
            out.append("")

    if c.get("structs"):
        out += ["## Structs", ""]
        for s in sorted_by_name(c["structs"]):
            sup = f' extends {type_html(sdk, s["super"], c)}' if s.get("super") else ""
            out += [f'<h3 id="s-{esc(s["name"])}">{esc(s["name"])}</h3>', ""]
            if sup:
                out += [f"<p>Struct{sup}</p>", ""]
            fields = s.get("fields", ())
            if fields:
                out.append("<table><thead><tr><th>Field</th><th>Type</th></tr></thead><tbody>")
                for fl in fields:
                    t = type_html(sdk, fl["type"], c) + (f"[{esc(fl['dim'])}]" if fl.get("dim") else "")
                    out.append(f'<tr><td><code>{esc(fl["name"])}</code></td><td><code>{t}</code></td></tr>')
                out.append("</tbody></table>")
            if s.get("defaults"):
                out += ["", '<details><summary>Defaults</summary><pre><code>'
                        + "\n".join(esc(d) for d in s["defaults"]) + "</code></pre></details>"]
            out.append("")

    if props:
        out += ["## Properties", "", "<table><thead><tr><th>Name</th><th>Type</th><th>Notes</th></tr></thead><tbody>"]
        for p in sorted_by_name(props):
            t = type_html(sdk, p["type"], c) + (f"[{esc(p['dim'])}]" if p.get("dim") else "")
            notes = [m for m in NOTABLE_PROP_MODS if m in p.get("mods", ())]
            if p.get("undeclared"):
                notes.append("no declaration: type is approximate")
            out.append(f'<tr><td id="p-{esc(p["name"])}"><code>{esc(p["name"])}</code></td>'
                       f'<td><code>{t}</code></td><td>{esc(", ".join(notes))}</td></tr>')
        out += ["</tbody></table>", ""]

    if funcs:
        out += ["## Functions", "", "<table><thead><tr><th>Name</th><th>Declaration</th></tr></thead><tbody>"]
        for f in sorted_by_name(funcs):
            note = " <em>(no declaration: types are approximate)</em>" if f.get("undeclared") else ""
            out.append(f'<tr><td id="f-{esc(f["name"])}"><code>{esc(f["name"])}</code></td>'
                       f'<td>{decl_html(sdk, f, c)}{note}</td></tr>')
        out += ["</tbody></table>", ""]

    if c.get("states"):
        out += ["## States", "", "State code replaces the class's functions while the object is in that state. "
                f"Hook a state's version as `{path}.<State>.<Function>`.", "",
                "<table><thead><tr><th>State</th><th>Functions</th></tr></thead><tbody>"]
        for st in sorted_by_name(c["states"]):
            label = esc(st["name"]) + (f' <small>extends {esc(st["super"])}</small>' if st.get("super") else "")
            fl = ", ".join(f"<code>{esc(f['name'])}</code>" for f in sorted_by_name(st.get("funcs", ())))
            if st.get("ignores"):
                fl += ("<br>" if fl else "") + "ignores " + ", ".join(f"<code>{esc(i)}</code>" for i in st["ignores"])
            out.append(f'<tr><td id="st-{esc(st["name"])}">{label}</td><td>{fl}</td></tr>')
        out += ["</tbody></table>", ""]

    if c.get("defaults"):
        out += ["## Default values", "", "```", *c["defaults"], "```", ""]

    if anc:
        out += ["## Inherited", "", "<table><thead><tr><th>From</th><th>Members</th></tr></thead><tbody>"]
        for a in anc:
            ap = [p for p in a.get("props", ()) if not HIDDEN_PROP.match(p["name"])]
            af = a.get("funcs", ())
            if a["package"] in GAME_PACKAGES:
                parts = []
                if ap:
                    parts.append("Properties: " + ", ".join(
                        f'<a href="{class_url(a)}#p-{esc(p["name"])}">{esc(p["name"])}</a>' for p in sorted_by_name(ap)))
                if af:
                    parts.append("Functions: " + ", ".join(
                        f'<a href="{class_url(a)}#f-{esc(f["name"])}">{esc(f["name"])}</a>' for f in sorted_by_name(af)))
                body = "<br>".join(parts) or "—"
            else:
                body = f"{len(ap)} properties, {len(af)} functions"
            out.append(f'<tr><td><a href="{class_url(a)}">{esc(a["name"])}</a></td><td>{body}</td></tr>')
        out += ["</tbody></table>", ""]
    return "\n".join(out)


def package_page(sdk, pkg):
    classes = sdk.packages[pkg]
    out = [f"# {pkg}", "", PACKAGE_INFO.get(pkg, "") + ".", "",
           f"{len(classes)} classes. Search the site (top right) for a class or member name, or filter this list:", "",
           '<input class="sdk-filter md-input" type="search" placeholder="Filter classes" '
           'oninput="for(const r of document.getElementById(\'sdk-classes\').tBodies[0].rows)'
           'r.hidden=!r.cells[0].textContent.toLowerCase().includes(this.value.toLowerCase())">',
           "<table id=\"sdk-classes\"><thead><tr><th>Class</th><th>Extends</th><th>Properties</th><th>Functions</th></tr></thead><tbody>"]
    for c in classes:
        sup = sdk.resolve_class(c.get("super"))
        sup_html = f'<a href="../{sup["package"]}/{sup["name"]}/">{esc(sup["name"])}</a>' if sup else esc(c.get("super", ""))
        np = sum(1 for p in c.get("props", ()) if not HIDDEN_PROP.match(p["name"]))
        out.append(f'<tr><td><a href="{esc(c["name"])}/">{esc(c["name"])}</a></td><td>{sup_html}</td>'
                   f'<td>{np}</td><td>{len(c.get("funcs", ()))}</td></tr>')
    out += ["</tbody></table>", ""]
    return "\n".join(out)


def overview_page(sdk):
    total = sum(len(v) for v in sdk.packages.values())
    out = ["# Game classes", "",
           f"Every class the game loads ({total} in {len(sdk.packages)} packages), with its properties, functions, "
           "structs, enums, constants, states and default values. Everything listed here can be reached from Lua: "
           "read and write properties, call functions and hook them (see the [modding guide](../API.md) and the "
           "[API reference](../reference/index.md)).", "",
           "| Package | Classes | |", "|---|---|---|"]
    for pkg in sdk.order:
        out.append(f"| [{pkg}]({pkg}/index.md) | {len(sdk.packages[pkg])} | {PACKAGE_INFO.get(pkg, '')} |")
    out += ["", "## Reading these pages", "",
            "- **Names** are the game's real object names, as `h7.find`, `h7.hook` and `dumps\\objects.txt` use "
            "them. A function's hook path is `Package.Class.Function`.",
            "- **Declarations** come from the game's decompiled UnrealScript and are checked against the objects "
            "that exist in the running game (v2.2.1): members the game doesn't have are left out, and members "
            "it has without a declaration are marked *no declaration*, with the engine's property kind as type.",
            "- **`private` / `protected`** don't restrict Lua: mods can read and write every property and call "
            "every function.",
            "- **`native`** functions are implemented in C++: hooks on them get no arguments and can't block "
            "(see [h7.hook](../reference/h7.md#h7hook)).",
            "- Optional parameters' **default values** are not applied when you call a function from Lua; "
            "pass them explicitly.",
            "- Function bodies are not included. To see what a function does, read the decompiled sources or "
            "hook it and log its arguments.", ""]
    return "\n".join(out)


def nav_section(sdk):
    return [{"Overview": "sdk/index.md"}] + [{pkg: f"sdk/{pkg}/index.md"} for pkg in sdk.order]


def pages(sdk):
    """Yields (src_uri, markdown) for every generated page."""
    yield "sdk/index.md", overview_page(sdk)
    for pkg in sdk.order:
        yield f"sdk/{pkg}/index.md", package_page(sdk, pkg)
        for c in sdk.packages[pkg]:
            if sdk.classes.get(c["name"]) is c:  # skip the rare duplicate class name in a later package
                yield f"sdk/{pkg}/{c['name']}.md", class_page(sdk, c)
