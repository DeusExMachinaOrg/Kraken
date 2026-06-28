#define LOGGER "fixcctl"

#include "ext/logger.hpp"

#include "routines.hpp"
#include "config.hpp"

#include "hta/m3d/Kernel.hpp"
#include "hta/_sLocalContactData.hpp"

namespace kraken::fix::cctlleakfix {
    void AllocLocalContactsHook(int flags)
    {
        flags &= 0xFFFF;
        hta::_sLocalContactData*& gLocalContacts = hta::_sLocalContactData::Instance();

        // Если раскомментировать, то, находясь в игре (и gLocalContacts != nullptr),
        // при загрузке сейва (в котором gLocalContacts != nullptr) произойдет повторное освобождение памяти (непонятно где)
        //if (gLocalContacts) {
        //    hta::m3d::Kernel::Instance()->g_mar.FreeMem(gLocalContacts, 0, 0);
        //}

        gLocalContacts = (hta::_sLocalContactData*)hta::m3d::Kernel::Instance()->g_mar.AllocMem(40 * flags, nullptr, 0);
    }

    int* iFlags = reinterpret_cast<int*>(0x00A34BA8);

    void __declspec(naked) dCollideCCTL_Hook()
    {
        __asm {
            pushad
            pushfd

            mov eax, iFlags
            mov eax, [eax]
            push eax
            call AllocLocalContactsHook
            add esp, 4

            popfd
            popad

            // Восстанавливаем ровно те инструкции, которые затерли (5 байт для jmp)
            test ebx, ebx
            jz short skip_ebx  // Прыжок на оригинальный loc_8800AE
            
            mov eax, [ebp+0x58]
            // Возврат в оригинальный код после затертых инструкций
            push 0x00880033 
            ret

        skip_ebx:
            push 0x008800AE
            ret
        }
    }

    void Apply()
    {
        const kraken::Config& config = kraken::Config::Instance();

        if (config.cctl_leak_fix.value) {
            LOG_INFO("Feature enabled");
            kraken::routines::Redirect(5, (void*)0x0088002C, (void*)&dCollideCCTL_Hook);

            routines::Nop((void*)0x0087FC47, 2);

            unsigned char xor_eax[] = { 0x31, 0xC0, 0x90 };
            routines::Patch((void*)0x0087FC55, xor_eax, sizeof(xor_eax));

            routines::Nop((void*)0x0087FC83, 5);
        }
    }
}