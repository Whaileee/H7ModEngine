return {
    name = "OnlineLobby",
    version = "0.2.0",
    author = "H7ModEngine",
    description = "Online multiplayer without Ubisoft's servers: the Online menu hosts and finds games through the "
        .. "community lobby (H7Lobby, [Online] LobbyUrl in H7ModEngine.ini). The host's PC runs the game, like LAN; "
        .. "other players connect to it directly, so the host needs UDP and TCP port 27015 open (the lobby lists a "
        .. "game only after checking that it can reach the host on TCP 27015). Online > Join lists "
        .. "the lobby's games with the host's edition (base or modded), engine version and whether they match yours.",
    category = "Multiplayer",
    enabled = true,
    settings = {
        { key = "upnp", label = "Open port with UPnP", default = true,
          description = "When hosting, ask the router to forward UDP and TCP port 27015 to this PC (works on most home "
              .. "routers). UDP carries the game; TCP answers the lobby's check that the game is really hosted here." },
        { key = "show_mismatched", label = "Show incompatible games", default = true,
          description = "List games whose host has a different game version, H7ModEngine version, edition, "
              .. "content packs or gameplay mods (Status: Differs; joining asks first). Differences usually make the "
              .. "game go out of sync." },
        { key = "block_redirects", label = "Block redirects", default = true,
          description = "While in a game joined through the lobby, refuse the host's requests to send this game to "
              .. "another server (it disconnects instead). Turn off only if a normal game ever gets disconnected "
              .. "by it." },
    },
}
