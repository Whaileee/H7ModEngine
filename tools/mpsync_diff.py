#!/usr/bin/env python3
"""Compare two MPSyncDebug logs (logs\\mpsync-host-*.log and logs\\mpsync-client-*.log) and print where they diverge.

    python tools/mpsync_diff.py <host.log> <client.log>
    python tools/mpsync_diff.py <one.log>            # only the alerts of one log

Reports, in this order:
  1. Alerts of each log: frame gaps, bursts, late/dropped/stuck commands, out-of-sync checks, repairs.
  2. Command collisions: one action counter (UAC) used for different commands on the two PCs, and commands one PC
     never got.
  3. The first synced random draw that differs (with what each PC did just before it).
  4. The first counter step whose RNG or ID counter differs.
  5. The first state snapshot that differs at the same UAC, line by line.
Works on MPSyncDebug log files and on desync reports (tools/fetch_reports.py saves them in the same format).
Line format and tags: dist/lib/mpsync.lua; background: docs/DESYNC.md.
"""
import re
import sys
from collections import Counter

LINE = re.compile(r"^\s*(-?[\d.]+) u=(-?\d+) r=(-?\d+) (\S+)(?: (.*))?$")
ALERTS = {"GAP", "BURST", "DROP?", "STUCK", "WAIT", "OOS", "OOSTYPE", "OOSREMOTE", "SYNCHSTART", "SYNCHREQ",
          "SYNCHASK", "SYNCH", "UACSET", "RNGSET", "CANCELLED", "SIMCOMBATEND", "MAP", "LATE", "ILATE"}
CONTEXT = 8


def field(detail, name):
    m = re.search(r"\b" + re.escape(name) + r"=(\S+)", detail or "")
    return m.group(1) if m else None


class Log:
    def __init__(self, path):
        self.path = path
        self.header = []
        self.events = []  # dicts: i, t, u, r, tag, detail, body
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                raw = raw.rstrip("\n")
                if raw.startswith("#"):
                    self.header.append(raw)
                elif raw.startswith("  | ") and self.events:
                    self.events[-1]["body"].append(raw[4:])
                else:
                    m = LINE.match(raw)
                    if m:
                        t, u, r, tag, detail = m.groups()
                        self.events.append(dict(i=len(self.events), t=float(t), u=int(u), r=int(r), tag=tag,
                                                detail=detail or "", body=[]))

    def name(self):
        role = next((field(h, "role") for h in self.header if "role=" in h), None)
        return role or self.path

    def context(self, idx):
        return self.events[max(0, idx - CONTEXT):idx + 1]


def fmt(e):
    return f"{e['t']:10.3f} u={e['u']} r={e['r']} {e['tag']} {e['detail']}".rstrip()


def alerts(log):
    print(f"== {log.name()}: {log.path}")
    for h in log.header:
        print("  " + h)
    counts = Counter(e["tag"] for e in log.events)
    print("  events: " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    shown = 0
    for e in log.events:
        if e["tag"] in ALERTS:
            print("  " + fmt(e))
            shown += 1
    if not shown:
        print("  no alerts")
    print()


def commands(log):
    """UAC -> description of the command that used it on this PC (sent here or queued from the network)."""
    out = {}
    for e in log.events:
        if e["tag"] in ("SEND", "QUEUE"):
            kind = "cmd " + str(field(e["detail"], "cmd")) + " src=" + str(field(e["detail"], "src"))
        elif e["tag"] in ("ISEND", "IQUEUE"):
            kind = "instant " + str(field(e["detail"], "type"))
        else:
            continue
        uac = field(e["detail"], "uac")
        if uac is not None and uac.lstrip("-").isdigit():
            out.setdefault(int(uac), (kind, e))
    return out


def collisions(a, b):
    print("== Command collisions (same UAC, different command) and missing commands")
    ca, cb = commands(a), commands(b)
    found = False
    for uac in sorted(set(ca) | set(cb)):
        if uac in ca and uac in cb:
            if ca[uac][0] != cb[uac][0]:
                found = True
                print(f"  UAC {uac}: {a.name()} ran '{ca[uac][0]}', {b.name()} ran '{cb[uac][0]}'")
        else:
            have, log, other = (ca, a, b) if uac in ca else (cb, b, a)
            # Only a gap inside the other log's range is suspicious (logs start and stop at different times).
            others = cb if have is ca else ca
            if others and min(others) < uac < max(others):
                found = True
                print(f"  UAC {uac}: only on {log.name()} ('{have[uac][0]}'), never queued on {other.name()}")
    if not found:
        print("  none")
    print()


def first_rng_difference(a, b):
    print("== Synced random draws")
    ra = [e for e in a.events if e["tag"] == "RNG"]
    rb = [e for e in b.events if e["tag"] == "RNG"]
    for n, (x, y) in enumerate(zip(ra, rb)):
        if (x["r"], x["detail"]) != (y["r"], y["detail"]):
            print(f"  draw #{n + 1} differs:")
            for log, e in ((a, x), (b, y)):
                print(f"  -- {log.name()} (last {CONTEXT} events before it):")
                for c in log.context(e["i"]):
                    print("     " + fmt(c))
            print()
            return
    print(f"  the first {min(len(ra), len(rb))} draws match ({len(ra)} vs {len(rb)} logged)")
    print()


def first_step_difference(a, b):
    print("== Counter steps (RNG and ID counter when the UAC went up)")
    sa = {}
    for e in a.events:
        if e["tag"] == "STEP":
            sa.setdefault(e["u"], e)
    for e in b.events:
        if e["tag"] != "STEP" or e["u"] not in sa:
            continue
        x = sa[e["u"]]
        if x["r"] != e["r"] or field(x["detail"], "id") != field(e["detail"], "id"):
            print(f"  first difference at UAC {e['u']}:")
            for log, ev in ((a, x), (b, e)):
                print(f"  -- {log.name()}:")
                for c in log.context(ev["i"]):
                    print("     " + fmt(c))
            print()
            return
    print("  all common steps match")
    print()


def first_state_difference(a, b):
    print("== State snapshots (taken while nothing runs)")

    def states(log):
        out, body = {}, {}
        for e in log.events:
            if e["tag"] == "STATE":
                body[field(e["detail"], "h")] = e["body"]
            if e["tag"] in ("STATE", "HASH"):
                h = field(e["detail"], "h")
                out.setdefault(e["u"], (h, e))
        return out, body

    sa, ba = states(a)
    sb, bb = states(b)
    common = sorted(set(sa) & set(sb))
    for u in common:
        (ha, ea), (hb, eb) = sa[u], sb[u]
        if ha != hb:
            print(f"  first difference at UAC {u} ({a.name()} {ha}, {b.name()} {hb}):")
            la, lb = ba.get(ha, []), bb.get(hb, [])
            for line in sorted(set(la) | set(lb)):
                if line in la and line not in lb:
                    print(f"    {a.name()} only: {line}")
                elif line in lb and line not in la:
                    print(f"    {b.name()} only: {line}")
            if not la or not lb:
                print("    (one side has no full snapshot for this hash)")
            print()
            return
    print(f"  {len(common)} common snapshots match")
    print()


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__)
        return 2
    logs = [Log(p) for p in argv[1:]]
    for log in logs:
        alerts(log)
    if len(logs) == 2:
        a, b = logs
        collisions(a, b)
        first_rng_difference(a, b)
        first_step_difference(a, b)
        first_state_difference(a, b)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
