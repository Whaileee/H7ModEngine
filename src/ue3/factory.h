#pragma once
#include <string>
#include "core.h"

// Creating engine objects at runtime: UObject::StaticConstructObject, FName(TCHAR*, FNAME_Add) and
// GetTransientPackage, called directly. The game is final (v2.2.1), so their addresses are fixed RVAs; each is
// checked against its first bytes before use and the feature stays off if anything differs.
namespace h7::ue3 {

// UE3 object flags used by the factory.
constexpr uint64_t RF_Public = 0x0000000400000000ull;
constexpr uint64_t RF_Standalone = 0x0008000000000000ull;
constexpr uint64_t RF_Transient = 0x0000400000000000ull;
constexpr uint64_t RF_ArchetypeObject = 0x400ull;
constexpr uint64_t RF_RootSet = 0x4000ull;

bool InitFactory();       // verifies the function signatures; logs and returns false if they don't match
bool FactoryAvailable();

// Finds or adds a name in the engine name table (FNAME_Add). {-1,0} if the factory is unavailable.
FName MakeName(const std::string& name);

UObject TransientPackage();

// StaticConstructObject(cls, outer or transient package, name ("" = auto), flags, template). Properties are
// initialised from the template (or the class defaults). Returns 0 and sets `error` on failure.
UObject ConstructObject(UObject cls, UObject outer, const std::string& name, uint64_t flags, UObject tmpl,
                        std::string& error);

// A top-level object (no outer), e.g. a UPackage.
UObject ConstructTopLevel(UObject cls, const std::string& name, uint64_t flags, std::string& error);

} // namespace h7::ue3
