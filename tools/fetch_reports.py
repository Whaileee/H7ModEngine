#!/usr/bin/env python3
"""Download desync reports from the lobby (admin token needed) and compare each game's host and client report.

    python tools/fetch_reports.py [--game ID] [--session HEX] [--days N] [--out DIR] [--diff] [--lobby URL]

The admin token (the H7Lobby Worker's ADMIN_TOKEN secret) comes from --token, the H7LOBBY_ADMIN_TOKEN environment
variable, or H7LOBBY_ADMIN_TOKEN=... in the repository's .env file (git-ignored), in that order.

Writes <out> (default: reports/ in the repository, git-ignored: reports hold players' game traces)/<game id, or "lan-<session>">/<time>-<role>-<report id>.log: a "# report ..." line with the report's
fields, then the trace itself (the MPSyncDebug log format, so tools/mpsync_diff.py reads it). Reports already
downloaded are skipped. --diff runs tools/mpsync_diff.py on the newest host and client report of every game that
has both. Players send reports only when they turned on OnlineLobby's "Send desync reports"; the API is in the
H7Lobby README ("Desync reports").
"""
import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import mpsync_diff  # noqa: E402

DEFAULT_LOBBY = "https://h7-lobby.whaileee.workers.dev"
REPO = HERE.parent


def dotenv(name):
    """A value from the repository's .env file (KEY=value lines; # comments), or None."""
    try:
        lines = (REPO / ".env").read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    for line in lines:
        key, sep, value = line.strip().partition("=")
        if sep and key.strip() == name:
            return value.strip().strip('"').strip("'") or None
    return None


def get(lobby, path, token):
    req = urllib.request.Request(lobby + path, headers={
        "Authorization": "Bearer " + token, "User-Agent": "H7ModEngine-fetch-reports"})
    with urllib.request.urlopen(req, timeout=30) as res:
        return json.loads(res.read().decode("utf-8"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lobby", default=DEFAULT_LOBBY)
    ap.add_argument("--token", default=os.environ.get("H7LOBBY_ADMIN_TOKEN") or dotenv("H7LOBBY_ADMIN_TOKEN"))
    ap.add_argument("--game")
    ap.add_argument("--session")
    ap.add_argument("--days", type=float, default=30, help="only reports from the last N days (default 30)")
    ap.add_argument("--out", default=str(REPO / "reports"))
    ap.add_argument("--diff", action="store_true", help="compare each game's newest host and client report")
    args = ap.parse_args()
    if not args.token:
        ap.error("no admin token: pass --token, set H7LOBBY_ADMIN_TOKEN, or put H7LOBBY_ADMIN_TOKEN=... in .env")
    lobby = args.lobby.rstrip("/")

    query = {"limit": "500", "since": str(int((time.time() - args.days * 86400) * 1000))}
    if args.game:
        query["game"] = args.game
    if args.session:
        query["session"] = args.session
    reports = get(lobby, "/v1/admin/reports?" + urllib.parse.urlencode(query), args.token)["reports"]
    print(f"{len(reports)} report(s)")

    out = Path(args.out)
    games = {}  # folder -> {role: newest path}
    for r in sorted(reports, key=lambda r: r["created"]):
        folder = out / (r["game"] or ("lan-" + r["session"][:8]))
        stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime(r["created"] / 1000))
        path = folder / f"{stamp}-{r['role']}-{r['id']}.log"
        if not path.exists():
            full = get(lobby, "/v1/admin/reports/" + r["id"], args.token)
            folder.mkdir(parents=True, exist_ok=True)
            mods = ",".join(f"{m['name']}:{m.get('version', '')}" for m in full.get("mods") or []) or "-"
            head = (f"# report id={full['id']} game={full['game'] or '-'} session={full['session']} role={full['role']} "
                    f"player={full.get('player')} trigger={full['trigger']} uac={full['uac']} "
                    f"game_version={full['game_version']} engine={full['engine_version']} "
                    f"edition={full.get('edition') or '-'} map={full.get('map') or '-'} "
                    f"simturns={full.get('simturns')} mods={mods} received={stamp}Z\n")
            path.write_text(head + full["log"], encoding="utf-8")
            print(f"  saved {path}")
        games.setdefault(folder, {})[r["role"]] = path

    if args.diff:
        for folder, roles in sorted(games.items()):
            if "host" in roles and "client" in roles:
                print(f"\n######## {folder.name}")
                mpsync_diff.main(["mpsync_diff", str(roles["host"]), str(roles["client"])])
            else:
                print(f"\n######## {folder.name}: only {', '.join(roles)} sent a report; nothing to compare")
    return 0


if __name__ == "__main__":
    sys.exit(main())
