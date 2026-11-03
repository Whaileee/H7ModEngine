#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "layout.h"

namespace h7::ue3 {

struct TArrayRaw {
    uintptr_t data;
    int32_t num;
    int32_t max;
};

struct FName {
    int32_t index;
    int32_t number;
};

using UObject = uintptr_t; // engine objects are handled as raw addresses + Layout offsets

// --- discovery --------------------------------------------------------------
bool FindGNames();   // locate the global FName table
bool FindGObjects(); // locate the global object array (needs names)
uintptr_t GNamesAddress();
uintptr_t GObjectsAddress();

// --- names ------------------------------------------------------------------
int32_t NameCount();
std::string NameToString(int32_t index);          // raw entry, no number suffix
std::string NameToString(const FName& name);      // with _N suffix like UE3
int32_t FindNameIndex(const std::string& name);   // -1 if absent (cached lookup)

// --- objects ----------------------------------------------------------------
int32_t ObjectCount();
UObject ObjectAt(int32_t index);
UObject ObjClass(UObject o);
UObject ObjOuter(UObject o);
FName ObjFName(UObject o);
std::string ObjName(UObject o);
std::string ObjPathName(UObject o);   // Outer.Outer.Name
std::string ObjFullName(UObject o);   // "Class Outer.Name"
bool IsA(UObject o, UObject cls);
bool IsValidObject(UObject o);

// Lookup by "Class Package.Group.Name" or by path "Package.Group.Name".
UObject FindObject(const std::string& fullOrPath);
UObject FindClass(const std::string& path); // "Core.Object" or "Object"

void InvalidateLookupCache();

} // namespace h7::ue3
