# Online multiplayer replacement — design

Ubisoft's online services for Heroes VII (Ubiservices login, RendezVous matchmaking, Storm NAT traversal) are shut
down, so the **Online** menu entries no longer work. This document describes how an H7ModEngine mod plus a small
community lobby server replace them, without patching game code.

Status (2026-11-05): implemented as the bundled `OnlineLobby` mod (`dist/mods/OnlineLobby`) and the H7Lobby Worker
(https://h7-lobby.whaileee.workers.dev). Verified in game on one PC: the Online menu opens, hosting goes through the
LAN path and is listed, Online > Join opens a lobby browser built on the map selection window (Host | Map | Players
| Version | Status, the map picture, the description and every difference), the "join anyway?" prompt works, and
a join to an unreachable host times out cleanly. **Not yet verified: a real match
between two PCs.** Where the implementation differs from the plan below: the browser is not the game's join menu (its
columns are fixed in its Flash movie and all in use) but `dist/mods/OnlineLobby/browser.lua`; the chosen game
becomes the manager's only search result and joins through `JoinSelected`; lobby entries get a native session record
from `h7.net.set_lan_host` (0x38 bytes, see `src/lua/bindings.cpp`); the lobby's plain TCP reachability probe is
gone (the host's game listens on UDP 27015 only) and was replaced by [host verification](#host-verification): the
mod answers a TCP challenge on 27015 itself (OnlineLobby 0.2.0, builds and passes `build.bat nettest`; not yet
verified in game).

## How the original online flow works

All of it goes through `MMH7Game.H7MultiplayerGameManager` and the online subsystem's `GameInterface`
(`OnlineSubsystemUPlay.OnlineGameInterfaceUPlay`, which extends `IpDrv.OnlineGameInterfaceImpl`, the stock UE3
LAN implementation).

| Step | Script | Online (Ubisoft) | LAN (still works) |
|---|---|---|---|
| Menu gate | `H7MainMenuCntl.MPCreateINET` / `MPJoinINET` / `MPDuel*INET` → `CheckConnection()` | fails: `IsInOfflineMode()` or `GetLoginStatus(0) == NotLoggedIn` → "online services not available" | no gate |
| Host | `H7MultiplayerGameManager.CreateOnlineGame(IsLAN, IsDuel, NumPlayers, ...)` | URL `H7Main?bIsLanMatch=False?listen?stormsocket`, `SetWaitingForStormData()`, `Update()` waits for `IsStormDataAvailable()` before `PublishCurrentGame()` | URL `H7Main?bIsLanMatch=True?listen`, `PublishCurrentGame()` at once |
| Publish | `PublishCurrentGame()` → `GameInterface.CreateOnlineGame(..., 'H7GameSession', settings)` | RendezVous session | LAN beacon on UDP 14001 |
| Search | `SearchOnlineGames(IsLAN, IsDuel)` → `GameInterface.FindOnlineGames` → `OnFindOnlineGamesComplete` | RendezVous query; private games kept only for Uplay friends | LAN broadcast, results after a 3 s timer (`OnLANSearchComplete`) |
| Filter | `StartFilteringBrowserList()` → `H7MeshBeaconClient` per result (TCP **27015**) → `FilterUnreachableServers()` after 3.5 s | same | same |
| Join | `JoinSelected` → `JoinSystemLink` → `GameInterface.JoinOnlineGame` → `OnJoinPartyComplete` → `GetResolvedConnectString` → `ConsoleCommand("start " $ URL)` | Storm-punched address | host address from the LAN result |

Network facts from `MMH7Engine.ini`: game port `[URL] Port=27015`; net driver
`OnlineSubsystemUPlay.IpNetDriverStorm` (falls back to `IpDrv.TcpNetDriver`, which is UE3's UDP driver despite
the name); the Storm socket is only used when the URL carries `?stormsocket`; LAN announce port 14001; mesh beacon
port 27015 (TCP).

**Key point:** the LAN path is a complete, Ubisoft-free multiplayer stack. The only things LAN lacks are an
internet-wide game list and a way through home routers. The replacement therefore runs Online games on the LAN
code path and supplies those two pieces.

## Architecture

```
 Host's game ──(1) register / heartbeat (HTTPS)──▶  Lobby server  ◀──(2) list games (HTTPS)── Joining game
     ▲                                               (GitHub repo,                                │
     │                                               auto-deployed)                               │
     └──────────────(3) game traffic: UDP 27015 + beacon TCP 27015, direct ─────────────────────────┘
```

### 1. Lobby server (new GitHub repo, e.g. `H7Lobby`)

GitHub cannot run a server, so the repo holds the code and a GitHub Actions workflow deploys it on every push.
Recommended host: **Cloudflare Workers + one Durable Object** (free tier is enough, no server to maintain; outbound
TCP `connect()` is available for the reachability probe). Fly.io is the alternative if we later need UDP (a relay
or hole punching).

API (JSON over HTTPS):

| Call | Purpose |
|---|---|
| `POST /games` | Host registers: name, game type (skirmish/duel), map file + map info number, slots, turn type, saved game flag, game/engine/mod versions, port. The server takes the **public IP from the request** (`CF-Connecting-IP`), so the host doesn't need to know it. Returns `id` + secret token. |
| `POST /games/:id/heartbeat` | Every ~15 s; player count / started flag. Games without a heartbeat for 45 s are dropped. |
| `DELETE /games/:id` | Host left or the game started (unless join in progress is wanted). |
| `GET /games?type=duel` | List for the browser. |
| `GET /probe/:id` (internal, on register) | Server tries a TCP connect to `ip:27015`; the result is shown as "reachable" in the list and tells the host if port forwarding failed. |

No accounts. Abuse limits: rate limit per IP and at most a few games per IP.

### 2. Engine additions (H7ModEngine, C++)

- `h7.http.request{method, url, body, headers, timeout}` → async (WinHTTP worker thread), result delivered to a
  Lua callback on the game thread from the tick. JSON via a small bundled Lua library.
- `h7.net.upnp_map(port, proto)` / `upnp_unmap` — Windows `IUPnPNAT` COM API, so most hosts don't have to set up
  port forwarding by hand (the Storm driver did this too: `IsUPnPEnabled=True`).
- `h7.net.make_lan_result(ip, port)` — allocates the native `FSessionInfo` (host `FInternetIpAddr`) that a LAN
  search result's `PlatformData` points to, so a lobby entry can be joined through the game's normal
  `JoinOnlineGame` → `GetResolvedConnectString` path. The layout must be checked in game first (see open questions).

### 3. The mod (`mods/OnlineLobby`, Lua)

All hooks are on script functions, so the bytecode trampoline covers them.

| Hook | Change |
|---|---|
| `H7MainMenuCntl.CheckConnection` (pre, block) | return `true`: the Online buttons open again. |
| `H7MultiplayerGameManager.CreateOnlineGame` (pre) | when `IsLAN == false`, flip it to `true` so the game listens without `?stormsocket` and publishes immediately; remember this is an *online* game. |
| `H7MultiplayerGameManager.OnCreateH7GameSessionComplete` (post) | for online games: UPnP-map 27015 UDP+TCP, `POST /games` from the `H7OnlineGameSettings` fields, start the heartbeat timer. |
| `DestroyOnlineGame` / leaving the lobby / game start | `DELETE /games/:id`, remove the UPnP mapping. |
| `H7MultiplayerGameManager.SearchOnlineGames` (pre) | when `IsLAN == false`: run as a LAN query (so the game sets up its normal result handling) and also `GET /games`. |
| `OnLANSearchComplete` (pre) | before the game filters the list: build an `H7OnlineGameSettings` per lobby entry (`h7.new`), pair it with `make_lan_result(ip, port)` and append it to `GameInterface.GetGameSearch().Results`. |
| `FilterUnreachableServers` | nothing to do if the host is reachable; the mesh beacon ping also gives the list its ping column. |

Join then needs no hook: `JoinSelected` → `JoinOnlineGame` resolves our `PlatformData` to `ip:27015` and the game
connects as on a LAN.

Version safety: the lobby entry carries the game build and the list of enabled mods + versions; the mod hides or
greys out games that don't match.

## Getting through routers

The host must accept UDP and TCP 27015. In order of effort:

1. **UPnP** from the mod (covers most home routers).
2. **Manual port forwarding**, with the lobby's TCP challenge ([host verification](#host-verification)) telling
   the host whether it worked.
3. **Fallback for strict NAT / CGNAT:** players use a virtual LAN (Tailscale, ZeroTier, Radmin VPN); the lobby can
   accept a manually entered host address for this.
4. Later, optional: a **UDP relay** on Fly.io (traffic for a turn-based game is small). Hole punching would need
   control over the engine's own socket and is not planned.

## Host verification

A lobby listing means "connect your game to this address", so before 0.2.0 anyone could list fake games: point
players at a stranger's PC or a hostile host, flood the list, or (over IPv6, which may claim any `ipv4`) evict a
real host's game. Now a game is listed only if the PC at the listed public IP answers a challenge that needs a
secret the registering mod generated.

```
host mod                                      entry Worker (stateless)              Lobby Durable Object
 token = 32 random bytes → 64 hex
 ckey  = HMAC(token, "H7LOBBY challenge key")
 TCP responder :27015 (own thread, key ckey)
 UPnP map UDP 27015 + TCP 27015
 POST /v1/games {..., token}  ──────────────▶ rate-limit binding, validate body
                                              ───── precheck {ip, hostIp} ─────▶ 10/min per IP(/48), bans, caps
                                              TCP connect hostIp:port (3 s)
            ◀──── "H7LOBBY 1 <nonce 32 hex>\n" ────
            ──── hex(HMAC(ckey, nonce)) "\n" ─────▶ compare (constant time)
                                              ───── register (X-Host-Verified) ─▶ store verified:true, list
 201 {id, ip, port, verified:true}  ◀────────  or 403 {error: "...TCP 27015...", ip, port}
 heartbeat / DELETE with Bearer token (heartbeats must come from the registering IP)
 stop: responder off, UPnP unmapped (if this mod made the mapping)
```

Protocol (identical in H7Lobby `src/verify.ts`, its Node reference `test/responder.mjs` and the engine's
`src/net/responder.cpp`; all values ASCII, keys are the bytes of the hex strings):

- `token`: 64 lowercase hex characters, new for every registration (`h7.crypto.random_hex(32)`), sent once over
  HTTPS. The lobby keeps only its SHA-256 (Bearer auth for heartbeat/DELETE) and `ckey`.
- `ckey = hex(HMAC-SHA256(key = token, msg = "H7LOBBY challenge key"))`; only `ckey` is given to the responder, so
  the open TCP port never acts as an oracle for the Bearer secret.
- Challenge: `"H7LOBBY 1 " + nonce + "\n"`, nonce 32 lowercase hex characters, **43 bytes**. Reply:
  `hex(HMAC-SHA256(key = ckey, msg = nonce)) + "\n"`, **65 bytes**. One exchange per connection; anything else, or
  silence for 2 s, is closed without a byte.
- Shared test vector (pinned in `tests/offline/net.lua` and H7Lobby `test/verify.test.ts`):
  token `00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff` →
  ckey `00f01b980cb2a9a08e109cfe39e64298fb77586162ba8ea27efb66ec34edc3cc`;
  nonce `0123456789abcdef0123456789abcdef` →
  reply `5794bf2e5e14ea5f4f7eeca473024143a18e94fa4dd32d5274dd6219a3720350`.

Why it holds: replay is useless (128-bit nonce, per-game key); reflection is impossible (the Worker never answers
challenges); only the owner of an IP can register a game for it, so stranger IPs can't be listed and "replace the
oldest of 3 games per IP" can't be used against someone else. The game listens on UDP 27015 only (measured
2026-11-05), so TCP 27015 is free for the responder; it runs in C++ on its own thread because the game thread
(and Lua) pauses while the window is unfocused, and hosts alt-tab while waiting.

