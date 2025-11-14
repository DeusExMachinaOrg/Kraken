#define LOGGER "wareuse"

#include "ext/logger.hpp"
#include "fix/wareuse.hpp"
#include "configstructs.hpp"
#include "routines.hpp"

#include "hta/CMiracle3d.hpp"
#include "hta/ai/PrototypeManager.hpp"
#include "hta/m3d/ui/drop"
#include "hta/ai/Player.hpp"

namespace kraken::fix::wareuse {
    static std::vector<configstructs::WareUnits> RepairWares;
    static std::vector<configstructs::WareUnits> RefuelWares;

    bool TryRepair(hta::ai::Vehicle* playerVehicle, hta::CStr& name)
    {
        for (auto wu : RepairWares)
        {
            if (name == wu.Ware.c_str())
            {
                float current = playerVehicle->GetHealth();
                float max = playerVehicle->GetMaxHealth();

                if (current >= max)
                {
                    return false;
                }

                float amount = wu.Units;
                if (current + amount > max)
                {
                    amount = max - current;
                }


                Repair(amount);
                return true;
            }
        }

        return false;
    }

    bool TryRefuel(hta::ai::Vehicle* playerVehicle, hta::CStr& name)
    {
        for (auto wu : RefuelWares)
        {
            if (name == wu.Ware.c_str())
            {
                float current = playerVehicle->GetFuel();
                float max = playerVehicle->GetMaxFuel();

                if (current >= max)
                {
                    return false;
                }

                float amount = wu.Units;
                if (current + amount > max)
                {
                    amount = max - current;
                }

                Refuel(nullptr, amount);
                return true;
            }
        }

        return false;
    }

    int __fastcall OnMouseButton0Hook(DragDropItemsWnd* dragDropItemsWnd, int, unsigned int state, const PointBase<float>* at)
    {
        auto app = CMiracle3d::Instance;
        auto impulse = (m3d::GameImpulse*)app->Impulse;

        if (!DragDropItemsWnd::DragSlot && impulse->CurKeys.IsThere(0x105)) // ctrl
        {
            ai::GeomRepositoryItem repositoryItem;
            dragDropItemsWnd->GetItemFromOrigin(&repositoryItem, at);
            if (repositoryItem.IsValid())
            {
                auto repositoryObj = repositoryItem.GetObj();
                if (repositoryObj && repositoryObj->IsKindOf((m3d::Class*)0x00A0238C)) // Ware class
                {
                    CStr name;
                    ai::PrototypeManager::Instance->GetPrototypeName(&name, repositoryObj->m_prototypeId);

                    auto playerVehicle = ai::Player::Instance->GetVehicle();
                    if (TryRepair(playerVehicle, name) || TryRefuel(playerVehicle, name))
                    {
                        playerVehicle->m_repository->GiveUpThingByObjId(repositoryItem.ObjId);
                        app->UiManager->RemoveWindow(0x24); // Info window
                        return 1;
                    }
                }
            }
        }

        return dragDropItemsWnd->OnMouseButton0(state, at);
    }

    void Apply()
    {
        LOG_INFO("Feature enabled");
        const kraken::Config& config = kraken::Config::Instance();
        for (const auto& wu : config.ware_units.value)
        {
            if (wu.Type == configstructs::WareType::REPAIR)
            {
                RepairWares.push_back(wu);
            }
            else if (wu.Type == configstructs::WareType::REFUEL)
            {
                RefuelWares.push_back(wu);
            }
        }
        kraken::routines::OverrideValue(reinterpret_cast<void*>(0x009CB32C), (void*)&OnMouseButton0Hook);
        if (config.wares.value)
        {
            routines::Override(32, (void*) 0x0047FD7F,
            "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90");
        }
    }
}