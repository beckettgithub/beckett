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

#include "luau/VM/src/lapi.h"
#include "luau/VM/src/lstate.h"
//#include <lstate.h>
#include <lualib.h>

// ─── Luau Compiler ─────────────────────────────────────────────────────────────
#include "Luau/Compiler.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/BytecodeUtils.h"

#define lua_upvalueindex(i) (LUA_GLOBALSINDEX - (i))
#define rbx_pop(L, n)      (*rbx_settop)(reinterpret_cast<uint64_t>(L), -(n)-1)
#define rbx_getglobal(L,s) rbx_getfield(L, LUA_GLOBALSINDEX, (s))
#define rbx_setglobal(L,s) rbx_setfield(L, LUA_GLOBALSINDEX, (s))
#define rbx_tostring(L,i)  rbx_tolstring(L, (i), NULL)

// ─── Addresses ────────────────────────────────────────────────────────────────
static constexpr uint64_t PUSHLSTRING_ADDR     = 0x1026f30da; //"ipairs" -> fourth call
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
static constexpr uintptr_t PCALL_ADDR = 0x1026f5ec0;
static constexpr uintptr_t TASKDEFER_ADDR = 0x100d08ac8;
static constexpr uintptr_t TOOBJECT_ADDR = 0x1026f201c;
// ─── Signatures ────────────────────────────────────────────────────────────────
using tolstring_t    = const char* (*)(lua_State* L, int idx, size_t* len);
using luauload_t     = int64_t (*)(lua_State* L, const char* name, const char* bc, size_t len, int env);
using settop_t       = int64_t (*)(lua_State* L, int idx);
using spawn_t        = int64_t (*)(lua_State* L);
using print_t        = void (*)(uint8_t level, const char* fmt, ...);
using getstate_t     = lua_State* (*)(uintptr_t script_context, const uint32_t* identity, const uintptr_t* script);
using newthread_t    = lua_State* (*)(lua_State* L);
using pushvalue_t    = void* (*)(lua_State* L, int idx);
using luaopen_base_t = int64_t (*)(lua_State* L);
using getfield_t     = uint64_t (*)(lua_State* L, int idx, const char* name);
using pcall_t = int (*)(lua_State* L, int nargs, int nresults, int errfunc);
using taskdefer_t = int (*)(lua_State* L);
using pushcclosure_t = void (*)(lua_State* rl, int (*)(uint64_t), const char* debugname, int nup, uint64_t cont);
using toobject_t = TValue* (*)(lua_State* L, int idx);
// ─── Globals ───────────────────────────────────────────────────────────────────
static tolstring_t    rbx_tolstring     = nullptr;
static luauload_t     rbx_luauload      = nullptr;
static settop_t       rbx_settop        = nullptr;
static spawn_t        rbx_spawn         = nullptr;
static print_t        rbx_print         = nullptr;
static getstate_t     rbx_getstate      = nullptr;
static newthread_t    rbx_newthread     = nullptr;
static pushvalue_t    rbx_pushvalue     = nullptr;
static luaopen_base_t rbx_luaopen_base  = nullptr;
static pushcclosure_t rbx_pushcclosure = nullptr;
static getfield_t     rbx_getfield = nullptr;
static pcall_t rbx_pcall = nullptr;
static taskdefer_t rbx_taskdefer = nullptr;
static toobject_t rbx_toobject = nullptr;

// ─── Caching & Scheduler ───────────────────────────────────────────────────────
static lua_State* newL = nullptr;
lua_State* base;
namespace scheduler {
    struct job { void** vtable; std::string job_name; };
    std::vector<job*> scheduler_jobs;
}

// ─── Compile Helpers ────────────────────────────────────────────────────────────
std::string compileWithCustomEncoder(const std::string& src) {
    struct MyEncoder final : Luau::BytecodeEncoder {
        void encode(uint32_t* data, size_t count) override {
            for (size_t i = 0; i < count;) {
                uint8_t& op = *reinterpret_cast<uint8_t*>(data + i);
                i += Luau::getOpLength(LuauOpcode(op));
                op = static_cast<uint8_t>(op * 227);
            }
        }
    };

    Luau::CompileOptions opts;
    opts.debugLevel = 1;
    opts.optimizationLevel = 1;
    static MyEncoder encoder;
    return Luau::compile(src, opts, {}, &encoder);
}

// ─── Identity Helpers ───────────────────────────────────────────────────────────
__int128* rbx_getidentity(lua_State* L) {
    return (__int128*)(*(uint64_t*)(reinterpret_cast<uint64_t>(L) + 0x78) + 0x30);
}
void rbx_setidentity(lua_State* L, __int128 id) {
    *rbx_getidentity(L) = id;
}

// ─── Base Lib Prep ─────────────────────────────────────────────────────────────
void prepare_base_lib(lua_State* L) {
    rbx_setidentity(L, 8);
    rbx_luaopen_base(L);
    rbx_settop(L, 1);
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
            lua_State* thread = rbx_getstate(script_context, &identity, &dummy);
                    

                    rbx_print(2, "[DBG] candidate_ctx=0x%llx → thread=0x%llx", script_context, thread);

                    if (thread != 0) {
                        
                        return script_context;
                    }
        
        }

        jobs_start += stride;
    }

    return 0;
}
uintptr_t capabilities = ~0ULL;
void SetProtoCapabilities(Proto* proto) {
    proto->userdata = &capabilities;
    for (int i = 0; i < proto->sizep; i++)
    {
        SetProtoCapabilities(proto->p[i]);
    }
}