In the mod (`dist/mods/OnlineLobby/main.lua`): the responder starts with the new `ckey` before each registration
(also the re-registration after a heartbeat 404 or 403) and stops, with the UPnP mappings this mod made, when
hosting ends; a failed challenge shows a popup naming the reason and asking to forward UDP and TCP 27015.

Client side, independent of the lobby:

- A game the lobby didn't mark `verified` (a custom or older lobby) gets the compatibility issue "Host not
  verified by the lobby", so joining it always asks first.
- `AllowDownloads` (true in `MMH7Engine.ini` `[IpDrv.TcpNetDriver]`, inherited by `IpNetDriverStorm`) is set to
  false on both net drivers' class defaults (and live drivers) at mod load, so a host can't make the client
  download packages.
- While connected to a game joined through the lobby, a `ClientTravel` to a different host is cancelled
  (setting `block_redirects`, on by default). `ClientTravel` is a native event (hooks get no arguments and can't
  block), so the mod reads the URL in `PreClientTravel`, which the native calls first, and replaces the queued
  travel with `disconnect` in a post-hook on `ClientTravel`. Relative travel (map changes on the same server)
  always passes. Not yet verified in game.

### Residual risks (not solved here)

- Someone with N public IPv4 addresses that accept inbound TCP can list 3N junk games (about 100 addresses fill the
  300-game cap); the lobby owner bans ranges (admin routes, H7Lobby README).
