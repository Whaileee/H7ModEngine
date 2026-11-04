"""Extracts the Heroes VII class reference (the "SDK" section of the docs site) into tools/docs/sdk/*.json.

    python tools/docs/extract_sdk.py --src N:/UDK/Custom/Development/Src --dump <Game>/H7ModEngine/dumps/objects.txt

Inputs:
  --src   Decompiled UnrealScript sources (<Package>/Classes/*.uc) for the game build the engine targets.
  --dump  dumps/objects.txt written by the engine at startup (Dev/DumpSDK=1). It is the ground truth for what
          exists in the running game: which packages and classes are loaded and the objects' real names (the
          decompiler renames some classes, e.g. AH7StructsAndEnumsNative is H7StructsAndEnumsNative in game).

Only declarations are kept: class headers, properties, functions (signatures, no bodies), structs, enums,
constants, states and simple default property values. Classes that exist in game but have no UnrealScript
source (native-only classes) are listed from the dump, with their members' names and property kinds.

Run it again when the targeted game version changes; commit the JSON. The docs site is generated from it by
tools/docs/hooks.py, so release builds don't need the game or the sources.
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict

FUNC_KEYWORDS = {"function", "event", "delegate", "operator", "preoperator", "postoperator"}
FUNC_MODIFIERS = {
    "native", "final", "static", "simulated", "private", "protected", "public", "latent", "iterator", "singular",
    "exec", "reliable", "unreliable", "server", "client", "const", "noexport", "noexportheader", "virtual",
    "demorecording", "coerce", "intrinsic", "k2call", "k2pure", "k2override",
}
PARAM_MODIFIERS = {"optional", "out", "coerce", "const", "init", "skip", "ref"}
VAR_MODIFIERS = {
    "private", "protected", "privatewrite", "protectedwrite", "public", "const", "config", "globalconfig",
    "localized", "transient", "native", "noexport", "editconst", "edithide", "editinline", "editinlineuse",
    "editinlinenew", "noclear", "noimport", "nontransactional", "repnotify", "repretry", "input", "export",
    "deprecated", "duplicatetransient", "interp", "instanced", "databinding", "serializetext", "notforconsole",
    "archetype", "crosslevelpassive", "crosslevelactive", "editoronly", "editfixedsize", "editortextbox",
    "allowabstract",
}

# ---------------------------------------------------------------------------------------------------------------
# Source cleanup
# ---------------------------------------------------------------------------------------------------------------

def strip_comments(text):
    """Removes // and /* */ comments and preprocessor lines, keeping string and name literals intact."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        else:
            out.append(c)
            i += 1
    lines = [l for l in "".join(out).split("\n") if not l.lstrip().startswith("`")]
    return "\n".join(lines)


def match_brace(text, i):
    """Index just after the brace block that starts at text[i] == '{' (strings are skipped)."""
    depth, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = text.find('"', i + 1)
            i = n if j < 0 else j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def split_top(s, sep=","):
    """Splits on `sep` outside of <> () [] {}."""
    parts, depth, cur = [], 0, []
    for c in s:
        if c in "<([{":
            depth += 1
        elif c in ">)]}":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    if "".join(cur).strip():
        parts.append("".join(cur))
    return parts


def norm_ws(s):
    s = re.sub(r"\s+", " ", s).strip()
    s = re.sub(r"\s*<\s*", "<", s)
    s = re.sub(r"\s*>", ">", s)
    return s


def strip_cpp_type(s):
    """Removes C++ type annotations: `Pointer Foo{FBar}` -> `Pointer Foo`."""
    return re.sub(r"\{[^{}]*\}", "", s)


def strip_metadata(s):
    """Removes editor metadata `<Tooltip=...|...>` after names (not generic types like array<int>)."""
    return re.sub(r"(\w)\s*<(?![\w.]+\s*>)[^<>]*>", r"\1", s)

# ---------------------------------------------------------------------------------------------------------------
# Declarations
# ---------------------------------------------------------------------------------------------------------------

