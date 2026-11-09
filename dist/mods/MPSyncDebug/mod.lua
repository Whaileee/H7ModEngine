return {
    name = "MPSyncDebug",
    version = "0.2.0",
    author = "H7ModEngine",
    description = "Developer tool for multiplayer desyncs: while a multiplayer game runs, writes every command, "
        .. "action counter step, synced random number and out-of-sync check to logs\\mpsync-<host|client>-<time>.log, "
        .. "plus frame gaps (window in the background), commands that arrive too late to run, and a state snapshot "
        .. "whenever the game is idle. Turn it on for every player, then compare the logs with "
        .. "tools\\mpsync_diff.py (see docs/DESYNC.md).",
    category = "Developer",
    enabled = false,
    settings = {
        { key = "trace_rng", label = "Trace synced random numbers", default = true,
          description = "Log every draw from the game's shared random generator. The first draw that differs "
              .. "between two players points at the code that broke the sync." },
        { key = "gap_seconds", label = "Frame gap threshold (s)", default = 0.5, min = 0.1, max = 5, step = 0.1,
          description = "Log a GAP line when no frame ran for this long (the game stops while its window is in "
              .. "the background)." },
        { key = "snapshots", label = "State snapshots", default = true,
          description = "When no command is running and the action counter moved, log heroes, armies and "
              .. "resources (or combat stacks), so the diff tool can show what differs." },
    },
}
