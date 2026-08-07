#define LOGGER "ENTRY"

#include "stdafx.hpp"
#include "config.hpp"
#include "routines.hpp"

#include "ext/logger.hpp"
#include "ext/runtime.hpp"
#include "ext/impulse.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>

#include "fix/fileserver.hpp"
#include "fix/physic.hpp"
#include "fix/breakablediag.hpp"
#include "fix/autobrakefix.hpp"
#include "fix/objcontupgrade.hpp"
#include "fix/luabinds.hpp"
#include "fix/posteffectreload.hpp"
#include "fix/wareuse.hpp"
#include "fix/recollectionfix.hpp"
#include "fix/ultrawide.hpp"
#include "fix/fastloading.hpp"
#include "fix/kineticfriction.hpp"
#include "fix/cardan.hpp"
#include "fix/tactics.hpp"
#include "fix/complexschwarz.hpp"
#include "fix/skinfix.hpp"
#include "fix/cctlleakfix.hpp"
#include "fix/locationdebug.hpp"
#include "fix/difficultywndescapefix.hpp"
#include "fix/mortarvolleylauncherfix.hpp"
#include "fix/gunlights.hpp"
#include "fix/bossmetalarm.hpp"
#include "fix/testharness.hpp"
#include "fix/jolt.hpp"
#include "fix/joltshadow.hpp"
#include "fix/odediag.hpp"
namespace kraken {
    HANDLE  G_MODULE = nullptr;
    Config* G_CONFIG = new Config();