TYPE_RE = re.compile(r"^((?:array|class)\s*<.*>|[\w.]+)\s+(.*)$")


def parse_var(stmt):
    """`var(Cat) private const int A, B[4];` -> [{name, type, mods, cats, dim}]."""
    body = strip_cpp_type(strip_metadata(stmt)).strip()
    assert body.startswith("var")
    body = body[3:].strip()
    cats = None
    if body.startswith("("):
        end = body.index(")")
        cats = [c.strip() for c in body[1:end].split(",") if c.strip()]
        body = body[end + 1:].strip()
    mods = []
    while True:
        m = re.match(r"(\w+)(\s*\([^)]*\))?\s+", body)
        if not m or m.group(1).lower() not in VAR_MODIFIERS:
            break
        mods.append(m.group(1).lower())
        body = body[m.end():]
    m = TYPE_RE.match(norm_ws(body))
    if not m:
        return []
    vtype, names = m.group(1), m.group(2)
    out = []
    for raw in split_top(names):
        raw = raw.strip()
        nm = re.match(r"(\w+)\s*(?:\[\s*([\w.]+)\s*\])?", raw)
        if nm:
            v = {"name": nm.group(1), "type": norm_ws(vtype)}
            if mods:
                v["mods"] = mods
            if cats is not None:
                v["cats"] = cats
            if nm.group(2):
                v["dim"] = nm.group(2)
            out.append(v)
    return out


def parse_params(s):
    params = []
    for raw in split_top(s):
        raw = norm_ws(strip_metadata(raw))
        if not raw:
            continue
        default = None
        if "=" in raw:
            raw, default = raw.split("=", 1)
            raw, default = raw.strip(), default.strip()
        mods = []
        while True:
            m = re.match(r"(\w+)\s+", raw)
            if not m or m.group(1).lower() not in PARAM_MODIFIERS:
                break
            mods.append(m.group(1).lower())
            raw = raw[m.end():]
        m = TYPE_RE.match(raw)
        if not m:
            continue
        nm = re.match(r"(\w+)\s*(?:\[\s*([\w.]+)\s*\])?", m.group(2))
        p = {"name": nm.group(1) if nm else m.group(2), "type": m.group(1)}
        if mods:
            p["mods"] = mods
        if nm and nm.group(2):
            p["dim"] = nm.group(2)
        if default is not None:
            p["default"] = default
        params.append(p)
    return params


def parse_function(head):
    """Parses a function header (text up to and including the parameter list)."""
    head = norm_ws(head)
    open_ = head.find("(")
    # operator(24) has its own parentheses: find the parameter list after the name
    m = re.match(r"^(.*?\b(?:operator|preoperator|postoperator)(?:\(\d+\))?\s+.*?\s+(\S+?))\s*\((.*)\)$", head)
    if m:
        before, name, params = m.group(1), m.group(2), m.group(3)
        before = before[: before.rfind(name)].strip()
    else:
        depth, close = 0, len(head) - 1
        before_all = head[:open_]
        # native(123) also has parentheses
        mm = re.match(r"^(.*?)(\w+)\s*\((.*)\)$", re.sub(r"native\s*\(\s*\d+\s*\)", "native", head))
        if not mm:
            return None
        before, name, params = mm.group(1).strip(), mm.group(2), mm.group(3)
    before = re.sub(r"native\s*\(\s*\d+\s*\)", "native", before)
    before = re.sub(r"operator\s*\(\s*\d+\s*\)", "operator", before)
    tokens = before.split()
    mods, kind, ret = [], None, None
    for idx, t in enumerate(tokens):
        tl = t.lower()
        if tl in FUNC_KEYWORDS and kind is None:
            kind = tl
        elif kind is None and tl in FUNC_MODIFIERS:
            mods.append(tl)
        elif kind is not None and tl in FUNC_MODIFIERS and ret is None and idx < len(tokens) - 1:
            mods.append(tl)
        elif kind is not None:
            ret = " ".join(tokens[idx:])
            break
    if kind is None:
        return None
    f = {"name": name, "kind": kind}
    if mods:
        f["mods"] = sorted(set(mods), key=mods.index)
    if ret:
        f["ret"] = norm_ws(ret)
    ps = parse_params(params)
    if ps:
        f["params"] = ps
    return f


