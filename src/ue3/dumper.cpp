#include "dumper.h"
#include "core.h"
#include "../core/log.h"
#include "../core/memory.h"
#include <windows.h>
#include <cstdio>
#include <map>
#include <vector>

namespace h7::ue3 {

static FILE* OpenW(const std::wstring& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    return f;
}

void DumpNamesAndObjects(const std::wstring& dir) {
    if (FILE* f = OpenW(dir + L"\\names.txt")) {
        int32_t n = NameCount();
        for (int32_t i = 0; i < n; ++i) fprintf(f, "%d %s\n", i, NameToString(i).c_str());
        fclose(f);
        Log("Wrote %d names", n);
    }
    if (FILE* f = OpenW(dir + L"\\objects.txt")) {
        int32_t n = ObjectCount();
        for (int32_t i = 0; i < n; ++i) {
            UObject o = ObjectAt(i);
            if (o) fprintf(f, "%d %016llx %s\n", i, (unsigned long long)o, ObjFullName(o).c_str());
        }
        fclose(f);
        Log("Wrote %d objects", n);
    }
}

void DumpProbe(const std::wstring& dir) {
    FILE* f = OpenW(dir + L"\\probe.txt");
    if (!f) return;
    const char* targets[] = {
        "Class Core.Object", "Class Core.Field", "Class Core.Struct", "Class Core.State", "Class Core.Class",
        "Class Core.Function", "Class Core.Property", "Class Core.BoolProperty",
        "ObjectProperty Core.Object.Outer", "NameProperty Core.Object.Name", "ClassProperty Core.Object.Class",
        "IntProperty Core.Object.NetIndex", "QWordProperty Core.Object.ObjectFlags",
        "BoolProperty Engine.Actor.bHidden", "BoolProperty Engine.Actor.bStatic", "StructProperty Engine.Actor.Location",
        "ArrayProperty Engine.Actor.Attached", "ByteProperty Engine.Actor.Physics", "ScriptStruct Core.Object.Vector",
        "Enum Engine.Actor.EPhysics", "Function Engine.Actor.Touch", "Function Engine.GameViewportClient.Tick",
        "Function Engine.PlayerController.ConsoleCommand", "Function Core.Object.Sin",
        "Function Engine.Actor.SetLocation", "Class MMH7Game.H7Creature", "Class MMH7Game.H7CombatController",
        "Object Core.Default__Object",
    };
    for (const char* t : targets) {
        UObject o = FindObject(t);
        fprintf(f, "=== %s -> %016llx\n", t, (unsigned long long)o);
        if (!o) continue;
        uint8_t buf[0x140];
        if (mem::Read((void*)o, buf, sizeof(buf))) fputs(HexDump(buf, sizeof(buf), o).c_str(), f);
        // Annotate pointer-sized fields that point at known objects.
        for (int off = 0; off < (int)sizeof(buf); off += 8) {
            uintptr_t v = *reinterpret_cast<uintptr_t*>(buf + off);
            if (IsValidObject(v)) fprintf(f, "    +%03x -> %s\n", off, ObjFullName(v).c_str());
            else if (mem::g_game.Contains(v)) fprintf(f, "    +%03x -> game+%llx\n", off, (unsigned long long)(v - mem::g_game.base));
        }
    }

    // UFunction::Func discovery: the most common code pointer across script functions is ProcessInternal.
    std::map<int, std::map<uintptr_t, int>> hist;
    int functions = 0;
    int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        UObject o = ObjectAt(i);
        if (!o || ObjName(ObjClass(o)) != "Function") continue;
        ++functions;
        uint8_t buf[0x140];
        if (!mem::Read((void*)o, buf, sizeof(buf))) continue;
        for (int off = L.objSize; off < (int)sizeof(buf); off += 8) {
            uintptr_t v = *reinterpret_cast<uintptr_t*>(buf + off);
            if (mem::g_game.Contains(v)) hist[off][v]++;
        }
    }
    fprintf(f, "\n=== code-pointer fields across %d UFunctions\n", functions);
    for (auto& [off, vals] : hist) {
        int total = 0, best = 0;
        uintptr_t bestV = 0;
        for (auto& [v, c] : vals) {
            total += c;
            if (c > best) best = c, bestV = v;
        }
        fprintf(f, "  +%03x: %d functions, %zu distinct, top game+%llx x%d (fn start game+%llx)\n", off, total,
                vals.size(), (unsigned long long)(bestV - mem::g_game.base), best,
                (unsigned long long)(mem::FunctionStart(bestV) ? mem::FunctionStart(bestV) - mem::g_game.base : 0));
    }

    // UObject vtable (from the Object CDO) with function starts, for ProcessEvent identification.
    UObject cdo = FindObject("Object Core.Default__Object");
    if (!cdo) cdo = ObjectAt(0);
    uintptr_t vt = mem::ReadOr<uintptr_t>(cdo + L.objVTable, 0);
    fprintf(f, "\n=== UObject vtable @ game+%llx\n", (unsigned long long)(vt - mem::g_game.base));
    for (int i = 0; i < 128; ++i) {
        uintptr_t fn = mem::ReadOr<uintptr_t>(vt + i * 8, 0);
        if (!mem::g_game.Contains(fn)) break;
        fprintf(f, "  [%3d] game+%llx\n", i, (unsigned long long)(fn - mem::g_game.base));
    }
    fclose(f);
    Log("Wrote probe.txt");
}

void DumpImage(const std::wstring& path) {
    size_t size = mem::g_game.end - mem::g_game.base;
    std::vector<uint8_t> img(size, 0);
    size_t unreadable = 0;
    for (size_t off = 0; off < size; off += 0x1000) {
        if (!mem::Read((void*)(mem::g_game.base + off), img.data() + off, 0x1000)) ++unreadable;
    }
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.data());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img.data() + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        sec[i].PointerToRawData = sec[i].VirtualAddress;
        sec[i].SizeOfRawData = sec[i].Misc.VirtualSize;
        // The first section holds the unpacked code but is flagged as data; mark it executable.
        if (i == 0) sec[i].Characteristics |= IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE;
    }
    nt->OptionalHeader.ImageBase = mem::g_game.base;
    if (FILE* f = OpenW(path)) {
        fwrite(img.data(), 1, img.size(), f);
        fclose(f);
        Log("Dumped image (%zu bytes, %zu unreadable pages)", size, unreadable);
    }
}

} // namespace h7::ue3
