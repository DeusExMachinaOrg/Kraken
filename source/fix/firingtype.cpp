#include "fix/firingtype.hpp"
#include "routines.hpp"
#include "hta/CStr.hpp"
#include "hta/ai/Enums.hpp"
#include <Windows.h>
#include <cstring>

// Hook for ai::GunPrototypeInfo::FiringType2Str (RVA 0x2E0BD0).
//
// HTA has FT_NUM_FIRING_TYPES = 13 with no FT_THORNS. Meridian added
// FT_THORNS = 13. The original function returns an empty CStr for any
// value >= 13, causing "MISSING" in the shop UI for thorn weapons.
//
// Calling convention (deduced from disasm):
//   ECX = hidden return CStr* retval
//   EDX = ai::FiringTypes type
//   returns EAX = retval

namespace {

// Executable trampoline: first 5 bytes of original + JMP back to original+5.
static uint8_t s_firingType2StrTrampoline[16];

static hta::CStr* __fastcall FiringType2Str_Hook(hta::CStr* retval, hta::ai::FiringTypes type) {
    if (type == hta::ai::FT_THORNS) {
        new (retval) hta::CStr("Thorns");
        return retval;
    }
    using Fn = hta::CStr*(__fastcall*)(hta::CStr*, hta::ai::FiringTypes);
    return reinterpret_cast<Fn>(static_cast<void*>(s_firingType2StrTrampoline))(retval, type);
}

} // namespace

namespace kraken::fix::firingtype {

void Apply() {
    void* const origFn = reinterpret_cast<void*>(0x006E0BD0);

    // Build trampoline: copy first 5 bytes, then JMP to original+5.
    DWORD oldProt;
    VirtualProtect(s_firingType2StrTrampoline, sizeof(s_firingType2StrTrampoline),
                   PAGE_EXECUTE_READWRITE, &oldProt);
    std::memcpy(s_firingType2StrTrampoline, origFn, 5);
    s_firingType2StrTrampoline[5] = 0xE9;
    const uintptr_t trampolineJmpSrc = reinterpret_cast<uintptr_t>(s_firingType2StrTrampoline) + 10;
    const uintptr_t trampolineTarget = reinterpret_cast<uintptr_t>(origFn) + 5;
    *reinterpret_cast<int32_t*>(s_firingType2StrTrampoline + 6) =
        static_cast<int32_t>(trampolineTarget - trampolineJmpSrc);

    // Redirect original to hook (5 bytes: JMP FiringType2Str_Hook).
    kraken::routines::Redirect(5, origFn, reinterpret_cast<void*>(&FiringType2Str_Hook));

    // LoadFromXML checks `if (Str2FiringType(str) == FT_NUM_FIRING_TYPES /*0xd*/)` to
    // log "Unknown firing type". Shift the threshold from 0xd to 0xe so FT_THORNS (13)
    // is no longer treated as invalid.
    // Instruction: cmp eax, 0xd  at RVA 0x2E0216 → byte 0x0D at offset +2 = RVA 0x2E0218
    kraken::routines::OverrideValue<uint8_t>(reinterpret_cast<void*>(0x006E0218), 0x0E);
}

} // namespace kraken::fix::firingtype
