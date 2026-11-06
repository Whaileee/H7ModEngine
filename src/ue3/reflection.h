#pragma once
#include "core.h"
#include <string>
#include <vector>

namespace h7::ue3 {

// Property flags (UE3)
constexpr uint64_t CPF_OptionalParm = 0x10;
constexpr uint64_t CPF_Parm = 0x80;
constexpr uint64_t CPF_OutParm = 0x100;
constexpr uint64_t CPF_ReturnParm = 0x400;

// Function flags (UE3)
constexpr uint32_t FUNC_Final = 0x1;
constexpr uint32_t FUNC_Defined = 0x2;
constexpr uint32_t FUNC_Iterator = 0x4;
constexpr uint32_t FUNC_Latent = 0x8;
constexpr uint32_t FUNC_Native = 0x400;
constexpr uint32_t FUNC_Event = 0x800;
constexpr uint32_t FUNC_Static = 0x2000;

enum class PropKind {
    Unknown, Byte, Int, Bool, Float, Object, Class, Name, Str, Struct, Array, Delegate, Interface, Map, QWord, Component
};

struct PropInfo {
    UObject prop = 0;
    PropKind kind = PropKind::Unknown;
    std::string name;
    int32_t offset = 0;
    int32_t arrayDim = 1;
    int32_t elementSize = 0;
    uint64_t flags = 0;
};

// Run after script packages are loaded; fills Layout L. Returns false if a required offset is missing.
bool Calibrate();
void SetCalibrationQuiet(bool quiet); // while retrying: don't log each failed attempt
bool IsCalibrated();
// Run once the engine is fully initialised (functions bound): finds UObject::ProcessInternal.
bool CalibrateFunctionPointers();
uintptr_t ProcessInternal();

// UStruct / UField
UObject StructSuper(UObject s);
UObject StructChildren(UObject s);
UObject FieldNext(UObject f);
int32_t StructSize(UObject s);

// UProperty
PropKind KindOf(UObject prop);
PropInfo Describe(UObject prop);
UObject PropInner(UObject prop);        // Object/Class: PropertyClass, Struct: Struct, Array: Inner, Byte: Enum
uint32_t BoolMask(UObject prop);

// Finds a property/function by name in a struct and its supers (case-insensitive).
UObject FindField(UObject structObj, const std::string& name);
std::vector<UObject> Fields(UObject structObj, bool includeSupers);

// UFunction
uint32_t FuncFlags(UObject fn);
uint16_t FuncNativeIndex(UObject fn);
uint16_t FuncParmsSize(UObject fn);
uint16_t FuncReturnOffset(UObject fn);
uintptr_t* FuncNativePtr(UObject fn);   // address of UFunction::Func
std::vector<PropInfo> FuncParams(UObject fn); // CPF_Parm properties in order (incl. return)

// UClass
UObject ClassDefaultObject(UObject cls);

// UEnum
std::vector<std::string> EnumNames(UObject enumObj);

// FString helpers
std::string ReadFString(uintptr_t at);

// Engine allocator (GMalloc is backed by tbbmalloc in this build): use for any memory the engine may free.
void* EngineRealloc(void* p, size_t n);

} // namespace h7::ue3
