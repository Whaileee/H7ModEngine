#pragma once

namespace h7 {

// Writes the files embedded in the DLL (default ini, ModManager) into the engine root, so a release needs only
// dinput8.dll. The ini is written only when missing; engine-owned files (ModManager) when missing or when this DLL
// carries a different version than the one last installed, so a newer DLL updates them but local edits survive.
// Call after the engine root exists.
void InstallBundledFiles();

} // namespace h7
