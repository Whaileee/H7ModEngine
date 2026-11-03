#include "hooks.h"
#include "reflection.h"
#include "../core/log.h"
#include "../core/memory.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace h7::hooks {

using namespace ue3;

struct Listener {
    int id;
    PreFn pre;
    PostFn post;
    bool removed = false; // set by Remove(); a dispatch already in flight skips it
};

struct HookEntry {
    UObject function;
    bool native;
    bool script;           // hooked through a bytecode trampoline (see "script hooks"), else through Func
    uintptr_t original;    // Func hooks: original UFunction::Func
    uint8_t* stub;         // Func hooks: per-function stub
    TArrayRaw origScript;  // script hooks: the function's own bytecode
    TArrayRaw trampScript; // script hooks: replacement bytecode
    uint8_t* shadow;       // script hooks: copy of the UFunction that keeps origScript
    int bodyStart = 0;     // script hooks: offset of the body in origScript (after the default-parm blocks)
    std::vector<std::shared_ptr<Listener>> listeners;
};

static std::recursive_mutex g_mutex;
static std::unordered_map<UObject, HookEntry*> g_hooks;
static std::unordered_map<int, UObject> g_listenerFn;
static int g_nextListener = 1;
static int g_frameNode = -1; // FFrame offset of Node (Object follows, then Code, then Locals)

// --- per-function stubs ----------------------------------------------------------
// mov r9, imm64(entry) ; mov rax, imm64(Handler) ; jmp rax
// Func is a UObject member function (this, FFrame&, Result) so r9 is free for the entry pointer.
static void __fastcall Handler(UObject self, uint8_t* stack, void* result, HookEntry* entry);

static uint8_t* AllocStub(HookEntry* entry) {
    static uint8_t* page = nullptr;
    static size_t used = 0;
    const size_t kStub = 32;
    if (!page || used + kStub > 0x10000) {
        page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        used = 0;
        if (!page) return nullptr;
    }
    uint8_t* s = page + used;
    used += kStub;
    uintptr_t e = reinterpret_cast<uintptr_t>(entry), h = reinterpret_cast<uintptr_t>(&Handler);
    s[0] = 0x49; s[1] = 0xB9; memcpy(s + 2, &e, 8);   // mov r9, entry
    s[10] = 0x48; s[11] = 0xB8; memcpy(s + 12, &h, 8); // mov rax, Handler
    s[20] = 0xFF; s[21] = 0xE0;                       // jmp rax
    FlushInstructionCache(GetCurrentProcess(), s, kStub);
    return s;
}

// --- ProcessEvent discovery --------------------------------------------------------
// A script event invoked from C++ runs as ProcessEvent -> (this->*Func)(...). The return address of
// our stub therefore lies inside ProcessEvent, which must also be a slot in the object's vtable.

static int g_peCandidate = -1;
static int g_peVotes = 0;

static void ObserveCaller(UObject self, uintptr_t retAddr) {
    if (L.ueProcessEventIndex >= 0) return;
    uintptr_t caller = mem::FunctionStart(retAddr);
    if (!caller) return;
    uintptr_t vt = mem::ReadOr<uintptr_t>(self, 0);
    for (int i = 0; i < 200; ++i) {
        uintptr_t f = mem::ReadOr<uintptr_t>(vt + i * 8, 0);
        if (!mem::g_game.Contains(f)) break;
        if (f != caller) continue;
        g_peVotes = g_peCandidate == i ? g_peVotes + 1 : 1;
        g_peCandidate = i;
        if (g_peVotes >= 3) {
            L.ueProcessEventIndex = i;
            Log("ProcessEvent = vtable[%d] (game+%llx)", i, (unsigned long long)(f - mem::g_game.base));
        }
        return;
    }
}

bool HasProcessEvent() { return L.ueProcessEventIndex >= 0; }

bool ProcessEvent(UObject obj, UObject fn, void* params) {
    if (L.ueProcessEventIndex < 0 || !obj || !fn) return false;
    using PE = void(__fastcall*)(UObject, UObject, void*, void*);
    uintptr_t vt = mem::ReadOr<uintptr_t>(obj, 0);
    auto pe = reinterpret_cast<PE>(mem::ReadOr<uintptr_t>(vt + L.ueProcessEventIndex * 8, 0));
    if (!pe) return false;
    // ProcessEvent rejects functions that have a native opcode index; clear it for the call.
    auto* iNative = reinterpret_cast<uint16_t*>(fn + L.funcFlags + 4);
    uint16_t saved = *iNative;
    *iNative = 0;
    pe(obj, fn, params, nullptr);
    *iNative = saved;
    return true;
}