- Verification is point-in-time: a host that keeps heart-beating from the same IP stays listed even if its game
  died. The 60 s TTL bounds a dead host and IP pinning bounds address drift; a periodic re-challenge is possible
  later (the lobby stores `ckey`).
- The TCP check proves control of the address and that TCP 27015 is forwarded, not that UDP 27015 is. UPnP maps
  both and the popup names both; a join to a host without UDP still times out cleanly.
- The free plan's 100k requests/day can be burned by hammering any route (workers.dev has no WAF); the rate-limit
  binding protects the Durable Object, not the quota. Fix if needed: custom domain + WAF rule.
- Host IPs are public to anyone calling `GET /v1/games` (inherent to direct connections, same as LAN), and the
  game's own UE3 netcode on UDP 27015 is reachable by anyone who learns the IP; the mod can't fix UE3.
- The game's mesh beacon (`H7MeshBeaconHost`) also uses TCP 27015 in the original online flow. It doesn't listen
  on the LAN path the mod uses (measured), but if it ever does while the responder holds the port, one of them
  fails to bind (the responder reports "already in use" and the lobby then refuses the game).
- Windows Firewall may ask the host once whether the game may accept connections (the responder listens on every
  interface); declining blocks the challenge.

## Desync reports

Opt-in (OnlineLobby setting "Send desync reports", **off by default**; `dist/mods/OnlineLobby/reports.lua`). While
it is on and a multiplayer game runs, OnlineLobby runs the same lockstep tracer as MPSyncDebug
(`dist/lib/mpsync.lua`, see [DESYNC.md](DESYNC.md)) but keeps only the last 800 lines in memory: nothing goes to disk.

