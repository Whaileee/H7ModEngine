return {
    name = "OnlineLobby",
    version = "0.1.0",
    author = "H7ModEngine",
    description = "Online multiplayer without Ubisoft's servers: the Online menu hosts and finds games through the "
        .. "community lobby (H7Lobby, [Online] LobbyUrl in H7ModEngine.ini). The host's PC runs the game, like LAN; "
        .. "other players connect to it directly, so the host needs UDP and TCP port 27015 open. Listed games show "
        .. "the host's edition (base or modded) and engine version; games that differ are marked [!].",
    category = "Multiplayer",
    enabled = true,
    settings = {
        { key = "upnp", label = "Open port with UPnP", default = true,
          description = "When hosting, ask the router to forward port 27015 to this PC (works on most home routers)." },
        { key = "show_mismatched", label = "Show incompatible games", default = true,
          description = "List games whose host has a different game version, H7ModEngine version, edition, "
              .. "content packs or gameplay mods (marked [!]; joining asks first). Differences usually make the "
              .. "game go out of sync." },
    },
}
