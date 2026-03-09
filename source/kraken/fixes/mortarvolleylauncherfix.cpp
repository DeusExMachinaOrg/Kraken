#define LOGGER "mortarvolleylauncherfix"

#include "config/config.hpp"
#include "logger/logger.hpp"
#include "common/routines.hpp"
#include "fixes/mortarvolleylauncherfix.hpp"

#include "hta/ai/MortarVolleyLauncher.hpp"

namespace kraken::fix::mortarvolleylauncherfix {
    bool __fastcall Fire(hta::ai::MortarVolleyLauncher* self, void*, bool enable) {
        bool shouldFire = enable;

        // Если хотим стрелять, проверяем, позволяет ли перезарядка/состояние
        if (enable && !self->CanFire()) {
            shouldFire = false;
        }

        self->m_bIsFiring = shouldFire;

        if (!shouldFire) {
            return false;
        }

        const hta::ai::MortarVolleyLauncherPrototypeInfo* proto = static_cast<const hta::ai::MortarVolleyLauncherPrototypeInfo*>(self->GetPrototypeInfo());

        // Количество стволов/выстрелов в залпе
        size_t volleySize = proto->m_fireLpMatrices.size();

        float originalTime = self->m_timeFromLastShot;
        bool totalSuccess = true;

        // Сбрасываем индекс текущего ствола перед залпом
        self->m_curBarrelIndex = 0;

        // Кол-во доступных выстрелов
        const int shots = (std::min)(volleySize, self->m_ShellsInCurrentCharge);

        for (size_t i = 0; i < shots; ++i) {
            // Выстрел
            if (!self->_DoFire()) {
                totalSuccess = false;
                break;
            }

            // Устанавливаем огромное время, чтобы обмануть проверку Rate of Fire
            // внутри Gun::_DoFire и выстрелить весь залп мгновенно
            self->m_timeFromLastShot = 1000000.0f;
        }

        // Если залп произведен успешно, сбрасываем таймер для отсчета КД
        if (totalSuccess) {
            self->m_timeFromLastShot = 0.0f;
        } else {
            // Если что-то пошло не так, возвращаем время назад
            self->m_timeFromLastShot = originalTime;
        }

        return totalSuccess;
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.mortarvolleylauncherfix.value == 1) {
            LOG_INFO("Feature enabled");
            kraken::routines::Redirect(0xBF, (void*)0x00848EC0, Fire);
        }
    }
}