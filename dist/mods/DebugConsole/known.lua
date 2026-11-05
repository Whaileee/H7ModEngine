-- Entries of the game's console command list (Console.ManualAutoCompleteList) that Heroes VII v2.2.1 (the only
-- version the engine supports) answers with "Command not recognized": the shipping build has no stats system,
-- only the viewport's own Stat FPS/UNIT/LEVELS and the audio device's Stat SOUND*. Found with
-- tests/probecommands.lua on 2026-11-05. They are never suggested; commands that fail later are learned at runtime.
return {
    "Stat GAME", "Stat MEMORY", "Stat XBOXMEMORY", "Stat PHYSICS", "Stat STREAMING", "Stat STREAMINGDETAILS",
    "Stat COLLISION", "Stat PARTICLES", "Stat SCRIPT", "Stat AUDIO", "Stat ANIM", "Stat NET", "Stat LIST",
    "Stat splitscreen",
}