def parse_enum(text):
    m = re.match(r"enum\s+(\w+)[^{]*\{(.*)\}", text, re.S)
    if not m:
        return None
    vals = [re.sub(r"<.*", "", v).strip() for v in split_top(strip_metadata(m.group(2)))]
    return {"name": m.group(1), "values": [v for v in vals if re.match(r"^\w+$", v)]}


def parse_defaults(block):
    """Simple `Key=Value` lines of a defaultproperties block (subobject definitions are skipped)."""
    inner = block[block.find("{") + 1: block.rfind("}")]
    out, depth = [], 0
    for line in inner.split("\n"):
        l = line.strip()
        if not l:
            continue
        if re.match(r"begin\s+object", l, re.I):
            depth += 1
            continue
        if re.match(r"end\s+object", l, re.I):
            depth -= 1
            continue
        if depth or "=" not in l:
            continue
        l = l.rstrip(";").strip()
        if len(l) > 300:
            l = l[:297] + "..."
        out.append(l)
    return out


def parse_struct(text):
    """`struct [mods] Name [extends Super] { ... }`."""
    head = text[: text.find("{")]
    head = norm_ws(strip_cpp_type(strip_metadata(head)))
    m = re.match(r"struct\s+(.*?)(\w+)(?:\s+extends\s+([\w.]+))?$", head)
    if not m:
        return None
    s = {"name": m.group(2)}
    mods = m.group(1).split()
    if mods:
        s["mods"] = [x.lower() for x in mods]
    if m.group(3):
        s["super"] = m.group(3)
    body = text[text.find("{") + 1: text.rfind("}")]
    fields, defaults = [], []
    for kind, stmt in statements(body):
        if kind == "var":
            fields.extend(parse_var(stmt))
        elif kind == "structdefaultproperties":
            defaults = parse_defaults(stmt)
    if fields:
        s["fields"] = fields
    if defaults:
        s["defaults"] = defaults
    return s


def statements(body):
    """Yields (kind, text) for the top-level statements of a class, struct or state body."""
    i, n = 0, len(body)
    while i < n:
        while i < n and (body[i].isspace() or body[i] == ";"):
            i += 1
        if i >= n:
            break
        word = re.match(r"\w+", body[i:])
        first = word.group(0).lower() if word else ""
        # Statement ends at ';' (vars, consts, prototypes) or after its '{...}' block.
        j, depth = i, 0
        while j < n:
            c = body[j]
            if c == '"':
                j = body.find('"', j + 1)
                if j < 0:
                    j = n
                j += 1
                continue
            if c == "{":
                if first in ("var", "const") or (first not in BLOCK_KINDS and is_cpp_brace(body, j)):
                    j = match_brace(body, j)
                    continue
                end = match_brace(body, j)
                # a block followed by ';' (struct/enum) belongs to the statement
                k = end
                while k < n and body[k] in " \t\r\n":
                    k += 1
                if k < n and body[k] == ";":
                    end = k + 1
                yield block_kind(first, body[i:j]), body[i:end]
                i = end
                break
            if c == ";":
                yield block_kind(first, body[i:j]), body[i:j]
                i = j + 1
                break
            j += 1
        else:
            yield block_kind(first, body[i:]), body[i:]
            i = n


BLOCK_KINDS = {"struct", "enum", "state", "cpptext", "structcpptext", "replication", "defaultproperties",
               "structdefaultproperties", "auto", "simulated"}


def is_cpp_brace(body, j):
    """`{...}` right after an identifier inside a declaration line is a C++ type annotation, not a body."""
    k = j - 1
    while k >= 0 and body[k] in " \t":
        k -= 1
    line_start = body.rfind("\n", 0, j) + 1
    line = body[line_start:j]
    return k >= 0 and (body[k].isalnum() or body[k] == "_") and "(" not in line and not re.search(
        r"\b(function|event|delegate|state|struct|enum)\b", line)


