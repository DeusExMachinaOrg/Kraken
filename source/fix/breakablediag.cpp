#define LOGGER "breakablediag"

#include <cstdint>
#include <cstring>
#include <windows.h>

#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/ai/BreakableObject.hpp"
#include "hta/Quaternion.hpp"

namespace kraken::fix::breakablediag {
    // docs §60 (user rammed a tree under [jolt] enabled=0 - pure ODE - and it fell; asked to
    // check the logs). jolt.cpp/joltshadow.cpp's own diagnostics (e.g. §56.5's
    // InstallCollideObjsDiagnostic) only install from inside fix::jolt::Apply()/
    // fix::joltshadow::Apply(), both gated on config.jolt.value != 0 - useless for observing the
    // NATIVE-only path the user is deliberately testing. This hooks
    // ai::BreakableObject::SetState directly (VA 0x00854af0, docs §58.2/§59) from its own
    // unconditional Apply() below, so it fires regardless of [jolt] enabled. Same safe
    // 5-byte-trampoline pattern as joltshadow.cpp's InstallCollideObjsDiagnostic - copy the
    // overwritten bytes out first so the hook can still call through to the real function.
    using SetStateFn = void(__thiscall*)(void* thisPtr, int32_t state);
    static uint8_t s_setStateTrampoline[16];
    static SetStateFn Real_SetState = nullptr;

    // docs §60.1: the game crashed on first live use of this hook - root cause confirmed via
    // disasm_typed. SetState's first instruction is "mov eax,[esp+4]" (4 bytes) but the SECOND is
    // "sub esp,144" (6 bytes, opcode 81 /5 + imm32) - a plain 5-byte copy (this project's usual
    // trampoline length, safe for the two functions hooked elsewhere) slices that second instruction
    // in half. The leftover opcode byte (0x81) then greedily consumes the injected jmp's own opcode
    // and displacement bytes as a bogus "sub ecx,imm32", and execution falls through into the
    // trampoline buffer's zero-initialized tail, which decodes as "add [eax],al" - a write through
    // whatever eax holds. eax at that point still held the `state` argument (1 in the observed
    // crash), which matches the logged fault exactly: write access violation at address 0x00000001.
    // Fix: copy both whole instructions (10 bytes, ending cleanly at the following "push ebx").
    // Neither is IP-relative (no call/jmp/jcc in range), so the verbatim bytes need no fixup when
    // relocated into the trampoline. routines::Redirect's `size` already supports >5 (it 0xCC-fills
    // the whole overwritten region before writing its 5-byte jmp), so no other change is needed there.
    static constexpr size_t kCopyLen = 10;

    // Declared __fastcall, not __thiscall - MSVC only allows __thiscall on genuine member
    // functions, not free ones. A thiscall CALLER (this in ecx, one stack-pushed arg) and a
    // fastcall callee expecting (ecx, edx, stack-arg) are ABI-compatible here: the real call
    // site never sets edx meaningfully, and `state` lands in the identical stack slot either
    // way (there's only ever one stack-passed argument) - the same trick this project already
    // needs nowhere else because every other hooked function here happens to be __fastcall
    // already; SetState is the first genuine __thiscall target.
    static void __fastcall SetStateHook(void* thisPtr, void* /*unused_edx*/, int32_t state) {
        hta::ai::BreakableObject* obj = reinterpret_cast<hta::ai::BreakableObject*>(thisPtr);
        // Read BEFORE calling through - Real_SetState overwrites m_state immediately below, and
        // the native function's own early-exit (new state == current state, docs §58's own
        // disassembly read) means this only ever logs on a REAL transition, not every frame -
        // safe to leave unconditional, no hotpath_diag gate needed.
        LOG_INFO("docs §60: BreakableObject::SetState this=%p requestedState=%d currentState=%d "
                 "destroyable=%d criticalHitEnergy=%.1f",
                 thisPtr, state, (int32_t) obj->m_state, obj->m_destroyable,
                 (double) obj->m_criticalHitEnergy);
        Real_SetState(thisPtr, state);
        // docs §63 (goal: "деревья падали как в ODE"): §62 found the whole dxJointBreakInfo
        // force-threshold system has ZERO callers anywhere in the binary (xrefs_to on all 5 of its
        // setter functions) - it's dead code, not how trees fall. The real mechanism is almost
        // certainly SetJointAnchor's universal joint acting as a hinge with its own rotational
        // limits (dJointSetUniversalParam) - the tree stays attached and swings/topples under
        // sustained force, no break event needed. Logging rotation (not just m_jointId) directly
        // tests THAT theory: read AFTER the real call returns so this reflects the current frame's
        // actual pose, logged unconditionally (every call, not just ENABLED) so repeat calls while
        // contact continues - matching native's own repeat-call behavior, docs §60.2's 18 no-op
        // calls under pure ODE - naturally produce a time series showing whether the tree is
        // actually rotating over successive frames.
        const hta::CVector pos = obj->GetPosition();
        const hta::Quaternion rot = obj->GetRotation();
        LOG_INFO("docs §63: BreakableObject::SetState this=%p post-call m_jointId=%p pos=(%.2f,%.2f,%.2f) "
                 "rot=(%.4f,%.4f,%.4f,%.4f)",
                 thisPtr, (void*) obj->m_jointId, (double) pos.x, (double) pos.y, (double) pos.z,
                 (double) rot.x, (double) rot.y, (double) rot.z, (double) rot.w);
    }

    static void InstallSetStateDiagnostic() {
        void* const orig = reinterpret_cast<void*>(0x00854af0);
        DWORD oldProtect;
        VirtualProtect(s_setStateTrampoline, sizeof(s_setStateTrampoline), PAGE_EXECUTE_READWRITE, &oldProtect);
        std::memcpy(s_setStateTrampoline, orig, kCopyLen);
        s_setStateTrampoline[kCopyLen] = 0xE9;
        *reinterpret_cast<int32_t*>(s_setStateTrampoline + kCopyLen + 1) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(orig) + kCopyLen
            - (reinterpret_cast<uintptr_t>(s_setStateTrampoline) + kCopyLen + 5));
        VirtualProtect(s_setStateTrampoline, sizeof(s_setStateTrampoline), oldProtect, &oldProtect);
        Real_SetState = reinterpret_cast<SetStateFn>(
            reinterpret_cast<uintptr_t>(s_setStateTrampoline));

        routines::Redirect(kCopyLen, orig, reinterpret_cast<void*>(&SetStateHook));
        LOG_INFO("docs §60: BreakableObject::SetState diagnostic installed (VA 0x00854af0), independent of [jolt] enabled");
    }

    void Apply() {
        InstallSetStateDiagnostic();
    }
}
