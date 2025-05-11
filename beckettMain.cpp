#include <iostream>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>
#include <thread>
#include <vector>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <sys/types.h>
#include <cassert>

// ─── Luau Compiler ─────────────────────────────────────────────────────────────
#include "Luau/Compiler.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/BytecodeUtils.h"
//#include "lualib.h"
// ─── Addresses                                   ────────────────────────────────────────────────────────
static constexpr uint64_t PUSHLSTRING_ADDR     = 0x1026f30da;
static constexpr uint64_t LUAULOAD_ADDR     = 0x102714007; //bytecode version mismatch is the string and the func is thewhole thing
static constexpr uint64_t GETSTATE_ADDR        = 0x100dab1ee;
static constexpr uint64_t SPAWN_ADDR           = 0x100d054a8;
static constexpr uint64_t SETTOP_ADDR          = 0x1026f2347;
static constexpr uint64_t TOSTRING_ADDR        = 0x1026f2a62;
static constexpr uint64_t PRINT_ADDR           = 0x1001a3a32; // current identity is %d (the function it is in)
static constexpr uint64_t GETTASKSCHEDULER_ADDR= 0x1035e396b; // WRONG, DO NOT USE
static constexpr uint64_t RAWGETI_ADDR = 0x1026f36db;
static constexpr uint64_t TASKSCHEDULER_ADDR   = 0x10579bb10; //this ones kinda easy to find just check what is pushed into the func where you find taskschduler job start and end and its whatever arg those are added to and then find the func where they send the arc and it should be lke data_blablabla
static constexpr uint64_t NEWTHREAD_ADDR       = 0x1026f2249;
static constexpr uint64_t PUSHVALUE_ADDR = 0x1026f2543;
static constexpr uint64_t PUSHCCLOSURE_ADDR = 0x1026f32d7;
static constexpr uint64_t SETFIELD_ADDR = 0x1026f3a9e;
static constexpr uintptr_t LUAOPENBASELIBS_ADDR = 0x1026f5cd3; //the function with ipairs and _version and _g  and it pushes all those base functiosn from a table
static constexpr uintptr_t GETFIELD_ADDR = 0x1026f34d7;
static constexpr uintptr_t ISNIL_ADDR = 0x1026f25c1; // same as pushnil almost
static constexpr uintptr_t CREATETABLE_ADDR = 0x1026f3774;
static constexpr uintptr_t GETMETATABLE_ADDR = 0x1026f38cd;
static constexpr uintptr_t PUSHNIL_ADDR = 0x1026f3041; //pretty hard to find but near the yday string just look in luau source and then compare it
// ─── Signatures ────────────────────────────────────────────────────────────────
using tolstring_t     = const char* (*)(uint64_t L, int idx, size_t* len);
using luauload_t   = int64_t (*)(uint64_t L, const char* name, const char* bc, size_t len, int env);
using settop_t        = int64_t (*)(uint64_t L, int idx);
using spawn_t         = int64_t (*)(uint64_t L);
using print_t         = void (*)(uint8_t level, const char* fmt, ...);
using getstate_t      = uintptr_t (*)(uintptr_t script_context, const uint32_t* identity, const uintptr_t* script);
using getscheduler_t  = uintptr_t* (*)();
using newthread_t     = uint64_t (*)(uint64_t rl);
using rawgeti_t = int64_t (*)(uint64_t L, int registryIndex, int key);
using pushvalue_t = void* (*)(uint64_t rl, int idx);
using pushcclosure_t = void (*)(uint64_t rl, int (*)(uint64_t), const char* debugname, int nup, uint64_t cont);
using setfield_t = void (*)(uint64_t rl, int idx, const char* key);
using luaopen_base_t = int64_t (*)(void* L);
using getfield_t = uint64_t (*)(uint64_t rl, int idx, const char* name);
using isnil_t = int (*)(uint64_t L, int idx);
using createtable_t = void (*)(uint64_t L, int narr, int nrec);
using getmetatable_t = int    (*)(uint64_t L, int objindex);
using pushnil_t      = void   (*)(uint64_t L);
// ─── Globals ───────────────────────────────────────────────────────────────────
static tolstring_t    rbx_tolstring     = nullptr;
static luauload_t  rbx_luauload   = nullptr;
static settop_t       rbx_settop        = nullptr;
static spawn_t        rbx_spawn         = nullptr;
static print_t        rbx_print         = nullptr;
static getscheduler_t rbx_getscheduler  = nullptr;
static getstate_t     rbx_getstate      = nullptr;
static newthread_t    rbx_newthread     = nullptr;
static rawgeti_t rbx_rawgeti = nullptr;
static settop_t       rbx_settop_orig   = nullptr;
static pushvalue_t rbx_pushvalue = nullptr;
static pushcclosure_t rbx_pushcclosure = nullptr;
static setfield_t rbx_setfield = nullptr;
static luaopen_base_t rbx_luaopen_base = nullptr;
static getfield_t rbx_getfield = nullptr;
static isnil_t rbx_isnil = nullptr;
static createtable_t rbx_createtable = nullptr;
static getmetatable_t rbx_getmetatable = nullptr;
static pushnil_t      rbx_pushnil      = nullptr;
#define LUA_GLOBALSINDEX (-10002)
#define LUA_REGISTRYINDEX (-10000)
// ─── Hardcoded Luau Script ─────────────────────────────────────────────────────


