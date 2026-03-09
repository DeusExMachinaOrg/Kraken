#include "common/routines.hpp"
#include "fixes/difficultywndescapefix.hpp"

#include "hta/CMiracle3d.hpp"
#include "hta/m3d/Kernel.hpp"

namespace kraken::fix::difficultywndescapefix {
    template <class Out, class In> Out UnsafeCast(In x) {
        union {
            In a;
            Out b;
        };
        a = x;
        return b;
    };

    class MainMenuUI_Fixed {
      public:
        MainMenuUI_Fixed() = delete;

        void OnNewGame() {
            int32_t retVal = 0;
            hta::CMiracle3d::Instance()->m_pInterfaceManager->ShowWindow(172, true, true, true, true, &retVal);

            if (retVal == 1) {
                const std::string cmd = std::string("/map ") + hta::m3d::Kernel::Instance()->GetEngineCfg().m_firstLevel.GetS();
                hta::m3d::Kernel::Instance()->GetEngineCfg().m_console->executeCommand(cmd.c_str());
            }
        }
    };

    void Apply() {
        routines::Redirect(0x0129, (void*)0x004C1EA0, UnsafeCast<void*>(&MainMenuUI_Fixed::OnNewGame));

        // Replace RequestDifficultyWnd::OnKey by m3d::ui::ModalWnd::OnKey
        routines::OverrideValue(reinterpret_cast<void*>(0x009D48A4), reinterpret_cast<void*>(0x00676BA0));
    };
};