// --- handler -----------------------------------------------------------------------

// FFrame: FOutputDevice header, then Node, Object, Code, Locals (8 bytes each). Structs are packed to 4,
// so Node's offset is found by matching the function and object being executed.
template <class IsNode>
static bool DiscoverFrame(uint8_t* stack, UObject self, IsNode isNode) {
    if (g_frameNode >= 0) return true;
    auto at = [&](int off) { return mem::ReadOr<uintptr_t>((uintptr_t)stack + off, 0); };
    for (int off = 8; off <= 0x40; off += 4) {
        if (isNode(at(off)) && at(off + 8) == self) {
            g_frameNode = off;
            Log("FFrame layout: Node=+%x Object=+%x Code=+%x Locals=+%x", off, off + 8, off + 16, off + 24);
            return true;
        }
    }
    return false;
}

static uint8_t* FrameLocals(uint8_t* stack, UObject self, UObject fn) {
    if (!DiscoverFrame(stack, self, [&](uintptr_t v) { return v == fn; })) return nullptr;
    if (mem::ReadOr<uintptr_t>((uintptr_t)stack + g_frameNode, 0) != fn) return nullptr;
    return reinterpret_cast<uint8_t*>(mem::ReadOr<uintptr_t>((uintptr_t)stack + g_frameNode + 24, 0));
}

static void ClearReturnValue(UObject fn, void* result) {
    if (!result) return;
    for (auto& p : FuncParams(fn))
        if (p.flags & CPF_ReturnParm) memset(result, 0, p.elementSize);
}

static void __fastcall Handler(UObject self, uint8_t* stack, void* result, HookEntry* e) {
    using Orig = void(__fastcall*)(UObject, uint8_t*, void*);
    if (!e->native) ObserveCaller(self, reinterpret_cast<uintptr_t>(_ReturnAddress()));

    std::vector<std::shared_ptr<Listener>> listeners;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        listeners = e->listeners;
    }
    if (listeners.empty()) {
        reinterpret_cast<Orig>(e->original)(self, stack, result);
        return;
    }
    CallContext ctx{self, e->function, e->native ? nullptr : FrameLocals(stack, self, e->function), result, e->native};
    bool block = false;
    for (auto& l : listeners)
        if (l->pre && !l->removed && l->pre(ctx)) block = true;
    if (!block || e->native) reinterpret_cast<Orig>(e->original)(self, stack, result);
    for (auto& l : listeners)
        if (l->post && !l->removed) l->post(ctx);
}

// --- script hooks (bytecode trampoline) ------------------------------------------------
// Script-to-script calls run the callee through ProcessInternal directly, never through UFunction::Func, so a
// Func hook only sees calls made from C++ (ProcessEvent). A script hook instead replaces the function's bytecode
// with
//     EX_Return EX_FinalFunction(g_hookNative) EX_EndFunctionParms
// By the time that runs the engine has parsed the call's arguments into the frame, for script and C++ callers
// alike. g_hookNative is a fake native UFunction (never registered in the object table) whose Func is
// ScriptHookNative: it runs the listeners and executes the original bytecode on a copy of the frame whose Node
// is a shadow copy of the UFunction that still owns the original Script (jump opcodes index Node->Script).
// The return value is written straight into the caller's result buffer.
// (ProcessInternal steps statements until EX_Return, then evaluates one expression into the result, so the
// leading EX_Return makes our call that return expression.)
// A function with optional parameters starts with one entry per optional parameter: an EX_DefaultParmValue block
//     49 <u16 length> <default expression> 15
// or, for a parameter without a default, a lone EX_Nothing (0B). The engine steps one entry per optional
// parameter itself while binding the arguments, before the body. The trampoline keeps those blocks
// in front, and the original body is entered right after them.

constexpr uint8_t EX_Return = 0x04, EX_Nothing = 0x0B, EX_EndFunctionParms = 0x16, EX_FinalFunction = 0x1C,
                  EX_DefaultParmValue = 0x49, EX_EndOfScript = 0x53;
constexpr int kFrameCopy = 0x80;     // covers FFrame (Node at +0x14 .. OutParms) with room to spare
constexpr int kFrameOutParms = 0x28; // FFrame::OutParms relative to Node (EX_LocalOutVariable reads +0x3C)

