#pragma once
#include <string>

namespace h7::ue3 {

// Writes names.txt and objects.txt (an "SDK index" modders can grep).
void DumpNamesAndObjects(const std::wstring& dir);

// Raw hex snapshots of well-known engine objects, used to verify/calibrate layouts.
void DumpProbe(const std::wstring& dir);

// Writes the unpacked game image as a PE file (raw == virtual layout) for static analysis.
void DumpImage(const std::wstring& path);

} // namespace h7::ue3