    // Formats "module.ext+0xRVA" for a code address - the same module+RVA convention used
    // throughout this codebase's own VA-based reverse-engineering (docs/jolt-integration-
    // techanalysis.md's "VA 0x..." references are just this + a fixed 0x400000 image base for
    // hta.exe/Meridian113.exe), so a logged frame can be looked up directly against game.pdb or
    // kraken.pdb without any extra translation.
    static void DescribeCodeAddress(void* addr, char* buf, size_t bufSize) {
        HMODULE hMod = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR) addr, &hMod);
        char modPath[MAX_PATH] = {};
        if (hMod != nullptr)
            GetModuleFileNameA(hMod, modPath, MAX_PATH);
        const char* modName = modPath;
        if (const char* slash = strrchr(modPath, '\\'))
            modName = slash + 1;
        snprintf(buf, bufSize, "%s+0x%zx", modName[0] ? modName : "?",
            hMod ? ((uintptr_t) addr - (uintptr_t) hMod) : (uintptr_t) addr);
    }

    // Live-confirmed noise source to filter out of KrakenCrashHandler below: ntdll.dll/
    // MSVCR71.dll (the game's own ancient CRT) routinely raise-and-internally-recover an
    // EXCEPTION_ACCESS_VIOLATION as part of normal, unrelated control flow (observed 3x in 45s
    // of ordinary gameplay, always at the same ntdll.dll+0x541d0 address, always harmless -
    // the process kept running normally each time). A vectored handler sees these same as any
    // other exception, so without this filter every crash report would be buried in false
    // positives. Only hta.exe's own code and kraken.dll itself are ever actually "our" crash.
    static bool IsGameOrKrakenCode(void* addr) {
        HMODULE hMod = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR) addr, &hMod);
        if (hMod == nullptr)
            return false;
        char modPath[MAX_PATH] = {};
        GetModuleFileNameA(hMod, modPath, MAX_PATH);
        const char* modName = modPath;
        if (const char* slash = strrchr(modPath, '\\'))
            modName = slash + 1;
        return _stricmp(modName, "hta.exe") == 0 || _stricmp(modName, "kraken.dll") == 0;
    }

    // Last-resort crash reporter (docs: added to diagnose a reproducible access-violation crash
    // right after loading a save with many Jolt-driven AI vehicles under [jolt_harness] apply=1).
    // Logs the faulting address/registers and a manual EBP-chain stack walk (this is a 32-bit
    // Debug build, so frame pointers are preserved even without /Od-independent unwind tables) -
    // every frame is reported as "module+0xRVA" so it can be matched straight back to source via
    // the game/kraken PDBs, the same way this project already resolves reversed VAs.
    //
    // Registered as a VECTORED exception handler, not SetUnhandledExceptionFilter: the latter is
    // a single process-wide slot that whoever calls it LAST wins outright, and hta.exe/the engine
    // apparently installs its own later during startup (confirmed live - our
    // SetUnhandledExceptionFilter registration silently never fired for a reproducible crash).
    // AddVectoredExceptionHandler(1, ...) instead inserts at the FRONT of a chain every registered
    // handler in the process gets a look at, first-chance, before any __except/SEH frame further
    // down the stack even gets a chance to handle+recover from it - can't be silently overridden
    // by a later registration the way the unhandled-filter slot can.
    //
    // Trade-off: a vectored handler sees EVERY exception in the process, including ones some inner
    // __try/__except goes on to handle and recover from (not just the one that ultimately kills
    // it) - filtered to EXCEPTION_ACCESS_VIOLATION specifically to keep this from spamming
    // kraken.log with benign SEH-based control flow elsewhere in the engine/CRT/Jolt. Always
    // returns EXCEPTION_CONTINUE_SEARCH so it never changes what actually handles/recovers from
    // the exception - this only observes.
    static LONG CALLBACK KrakenCrashHandler(EXCEPTION_POINTERS* info) {
        __try {
            EXCEPTION_RECORD* er  = info->ExceptionRecord;
            CONTEXT*          ctx = info->ContextRecord;
            // 0xE06D7363 ('msc') is the MSVC C++ `throw`. Normally it is pure control flow and
            // must NOT be logged (the CRT and the engine both throw routinely), but it is also
            // how a fatal std::bad_alloc / Jolt-side throw manifests, and one of those is
            // currently killing the process with no other trace. Log a bounded number of them
            // with a stack, then go quiet again so ordinary handled throws can't flood the log.
            constexpr DWORD kMsvcCppException = 0xE06D7363;
            const bool isAv  = er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION;
            const bool isCpp = er->ExceptionCode == kMsvcCppException;
            if (!isAv && !isCpp)
                return EXCEPTION_CONTINUE_SEARCH;
            if (isCpp) {
                static int s_cppExceptionsLogged = 0;
                if (s_cppExceptionsLogged >= 8)
                    return EXCEPTION_CONTINUE_SEARCH;
                ++s_cppExceptionsLogged;
            }
            if (!IsGameOrKrakenCode(er->ExceptionAddress))
                return EXCEPTION_CONTINUE_SEARCH; // ntdll/CRT-internal noise, see IsGameOrKrakenCode's comment

            char where[256];
            DescribeCodeAddress(er->ExceptionAddress, where, sizeof(where));
            LOG_ERROR("=== UNHANDLED EXCEPTION === code=0x%08X addr=%p (%s) flags=0x%08X",
                (unsigned) er->ExceptionCode, er->ExceptionAddress, where, (unsigned) er->ExceptionFlags);
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
                const char* kind = er->ExceptionInformation[0] == 0 ? "reading"
                    : (er->ExceptionInformation[0] == 1 ? "writing" : "executing");
                LOG_ERROR("Access violation %s address 0x%p", kind, (void*) er->ExceptionInformation[1]);
            }

            LOG_ERROR("Registers: eip=0x%08X esp=0x%08X ebp=0x%08X eax=0x%08X ebx=0x%08X ecx=0x%08X edx=0x%08X esi=0x%08X edi=0x%08X",
                ctx->Eip, ctx->Esp, ctx->Ebp, ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi);

            // Bounds-checked against the real thread stack limits so a corrupted/garbage EBP
            // chain can't wander into unmapped memory and fault a second time inside the handler.
            ULONG_PTR stackLow = 0, stackHigh = 0;
            GetCurrentThreadStackLimits(&stackLow, &stackHigh);

            LOG_ERROR("Stack trace (EBP chain):");
            uintptr_t* ebp = (uintptr_t*) ctx->Ebp;
            for (int depth = 0; depth < 48 && ebp != nullptr; ++depth) {
                if ((uintptr_t) ebp < stackLow || (uintptr_t) (ebp + 2) > stackHigh)
                    break;
                uintptr_t retAddr = ebp[1];
                if (retAddr == 0)
                    break;
                char frameWhere[256];
                DescribeCodeAddress((void*) retAddr, frameWhere, sizeof(frameWhere));
                LOG_ERROR("  #%d 0x%p (%s)", depth, (void*) retAddr, frameWhere);
                uintptr_t* nextEbp = (uintptr_t*) ebp[0];
                if (nextEbp <= ebp) // chain must climb toward higher addresses, else it's corrupt/looping
                    break;
                ebp = nextEbp;
            }
            LOG_ERROR("=== END CRASH REPORT ===");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Logging itself faulted - nothing more can be safely done here.
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void ConstantHotfix() {
        routines::Override(sizeof(uint32_t), (void*) 0x0057BCAF, (char*) &G_CONFIG->save_height.value);
        routines::Override(sizeof(uint32_t), (void*) 0x0070808B, (char*) &G_CONFIG->view_resolution.value);
        routines::Override(sizeof(uint32_t), (void*) 0x00708092, (char*) &G_CONFIG->view_resolution.value);
        routines::Override(sizeof(float),    (void*) 0x00602D25, (char*) &G_CONFIG->gravity.value);
        routines::Override(sizeof(uint32_t), (void*) 0x005539D5, (char*) &G_CONFIG->price_fuel.value);
        routines::Override(sizeof(uint32_t), (void*) 0x0057BCA8, (char*) &G_CONFIG->save_width.value);
        routines::RemapPtr((void*) 0x005DAC06, &G_CONFIG->keep_throttle.value);
        routines::RemapPtr((void*) 0x005DAC81, &G_CONFIG->handbrake_power.value);
        routines::Override(sizeof(float), (void*) 0x004017DB, (char*) &G_CONFIG->brake_power.value);
        routines::Override(sizeof(bool),  (void*) 0x007DFADC, (char*) &G_CONFIG->friend_damage.value);
        routines::Override(sizeof(float),    (void*) 0x00602D4E, (char*) &G_CONFIG->contact_surface_layer.value);
        routines::Override(sizeof(float),    (void*) 0x00602D5E, (char*) &G_CONFIG->cfm.value);
        routines::Override(sizeof(float),    (void*) 0x00602D6E, (char*) &G_CONFIG->erp.value);
        routines::OverrideValue((void*) 0x0056BF09, (uint8_t) 0xEB); // Render all quest icons on radar

        // TODO: [Invesigation] Repaint Price
        // That's not work. Need to more deep research for fix it.
        // Look here [0x00474312] void __thiscall SkinsWnd::BuySkin(SkinsWnd *this)
        // routines::Override(sizeof(uint32_t), (void*) 0x00474641, (char*) &G_CONFIG->price_paint.value);
    };

    API void EntryPoint(HANDLE module) {
        G_MODULE = module;

        logger::Init();
        AddVectoredExceptionHandler(1, &KrakenCrashHandler);
        runtime::Init();
        impulse::Init();

        LOG_INFO("Prepare patches");
        ConstantHotfix();
        fix::fileserver::Apply();
        fix::physic::Apply();
        fix::breakablediag::Apply();
        fix::autobrakefix::Apply();
        fix::objcontupgrade::Apply();
        fix::luabinds::Apply(G_CONFIG);
        fix::posteffectreload::Apply(G_CONFIG);
        fix::wareuse::Apply();
        fix::recollection::Apply();
        fix::ultrawide::Apply();
        fix::fastloading::Apply();
        //fix::kineticfriction::Apply();
        fix::cardan::Apply();
        fix::tactics::Apply();
        fix::complexschwarz::Apply();
        fix::skinfix::Apply();
        fix::cctlleakfix::Apply();
        fix::locationdebug::Apply();
        fix::difficultywndescapefix::Apply();
        fix::mortarvolleylauncherfix::Apply();
        fix::gunlights::Apply();
        fix::bossmetalarm::Apply();
        fix::testharness::Apply();
        fix::jolt::Apply();
        fix::joltshadow::Apply();
        fix::odediag::Apply();
    };
};