def block_kind(first, head):
    h = norm_ws(head).lower()
    if first in ("var", "const", "enum", "struct", "cpptext", "structcpptext", "replication", "defaultproperties",
                 "structdefaultproperties"):
        return first
    if re.match(r"^(auto\s+|simulated\s+)*state\b", h):
        return "state"
    if "(" in h:
        return "function"
    if first in ("ignores",):
        return "ignores"
    return "other"


def parse_class_file(path):
    text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
    m = re.search(r"\b(class|interface)\s+(\w+)", text)
    if not m:
        return None
    hdr_end = text.find(";", m.start())
    header = norm_ws(text[m.start():hdr_end])
    cls = {"name": m.group(2), "kind": m.group(1)}
    hm = re.search(r"\bextends\s+([\w.]+)", header)
    if hm:
        cls["super"] = hm.group(1)
    wm = re.search(r"\bwithin\s+([\w.]+)", header)
    if wm:
        cls["within"] = wm.group(1)
    im = re.findall(r"\bimplements\s*\(([^)]*)\)", header)
    if im:
        cls["implements"] = [x.strip() for g in im for x in g.split(",") if x.strip()]
    rest = re.sub(r"^(class|interface)\s+\w+(\s+extends\s+[\w.]+)?(\s+within\s+[\w.]+)?", "", header)
    flags = []
    for fm in re.finditer(r"(\w+)(\s*\(([^)]*)\))?", rest):
        w = fm.group(1).lower()
        if w in ("implements", "hidecategories", "showcategories", "autoexpandcategories", "classgroup",
                 "dontsortcategories", "collapsecategories", "dependson", "inherits"):
            continue
        flags.append(w)
    if flags:
        cls["flags"] = flags

    body = text[hdr_end + 1:]
    props, funcs, consts, enums, structs, states, defaults = [], [], [], [], [], [], []
    for kind, stmt in statements(body):
        try:
            if kind == "var":
                inner = re.match(r"var(\s*\([^)]*\))?\s*(struct|enum)\b", stmt)
                if inner and "{" in stmt:
                    blk = stmt[stmt.find(inner.group(2)): stmt.rfind("}") + 1]
                    (structs if inner.group(2) == "struct" else enums).append(
                        parse_struct(blk) if inner.group(2) == "struct" else parse_enum(blk))
                    tail = stmt[stmt.rfind("}") + 1:].strip().rstrip(";")
                    name = (structs[-1] if inner.group(2) == "struct" else enums[-1])["name"]
                    for v in split_top(tail):
                        if v.strip():
                            props.append({"name": v.strip(), "type": name})
                else:
                    props.extend(parse_var(stmt))
            elif kind == "const":
                cm = re.match(r"const\s+(\w+)\s*=\s*(.*)$", norm_ws(stmt).rstrip(";"), re.S)
                if cm:
                    consts.append({"name": cm.group(1), "value": cm.group(2).strip()})
            elif kind == "enum":
                e = parse_enum(stmt)
                if e:
                    enums.append(e)
            elif kind == "struct":
                s = parse_struct(stmt)
                if s:
                    structs.append(s)
            elif kind == "function":
                head = stmt.split("{", 1)[0] if "{" in stmt else stmt
                head = head[: head.rfind(")") + 1]
                f = parse_function(head)
                if f:
                    funcs.append(f)
            elif kind == "state":
                head = norm_ws(stmt[: stmt.find("{")])
                sm = re.match(r"^((?:\w+\s+)*?)state(\s*\(\s*\))?\s+(\w+)(?:\s+extends\s+(\w+))?", head, re.I)
                if not sm:
                    continue
                st = {"name": sm.group(3)}
                if sm.group(1).strip():
                    st["mods"] = sm.group(1).lower().split()
                if sm.group(4):
                    st["super"] = sm.group(4)
                sbody = stmt[stmt.find("{") + 1: stmt.rfind("}")]
                sfuncs, ignores = [], []
                for k2, s2 in statements(sbody):
                    if k2 == "function":
                        h2 = s2.split("{", 1)[0] if "{" in s2 else s2
                        f = parse_function(h2[: h2.rfind(")") + 1])
                        if f:
                            sfuncs.append(f)
                    elif k2 == "ignores":
                        ignores += [x.strip() for x in norm_ws(s2)[7:].split(",") if x.strip()]
                if sfuncs:
                    st["funcs"] = sfuncs
                if ignores:
                    st["ignores"] = ignores
                states.append(st)
            elif kind == "defaultproperties":
                defaults = parse_defaults(stmt)
        except Exception as e:  # keep going; report at the end
            print(f"warning: {os.path.basename(path)}: {kind}: {e}", file=sys.stderr)
    for k, v in (("consts", consts), ("enums", enums), ("structs", structs), ("props", props), ("funcs", funcs),
                 ("states", states), ("defaults", defaults)):
        if v:
            cls[k] = v
    return cls