- **Triggers:** the game's own out-of-sync detection (`OOS`, or `OOSREMOTE` when the other player detected it) and
  commands the game will lose (`LATE`/`ILATE`: arrived with a counter already behind ours; `DROP?`, `STUCK`).
  The report goes 5 s after the first trigger (later triggers join it), so it includes the aftermath; at most 3
  per game, 60 s apart.
- **Contents:** the newest trace lines up to 96 KB, a state snapshot taken when sending, and `session` (random per
  game), the lobby game id (hosted or joined; empty on LAN), role, player slot, map, sim turns, game and engine
  versions, gameplay mods, edition and content fingerprint. No player or profile names, no addresses, no saves.
- **Lobby:** `POST /v1/reports` (H7Lobby README "Desync reports"): its own Durable Object, no IP stored, 30 days.
- **Reading them:** `tools/fetch_reports.py --diff` (admin token: `H7LOBBY_ADMIN_TOKEN` in the git-ignored `.env`, or the same environment variable) saves each report as an
  MPSyncDebug-format log under `reports/<game id>/` and runs `tools/mpsync_diff.py` on every game whose host and
  client both reported.

## Phases

1. **In-game checks (no server):** host a LAN game, dump `GameInterface` session data to confirm the `FSessionInfo`
   layout; test that the args of `CreateOnlineGame` can be changed from a pre-hook; test joining a LAN game by
   direct `start ip:27015` between two PCs or over a virtual LAN.
2. **Engine:** `h7.http`, JSON, `h7.net.upnp_*`, `make_lan_result`.
3. **Lobby server repo:** Worker + Durable Object + tests + GitHub Actions deploy (needs the owner's Cloudflare
   account and an API token stored as a repository secret).
4. **Mod:** hooks above, settings in the Mods menu (lobby URL, UPnP on/off, manual host address).
5. Two-PC test over the internet; then publish.

## Open questions

- Is `GetLoginStatus` / `IsInOfflineMode` used anywhere else on the multiplayer path (lobby UI, DLC privileges in
  `AddSharedPrivileges`, `IsFriendUPlay` for private games)? Private games are LAN-filtered out of the friend check
  already because the query is a LAN query.
- Does the client accept a session whose `bIsLanMatch` is true but whose host is on the internet (e.g. LAN tick
  rate, `MaxClientRate` vs `MaxInternetClientRate`)? If needed, set `bIsLanMatch` back to `false` on the settings
  after creation.
- `FSessionInfo` layout in this build (UE3 PC: an `FInternetIpAddr` wrapping a `sockaddr_in`).
- Does `IpNetDriverStorm` behave like `TcpNetDriver` when `?stormsocket` is absent and the Storm services are down?
