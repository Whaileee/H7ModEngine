#include "memory.h"
#include <windows.h>
#include <cstring>

namespace h7::mem {

ModuleRange g_game;

void InitGameModule() {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_game.base = base;
    g_game.end = base + nt->OptionalHeader.SizeOfImage;
}

bool Read(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Write(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ForEachRegion(uintptr_t lo, uintptr_t hi, bool writableOnly,
                   const std::function<void(uintptr_t, size_t)>& fn) {
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    uintptr_t p = lo;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < hi && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
        uintptr_t rbase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t rend = rbase + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & readable) && (!writableOnly || (mbi.Protect & writable))) {
            uintptr_t s = rbase < lo ? lo : rbase;
            uintptr_t e = rend > hi ? hi : rend;
            if (e > s) fn(s, e - s);
        }
        p = rend;
    }
}

uintptr_t FunctionStart(uintptr_t pc) {
    DWORD64 imageBase = 0;
    const RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
    if (!rf) return 0;
    // Chained unwind info points back to the primary entry; follow it to the real start.
    auto info = reinterpret_cast<const uint8_t*>(imageBase + rf->UnwindData);
    while (info[0] >> 3 & UNW_FLAG_CHAININFO) {
        uint8_t codes = info[2];
        auto chained = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + ((codes + 1) & ~1) * 2);
        rf = chained;
        info = reinterpret_cast<const uint8_t*>(imageBase + rf->UnwindData);
    }
    return imageBase + rf->BeginAddress;
}

} // namespace h7::mem