uint8_t* OutParmAddr(const CallContext& ctx, UObject prop) {
    for (uint8_t* rec = ctx.outParms; rec; rec = *reinterpret_cast<uint8_t**>(rec + 16))
        if (*reinterpret_cast<UObject*>(rec) == prop) return *reinterpret_cast<uint8_t**>(rec + 8);
    return nullptr;
}

static uint8_t* g_hookNative = nullptr;

static void __fastcall ScriptHookNative(UObject self, uint8_t* stack, void* result) {
    if (!DiscoverFrame(stack, self, [](uintptr_t v) {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            auto it = g_hooks.find(v);
            return it != g_hooks.end() && it->second->script;
        })) {
        Log("script hook: FFrame layout unknown; call skipped");
        return;
    }
    auto field = [&](int off) -> uintptr_t& { return *reinterpret_cast<uintptr_t*>(stack + g_frameNode + off); };
    UObject fn = field(0);
    uint8_t*& code = reinterpret_cast<uint8_t*&>(field(16));
    if (*code == EX_EndFunctionParms) ++code; // P_FINISH: we take no parameters of our own

    HookEntry* e = nullptr;
    std::vector<std::shared_ptr<Listener>> listeners;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        auto it = g_hooks.find(fn);
        if (it == g_hooks.end()) return;
        e = it->second;
        listeners = e->listeners;
    }
    CallContext ctx{self, fn, reinterpret_cast<uint8_t*>(field(24)), result, false,
                    reinterpret_cast<uint8_t*>(field(kFrameOutParms))};
    bool block = false;
    for (auto& l : listeners)
        if (l->pre && !l->removed && l->pre(ctx)) block = true;
    if (!block) {
        alignas(16) uint8_t frame[kFrameCopy];
        memcpy(frame, stack, kFrameCopy);
        *reinterpret_cast<uintptr_t*>(frame + g_frameNode) = reinterpret_cast<uintptr_t>(e->shadow);
        *reinterpret_cast<uintptr_t*>(frame + g_frameNode + 16) = e->origScript.data + e->bodyStart;
        using Fn = void(__fastcall*)(UObject, uint8_t*, void*);
        reinterpret_cast<Fn>(ProcessInternal())(self, frame, result);
    } else {
        ClearReturnValue(fn, result);
    }
    for (auto& l : listeners)
        if (l->post && !l->removed) l->post(ctx);
}

static bool InitHookNative() {
    if (g_hookNative) return true;
    if (L.structScript < 0 || L.funcSize <= 0 || !ProcessInternal()) return false;
    UObject model = FindObject("Function Core.Object.Sin");
    if (!model) return false;
    auto* f = new uint8_t[L.funcSize];
    memcpy(f, reinterpret_cast<void*>(model), L.funcSize);
    *reinterpret_cast<uint32_t*>(f + L.funcFlags) = FUNC_Final | FUNC_Native;
    *reinterpret_cast<uint16_t*>(f + L.funcFlags + 4) = 0; // iNative
    *reinterpret_cast<uintptr_t*>(f + L.funcNative) = reinterpret_cast<uintptr_t>(&ScriptHookNative);
    g_hookNative = f;
    return true;
}

// Length of the EX_DefaultParmValue blocks at the start of a function's script, or -1 if they don't parse.
static int DefaultParmBlocks(UObject fn, const TArrayRaw& script) {
    int optional = 0;
    for (const PropInfo& p : FuncParams(fn))
        if (p.flags & CPF_OptionalParm) ++optional;
    const uint8_t* s = reinterpret_cast<const uint8_t*>(script.data);
    // A parameter without a default (optional int X) has a lone EX_Nothing instead of a block.
    int pos = 0;
    for (int i = 0; i < optional; ++i) {
        if (pos < script.num && s[pos] == EX_Nothing) {
            ++pos;
        } else if (pos + 3 <= script.num && s[pos] == EX_DefaultParmValue) {
            pos += 3 + *reinterpret_cast<const uint16_t*>(s + pos + 1);
        } else {
            return -1;
        }
    }
    return pos < script.num ? pos : -1;
}

