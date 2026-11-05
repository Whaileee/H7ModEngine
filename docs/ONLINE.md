# Online multiplayer replacement — design

Ubisoft's online services for Heroes VII (Ubiservices login, RendezVous matchmaking, Storm NAT traversal) are shut
down, so the **Online** menu entries no longer work. This document describes how an H7ModEngine mod plus a small
community lobby server replace them, without patching game code.

Status: design only, written from the decompiled sources (`N:\UDK\Custom\Development\Src`). Nothing here is
verified in game yet; the open questions are listed at the end.

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
2. **Manual port forwarding**, with the lobby probe telling the host whether it worked.
3. **Fallback for strict NAT / CGNAT:** players use a virtual LAN (Tailscale, ZeroTier, Radmin VPN); the lobby can
   accept a manually entered host address for this.
4. Later, optional: a **UDP relay** on Fly.io (traffic for a turn-based game is small). Hole punching would need
   control over the engine's own socket and is not planned.

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