// ─── Script Execution ──────────────────────────────────────────────────────────
bool execute_script(const std::string& src) {
    std::string bc = compileWithCustomEncoder(src);
    
    rbx_print(0, "execute_script start");
    uint32_t  identity = 8;
    uintptr_t dummy    = 0;
    if (!newL) {
        uintptr_t ctx = get_script_context();
        base = rbx_getstate(ctx, &identity, &dummy);
        rbx_print(0, "getstate ran");
        rbx_setidentity(base, 8);
        newL = rbx_newthread(base);
        
        rbx_print(0, "newthread ran");
        
    }
    rbx_settop(base, 0);
    lua_gc(base, LUA_GCSTOP, 0);
    lua_State* L = newL;
    
    
    

    if (!L) {
            rbx_print(3, "Failed to create Lua thread");
            lua_gc(base, LUA_GCRESTART, 0);
            return false;
    }
    
    prepare_base_lib(L);
    rbx_settop(L, 0);
    
    rbx_pushcclosure(L, (int(*)(uint64_t))rbx_taskdefer, "task.defer", 0, 0);
    uintptr_t Userdata = (uintptr_t)L->userdata;
    if (!Userdata) {
            rbx_print(3, "Invalid userdata in thread");
            lua_gc(base, LUA_GCRESTART, 0);
            return false;
        }
    rbx_print(0, "Userdata set (but not added to)");
    rbx_print(1, "[DBG] Userdata: 0x%llx", Userdata);
    uintptr_t capabilitiesPtr = Userdata + 0x48;
    
    rbx_print(0, "Valid address for userdata");
    rbx_setidentity(L, 8);
    rbx_print(0, "set identity to 8");
    if (capabilitiesPtr) {
        *(int64_t*)capabilitiesPtr = ~0ULL;
    }
    else {
        rbx_print(3, "Invalid memory addresses for identity or capabilities");
        lua_gc(base, LUA_GCRESTART, 0);
        return false;
    }
    
    
    
    if (rbx_luauload(L, "@beckett", bc.data(), bc.size(), 0) != 0) {
        size_t errlen = 0;
        const char* err = rbx_tolstring(L, -1, &errlen);
        rbx_print(3, "%.*s", (int)errlen, err ? err : "load failed");
        rbx_settop(L, 0);
        lua_gc(base, LUA_GCRESTART, 0);
        return false;
    }
    rbx_print(0, "luau_load ran");
    Closure* closure = clvalue(rbx_toobject(L, -1));
    if (!closure) {
            rbx_print(3, "Failed to retrieve closure");
            lua_gc(base, LUA_GCRESTART, 0);
            return false;
        }
    if (!closure || closure->isC || !closure->l.p) {
        rbx_print(3, "Skipping SetProtoCapabilities: invalid or C closure");
    } else {
        SetProtoCapabilities(closure->l.p);
    }

    rbx_print(0, " closure set up");
    //rbx_setidentity(L, 8);
    int top = lua_gettop(L);
    rbx_print(0, "[DBG] stack size before spawn = %d", top);
    //rbx_spawn(L);
    if (rbx_pcall(L, 1, 0, 0) != LUA_OK) {
        size_t errlen = 0;
        const char* err = rbx_tolstring(L, -1, &errlen);
        rbx_print(3, "%.*s", (int)errlen, err ? err : "pcall failed");
        rbx_settop(L, 0);
        lua_gc(base, LUA_GCRESTART, 0);
        return false;
        }
    rbx_print(0, "ran pcall");
    rbx_settop(L, 0);
    //lua_pop(L, 1);
    lua_gc(L, LUA_GCRESTART, 0);
    return true;
}

// ─── Entry Point ───────────────────────────────────────────────────────────────
__attribute__((constructor))
void c_main() {
    std::thread([] {
        uintptr_t slide = _dyld_get_image_vmaddr_slide(0);

        // initialize pointers with proper casts to lua_State*
        rbx_print        = reinterpret_cast<print_t>(slide + PRINT_ADDR);
        rbx_tolstring    = reinterpret_cast<tolstring_t>(slide + TOSTRING_ADDR);
        rbx_luauload     = reinterpret_cast<luauload_t>(slide + LUAULOAD_ADDR);
        rbx_settop       = reinterpret_cast<settop_t>(slide + SETTOP_ADDR);
        rbx_spawn        = reinterpret_cast<spawn_t>(slide + SPAWN_ADDR);
        rbx_getstate     = reinterpret_cast<getstate_t>(slide + GETSTATE_ADDR);
        rbx_newthread    = reinterpret_cast<newthread_t>(slide + NEWTHREAD_ADDR);
        rbx_pushvalue    = reinterpret_cast<pushvalue_t>(slide + PUSHVALUE_ADDR);
        rbx_luaopen_base = reinterpret_cast<luaopen_base_t>(slide + LUAOPENBASELIBS_ADDR);
        rbx_pcall = reinterpret_cast<pcall_t>(slide + PCALL_ADDR);
        rbx_taskdefer = reinterpret_cast<taskdefer_t>(slide + TASKDEFER_ADDR);
        // finally, run your script
        rbx_pushcclosure = reinterpret_cast<pushcclosure_t>(slide + PUSHCCLOSURE_ADDR);
        rbx_toobject = reinterpret_cast<toobject_t>(slide + TOOBJECT_ADDR);
        execute_script("print('hello')");
    }).detach();
}
