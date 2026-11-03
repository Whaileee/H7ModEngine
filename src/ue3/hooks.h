#pragma once
#include "core.h"
#include <functional>

namespace h7::hooks {

struct CallContext {
    ue3::UObject self;
    ue3::UObject function;
    uint8_t* locals;  // parameter/local storage for script functions; null for natives
    void* result;     // where the return value is written (may be null)
    bool native;      // function is implemented in C++ (params not readable)
    // Script hooks: the frame's FOutParmRec list {UProperty*, PropAddr, Next}. On script-to-script calls an out
    // parameter lives at the caller (PropAddr), not in locals.
    uint8_t* outParms = nullptr;
};

// Address of out parameter `prop` in the call, or null if it is not in the list.
uint8_t* OutParmAddr(const CallContext& ctx, ue3::UObject prop);

// pre returns true to skip the original (ignored for native functions, which must consume
// their parameters from the caller's bytecode).
using PreFn = std::function<bool(CallContext&)>;
using PostFn = std::function<void(CallContext&)>;

// Adds a listener to an engine function (no code patching; all listeners of a function share one hook).
// Script functions are hooked by replacing their bytecode with a trampoline, which sees every call; this must
// happen on the game thread. Native functions (and script functions with viaFunc) are hooked by swapping
// UFunction::Func to a per-function stub, which for script functions only sees calls made from C++.
// Returns a listener id (> 0), or 0 on failure.
int Add(ue3::UObject fn, PreFn pre, PostFn post, bool viaFunc = false);
void Remove(int listenerId);
// Re-applies hooks whose UFunction::Func was re-bound by the engine. Called every frame.
void Maintain();

// ProcessEvent: discovered automatically from the first hooked script event invoked by C++.
bool HasProcessEvent();
// Calls `fn` on `obj` with a params buffer laid out by the function's properties.
bool ProcessEvent(ue3::UObject obj, ue3::UObject fn, void* params);

} // namespace h7::hooks