static bool InitScriptHook(HookEntry* e) {
    if (!mem::Read(e->function + L.structScript, e->origScript) || e->origScript.num <= 0) return false;
    e->bodyStart = DefaultParmBlocks(e->function, e->origScript);
    if (e->bodyStart < 0) {
        Log("script hook: cannot parse the default parameter values of %s", ObjPathName(e->function).c_str());
        return false;
    }
    e->shadow = new uint8_t[L.funcSize];
    memcpy(e->shadow, reinterpret_cast<void*>(e->function), L.funcSize);
    uint8_t call[] = {EX_Return, EX_FinalFunction, 0, 0, 0, 0, 0, 0, 0, 0, EX_EndFunctionParms,
                      EX_Return, EX_Nothing, EX_EndOfScript};
    uintptr_t native = reinterpret_cast<uintptr_t>(g_hookNative);
    memcpy(call + 2, &native, 8);
    int size = e->bodyStart + (int)sizeof(call);
    auto* data = static_cast<uint8_t*>(EngineRealloc(nullptr, size)); // the engine frees Script with its allocator
    if (!data) return false;
    memcpy(data, reinterpret_cast<void*>(e->origScript.data), e->bodyStart);
    memcpy(data + e->bodyStart, call, sizeof(call));
    e->trampScript = {reinterpret_cast<uintptr_t>(data), size, size};
    return true;
}

// Script hooks are only installed/removed from Lua, i.e. on the game thread between script calls.
static void SetScript(HookEntry* e, bool hooked) {
    const TArrayRaw& s = hooked ? e->trampScript : e->origScript;
    mem::Write(reinterpret_cast<void*>(e->function + L.structScript), &s, sizeof(s));
}

// --- listeners -----------------------------------------------------------------------

int Add(UObject fn, PreFn pre, PostFn post, bool viaFunc) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    uintptr_t* slot = FuncNativePtr(fn);
    if (!slot || !IsValidObject(fn)) return 0;
    HookEntry*& e = g_hooks[fn];
    if (!e) {
        uint32_t flags = FuncFlags(fn);
        bool native = (flags & FUNC_Native) != 0;
        e = new HookEntry{fn, native};
        e->script = !native && !viaFunc && !(flags & (FUNC_Latent | FUNC_Iterator)) && InitHookNative() &&
                    InitScriptHook(e);
        if (!e->script) {
            uintptr_t original = *slot;
            // Script functions are bound to ProcessInternal lazily; an unbound one can use it directly.
            if (!original && !native) original = ProcessInternal();
            e->original = original;
            e->stub = original ? AllocStub(e) : nullptr;
            if (!e->stub) {
                Log("cannot hook %s: function is not bound yet", ObjPathName(fn).c_str());
                delete e;
                g_hooks.erase(fn);
                return 0;
            }
            if (!native && !viaFunc)
                Log("warning: %s is hooked through Func; only calls from C++ will be seen", ObjPathName(fn).c_str());
        }
        // Probe events (Tick, Touch, ...) declared without a body are filtered out by ProcessEvent.
        if (!native && (flags & FUNC_Event) && !(flags & FUNC_Defined))
            Log("warning: %s has no script body; if it is a probe event the engine never calls it",
                ObjPathName(fn).c_str());
    }
    int id = g_nextListener++;
    e->listeners.push_back(std::make_shared<Listener>(Listener{id, std::move(pre), std::move(post)}));
    g_listenerFn[id] = fn;
    if (e->script)
        SetScript(e, true);
    else // Aligned 8-byte store: safe even if the game thread is calling this function right now.
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), reinterpret_cast<LONG64>(e->stub));
    return id;
}

void Maintain() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& [fn, e] : g_hooks) {
        if (e->listeners.empty() || e->script) continue;
        uintptr_t* slot = FuncNativePtr(fn);
        uintptr_t cur = *slot;
        if (cur == reinterpret_cast<uintptr_t>(e->stub)) continue;
        // The engine (re)bound this function after we hooked it; adopt the new target and re-hook.
        if (cur) e->original = cur;
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), reinterpret_cast<LONG64>(e->stub));
        Log("re-applied hook on %s", ObjPathName(fn).c_str());
    }
}

void Remove(int id) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_listenerFn.find(id);
    if (it == g_listenerFn.end()) return;
    HookEntry* e = g_hooks[it->second];
    g_listenerFn.erase(it);
    auto& ls = e->listeners;
    ls.erase(std::remove_if(ls.begin(), ls.end(), [&](auto& l) {
                 if (l->id != id) return false;
                 l->removed = true;
                 return true;
             }),
             ls.end());
    if (ls.empty()) {
        // Restore the original; the entry, stub and shadow stay allocated in case a call is in flight.
        if (e->script)
            SetScript(e, false);
        else
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(FuncNativePtr(e->function)), (LONG64)e->original);
    }
}

} // namespace h7::hooks