# ---------------------------------------------------------------------------------------------------------------
# Runtime dump
# ---------------------------------------------------------------------------------------------------------------

def read_dump(path):
    """objects.txt -> {path: class} for classes, functions, properties, enums, structs and consts."""
    objs = {}
    keep = re.compile(r"^(Class|Function|ScriptStruct|Enum|Const|State|\w+Property)$")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ", 3)
            if len(parts) == 4 and keep.match(parts[2]) and ":" not in parts[3]:
                objs[parts[3]] = parts[2]
    return objs


PROP_KIND = {
    "IntProperty": "int", "FloatProperty": "float", "BoolProperty": "bool", "ByteProperty": "byte",
    "StrProperty": "string", "NameProperty": "name", "ObjectProperty": "Object", "ClassProperty": "class",
    "StructProperty": "struct", "ArrayProperty": "array", "DelegateProperty": "delegate",
    "InterfaceProperty": "interface", "ComponentProperty": "Component", "MapProperty": "map",
}


def class_from_dump(pkg, name, objs, children):
    path = f"{pkg}.{name}"
    cls = {"name": name, "kind": "class", "native_only": True}
    props, funcs, enums, structs, consts = [], [], [], [], []
    for child in sorted(children.get(path, ())):
        kind = objs[child]
        short = child[len(path) + 1:]
        if short.startswith("VfTable_") or (short.startswith("__") and short.endswith("__Delegate")):
            continue  # compiler-generated: interface vtables, delegate storage
        if kind == "Function":
            params, ret = [], None
            for p in sorted(children.get(child, ())):
                pn = p[len(child) + 1:]
                t = PROP_KIND.get(objs[p], objs[p].replace("Property", ""))
                if pn == "ReturnValue":
                    ret = t
                else:
                    params.append({"name": pn, "type": t})
            f = {"name": short, "kind": "function"}
            if ret:
                f["ret"] = ret
            if params:
                f["params"] = params
            funcs.append(f)
        elif kind.endswith("Property"):
            props.append({"name": short, "type": PROP_KIND.get(kind, kind.replace("Property", ""))})
        elif kind == "Enum":
            enums.append({"name": short, "values": []})
        elif kind == "ScriptStruct":
            fields = [{"name": c[len(child) + 1:], "type": PROP_KIND.get(objs[c], objs[c].replace("Property", ""))}
                      for c in sorted(children.get(child, ())) if objs[c].endswith("Property")]
            s = {"name": short}
            if fields:
                s["fields"] = fields
            structs.append(s)
        elif kind == "Const":
            consts.append({"name": short, "value": ""})
    for k, v in (("consts", consts), ("enums", enums), ("structs", structs), ("props", props), ("funcs", funcs)):
        if v:
            cls[k] = v
    return cls

