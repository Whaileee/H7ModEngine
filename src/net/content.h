#pragma once
#include <string>
#include <vector>

namespace h7::net {

// One group of community content in the user's CookedPC folder.
struct ContentPack {
    std::string name;    // "mods/Conflux", "mods/H7Factions_extra", "Data", "Visuals", ...
    int files = 0;
    std::string version; // 8 hex digits: hash of the files' names, sizes and package headers
};

struct GameContent {
    std::string root;               // the folder that was scanned (UTF-8)
    std::vector<ContentPack> packs; // sorted by name; empty = unmodded game
    std::string fingerprint;        // 16 hex digits over all packs ("" when unmodded)
    std::string error;
};

// Blocking (reads the first 4 KB of every package): call from a worker thread.
// Scans <Documents>\My Games\Might & Magic Heroes VII\MMH7Game\CookedPC for *.upk and *.umap, where the game
// loads community content from. LocalShaderCache* files are machine-generated and skipped.
GameContent ScanGameContent();

} // namespace h7::net