static uint64_t g_cachedThread = 0;
static uintptr_t baseL = 0;
static uintptr_t g_cachedBaseL = 0;
// ─── Scheduler Utilities ───────────────────────────────────────────────────────
namespace scheduler {
    struct job {
        void** vtable;
        std::string job_name;
    };
    std::vector<job*> scheduler_jobs;
}

// ─── Luau Compilation ──────────────────────────────────────────────────────────
std::string compile_luau_to_bytecode(const std::string& source) {
    Luau::CompileOptions o;
    o.debugLevel = 1;
    o.optimizationLevel = 1;
    return Luau::compile(source, o);
}

struct MyEncoder final : Luau::BytecodeEncoder {
    void encode(uint32_t* data, size_t count) override {
        for (size_t i = 0; i < count;) {
            uint8_t& op = *reinterpret_cast<uint8_t*>(data + i);
            i += Luau::getOpLength(LuauOpcode(op));
            op = static_cast<uint8_t>(op * 227);
        }
    }
};

std::string compileWithCustomEncoder(const std::string& src) {
    Luau::CompileOptions opts;
    opts.debugLevel = 1;
    opts.optimizationLevel = 1;
    static MyEncoder encoder;
    return Luau::compile(src, opts, {}, &encoder);
}

// ─── Roblox Identity Helpers (stolen) ───────────────────────────────────────────────────
__int128* rbx_getidentity(uint64_t rl) {
    return (__int128*)(*(uint64_t*)(rl + 0x78) + 0x30);
}

void rbx_setidentity(uint64_t rl, __int128 identity) {
    *rbx_getidentity(rl) = identity;
}

// ─── Script Context Scanner ────────────────────────────────────────────────────
uintptr_t get_script_context() {
    scheduler::scheduler_jobs.clear();
    uintptr_t slide = _dyld_get_image_vmaddr_slide(0);
    uintptr_t scheduler_ref = slide + TASKSCHEDULER_ADDR;

    uintptr_t jobs_start = *reinterpret_cast<uintptr_t*>(scheduler_ref + 0x1f0);
    uintptr_t jobs_end   = *reinterpret_cast<uintptr_t*>(scheduler_ref + 0x1f8);

    const ptrdiff_t stride = sizeof(uint64_t);

    rbx_print(0, "[DBG] scanning jobs: start=0x%llx, end=0x%llx", jobs_start, jobs_end);

    while (jobs_start < jobs_end) {
        auto job = *reinterpret_cast<scheduler::job**>(jobs_start);
        scheduler::scheduler_jobs.push_back(job);

        uint8_t smallFlag = *reinterpret_cast<uint8_t*>((char*)job + 0x18);
        const char* namePtr = (smallFlag & 1)
            ? *reinterpret_cast<const char**>((char*)job + 0x28)
            : (const char*)job + 0x19;

        rbx_print(1, "[DBG] job: %s (namePtr=0x%llx) at 0x%llx", namePtr, (uint64_t)namePtr, jobs_start);

        if (strcmp(namePtr, "WaitingHybridScriptsJob") == 0) {
            uint64_t script_context = *reinterpret_cast<uint64_t*>((char*)job + 0x210);
            const uint32_t identity = 8;
                    const uintptr_t  dummy   = 0;
                    uintptr_t thread = rbx_getstate(script_context, &identity, &dummy);

                    rbx_print(2, "[DBG] candidate_ctx=0x%llx → thread=0x%llx", script_context, thread);

                    if (thread != 0) {
                        
                        return script_context;
                    }
        
        }

        jobs_start += stride;
    }

    return 0;
}

