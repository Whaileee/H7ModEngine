#pragma once
#include <cstdint>

// Memory layout of the engine's UE3 core types (64-bit, package version 868).
// UObject offsets come from the game's own Object.uc (native var order, x64 alignment)
// and are verified at runtime. Offsets marked "calibrated" are discovered at startup.
namespace h7::ue3 {

struct Layout {
    // FNameEntry
    int nameEntryIndex = -1;     // calibrated: int holding (index << 1) | bUnicode
    int nameEntryString = -1;    // calibrated: start of the inline name characters

    // UObject
    int objVTable = 0x00;
    int objFlags = 0x10;
    int objInternalIndex = 0x38; // == index into GObjects
    int objOuter = 0x40;
    int objName = 0x48;          // FName { int Index; int Number; }
    int objClass = 0x50;
    int objArchetype = 0x58;
    int objSize = 0x60;

    // UField
    int fieldNext = 0x60;

    // UStruct / UFunction / UProperty: calibrated (see Calibrate*), -1 until known
    int structSuper = -1;
    int structChildren = -1;
    int structPropertySize = -1;
    int structScript = -1;       // TArray<BYTE> bytecode (4-byte packed, so not 8-aligned)

    int funcFlags = -1;
    int funcNative = -1;         // UFunction::Func (pointer-to-member, 8 bytes on x64 single inheritance)
    int funcSize = -1;           // sizeof(UFunction)
    int funcParmsSize = -1;
    int funcReturnOffset = -1;

    int propArrayDim = -1;
    int propElementSize = -1;
    int propFlags = -1;          // uint64
    int propOffset = -1;

    int boolBitMask = -1;        // UBoolProperty
    int objPropClass = -1;       // UObjectProperty::PropertyClass
    int structPropStruct = -1;   // UStructProperty::Struct
    int arrayPropInner = -1;     // UArrayProperty::Inner
    int bytePropEnum = -1;       // UByteProperty::Enum

    int ueProcessEventIndex = -1; // UObject vtable slot of ProcessEvent
};

extern Layout L;

} // namespace h7::ue3
