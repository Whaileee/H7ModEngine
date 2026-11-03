#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

namespace h7::mem {

struct ModuleRange {
    uintptr_t base = 0;
    uintptr_t end = 0;
    bool Contains(uintptr_t p) const { return p >= base && p < end; }
};

// The game executable's image range (resolved at startup).
extern ModuleRange g_game;

void InitGameModule();

// SEH-protected copy; returns false if any byte was unreadable.
bool Read(const void* src, void* dst, size_t n);
bool Write(void* dst, const void* src, size_t n);

template <class T>
inline bool Read(uintptr_t addr, T& out) { return Read(reinterpret_cast<const void*>(addr), &out, sizeof(T)); }

template <class T>
inline T ReadOr(uintptr_t addr, T fallback) {
    T v;
    return Read(addr, v) ? v : fallback;
}

// Calls fn(regionBase, regionSize) for committed, readable regions inside [lo, hi).
// If writableOnly is set, only regions with write access are reported.
void ForEachRegion(uintptr_t lo, uintptr_t hi, bool writableOnly,
                   const std::function<void(uintptr_t, size_t)>& fn);

// Start address of the function containing `pc`, using the image's .pdata unwind table.
uintptr_t FunctionStart(uintptr_t pc);

} // namespace h7::mem