void rbx_register(uint64_t rl, int(* function)(uint64_t), const char* name) {
    (*rbx_pushcclosure)(rl, function, "beckett", 0, 0);
    (*rbx_setfield)(rl, LUA_GLOBALSINDEX, name);
}

void prepare_base_lib(uintptr_t state)
{
    //this function can be found with the string ipairs and _VERSION and lke _G in the source code it basically pushes the base c functions so i just use it cause its easier
    rbx_setidentity(state, 8);

    rbx_luaopen_base(reinterpret_cast<void*>(state));

    rbx_settop(state, 1);
}


// ─── Script Execution ──────────────────────────────────────────────────────────
bool execute_script(const std::string& src) {
    std::string bytecode = compileWithCustomEncoder(src);
    const uint32_t identity = 8;
    const uintptr_t script = 0;
    rbx_print(0, "Execution started, execute_script()");
    if(!g_cachedBaseL) {
        baseL = rbx_getstate(get_script_context(), &identity, &script);
        rbx_setidentity(baseL, 8);
        g_cachedBaseL = rbx_newthread(baseL);
    }
    uint64_t L = g_cachedBaseL;
    rbx_setidentity(L, 8);
    prepare_base_lib(L);
    if (rbx_luauload(L, "@beckett", bytecode.data(), static_cast<int>(bytecode.size()), 0) != 0) {
        size_t errlen = 0;
        const char* err = rbx_tolstring(L, -1, &errlen);
        rbx_print(3, "%.*s", (int)errlen, err ? err : "load failed");
        rbx_settop(L, 0);
        return false;
    }
    
    rbx_setidentity(L, 8);

    
    rbx_spawn(L);
    rbx_settop(L, 0);
    return true;
}

// ─── Entry Point ───────────────────────────────────────────────────────────────
__attribute__((constructor))
void c_main() {
    std::thread([] {
        uintptr_t slide = _dyld_get_image_vmaddr_slide(0);
        //where functions get initialized
        rbx_print        = reinterpret_cast<print_t>(slide + PRINT_ADDR);
        rbx_tolstring    = reinterpret_cast<tolstring_t>(slide + TOSTRING_ADDR);
        rbx_luauload  = reinterpret_cast<luauload_t>(slide + LUAULOAD_ADDR);
        rbx_settop       = reinterpret_cast<settop_t>(slide + SETTOP_ADDR);
        rbx_spawn        = reinterpret_cast<spawn_t>(slide + SPAWN_ADDR);
        rbx_getscheduler = reinterpret_cast<getscheduler_t>(slide + GETTASKSCHEDULER_ADDR);
        rbx_getstate     = reinterpret_cast<getstate_t>(slide + GETSTATE_ADDR);
        rbx_newthread    = reinterpret_cast<newthread_t>(slide + NEWTHREAD_ADDR);
        rbx_rawgeti    = reinterpret_cast<rawgeti_t>(slide + RAWGETI_ADDR);
        rbx_pushvalue = reinterpret_cast<pushvalue_t>(slide + PUSHVALUE_ADDR);
        rbx_pushcclosure = reinterpret_cast<pushcclosure_t>(slide + PUSHCCLOSURE_ADDR);
        rbx_setfield = reinterpret_cast<setfield_t>(slide + SETFIELD_ADDR);
        rbx_luaopen_base = reinterpret_cast<luaopen_base_t>(slide + LUAOPENBASELIBS_ADDR);
        rbx_isnil = reinterpret_cast<isnil_t>(slide + ISNIL_ADDR);
        rbx_createtable = reinterpret_cast<createtable_t>(slide + CREATETABLE_ADDR);
        
        execute_script("print('hello world')"); //or whatever you want
    }).detach();
}