# ---------------------------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="Folder with <Package>/Classes/*.uc")
    ap.add_argument("--dump", required=True, help="dumps/objects.txt from the engine")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "sdk"))
    args = ap.parse_args()

    objs = read_dump(args.dump)
    children = defaultdict(set)
    for p in objs:
        if "." in p:
            children[p.rsplit(".", 1)[0]].add(p)
    game_classes = defaultdict(set)  # package -> class names in game
    for p, k in objs.items():
        if k == "Class" and p.count(".") == 1:
            pkg, name = p.split(".")
            game_classes[pkg].add(name)

    # Decompiled names that differ from the game's: AH7Foo -> H7Foo (and AAH7Foo -> H7Foo).
    renames = {}
    parsed = {}
    for pkg in sorted(game_classes):
        cdir = os.path.join(args.src, pkg, "Classes")
        if not os.path.isdir(cdir):
            continue
        for fn in sorted(os.listdir(cdir)):
            if not fn.endswith(".uc"):
                continue
            c = parse_class_file(os.path.join(cdir, fn))
            if not c:
                print(f"warning: no class in {pkg}/{fn}", file=sys.stderr)
                continue
            real = c["name"]
            while real not in game_classes[pkg] and real.startswith("A") and len(real) > 1:
                real = real[1:]
            if real not in game_classes[pkg]:
                print(f"note: {pkg}.{c['name']} is not in the game; skipped", file=sys.stderr)
                continue
            if real != c["name"]:
                renames[c["name"]] = real
                c["name"] = real
            parsed.setdefault(pkg, {})[real] = c

    rename_re = re.compile(r"\b(" + "|".join(map(re.escape, sorted(renames, key=len, reverse=True))) + r")\b") \
        if renames else None

    def fix(o):
        if isinstance(o, str):
            return rename_re.sub(lambda m: renames[m.group(1)], o) if rename_re else o
        if isinstance(o, list):
            return [fix(x) for x in o]
        if isinstance(o, dict):
            return {k: (fix(v) if k not in ("name",) else v) for k, v in o.items()}
        return o

    os.makedirs(args.out, exist_ok=True)
    total = missing_members = added_members = 0
    for pkg in sorted(game_classes):
        classes = []
        for name in sorted(game_classes[pkg], key=str.lower):
            c = parsed.get(pkg, {}).get(name)
            if c:
                c = fix(c)
                path = f"{pkg}.{name}"
                # drop members the game doesn't have (editor-only, compiled out)
                for key in ("props", "funcs"):
                    if key in c:
                        keep = [m for m in c[key] if f"{path}.{m['name']}" in objs]
                        missing_members += len(c[key]) - len(keep)
                        c[key] = keep
                        if not keep:
                            del c[key]
                # add what the game has but the sources don't declare (Ubisoft's changes to engine packages)
                from_game = class_from_dump(pkg, name, objs, children)
                for key in ("consts", "enums", "structs", "props", "funcs"):
                    have = {m["name"] for m in c.get(key, ())}
                    if key == "funcs":
                        have |= {f["name"] for st in c.get("states", ()) for f in st.get("funcs", ())}
                    extra = [dict(m, undeclared=True) for m in from_game.get(key, ()) if m["name"] not in have]
                    if extra:
                        added_members += len(extra)
                        c[key] = c.get(key, []) + extra
            else:
                c = class_from_dump(pkg, name, objs, children)
            c["package"] = pkg
            classes.append(c)
        total += len(classes)
        with open(os.path.join(args.out, f"{pkg}.json"), "w", encoding="utf-8", newline="\n") as f:
            f.write('{"package": %s, "classes": [\n' % json.dumps(pkg))
            f.write(",\n".join(json.dumps(c, ensure_ascii=False, separators=(",", ":")) for c in classes))
            f.write("\n]}\n")
        print(f"{pkg}: {len(classes)} classes ({sum(1 for c in classes if c.get('native_only'))} native-only)")
    print(f"total {total} classes; renamed {len(renames)}; dropped {missing_members} members not in game; added {added_members} from the game")


if __name__ == "__main__":
    main()
