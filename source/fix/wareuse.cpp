#define LOGGER "wareuse"

#include "fix/wareuse.hpp"
#include "configstructs.hpp"
#include "ext/logger.hpp"
#include "routines.hpp"
#include "stdafx.hpp"

#include "hta/CMiracle3d.hpp"
#include "hta/DragSlot.hpp"
#include "hta/PointBase.hpp"
#include "hta/ai/Player.hpp"
#include "hta/ai/PrototypeManager.hpp"
#include "hta/m3d/GameImpulse.hpp"
#include "hta/m3d/ui/DragDropItemsWnd.hpp"
#include "hta/m3d/ui/GarageWnd.hpp"

namespace kraken::fix::wareuse {
    static std::vector<configstructs::WareUnits> RepairWares;
    static std::vector<configstructs::WareUnits> RefuelWares;

    const auto Refuel = reinterpret_cast<void(__thiscall*)(void*, int units)>(0x0044BB40);

    #define EPS 0.001f

    const hta::CStr& getChassisName() {
        static const hta::CStr name = "CHASSIS_7";
        return name;
    }

    hta::ai::Vehicle* GetPlayerVehicle() {
        if (auto* player = hta::ai::Player::Instance())
            return player->GetVehicle();
        return nullptr;
    }

    float CalcPartDeficit(hta::ai::VehiclePart* part) {
        if (!part)
            return 0.0f;
        return part->m_durability.m_maxValue.m_value - part->m_durability.m_value.m_value;
    }

    vc3::vector<hta::ai::VehiclePart*> GetDamagedParts(hta::ai::Vehicle* vehicle) {
        vc3::vector<hta::ai::VehiclePart*> damaged;
        if (!vehicle)
            return damaged;

        for (const auto& [partName, part] : vehicle->m_vehicleParts) {
            if (part && partName != getChassisName() && CalcPartDeficit(part) > EPS)
                damaged.push_back(part);
        }
        return damaged;
    }

    bool SmartRepair(int hp, float armor) {
        bool repaired = false;

        hta::ai::Vehicle* vehicle = GetPlayerVehicle();
        if (!vehicle)
            return repaired;

        // === Ремонт HP ===
        {
            hta::ai::NumericInRangeRegenerating<float>& health = vehicle->Health();
            float current = health.m_value.m_value;
            float maxVal = health.m_maxValue.m_value;
            float deficit = maxVal - current;

            float addHp = (std::min)(static_cast<float>(hp), deficit);
            if (addHp > EPS) {
                health.m_value.set(current + addHp);
                repaired = true;
            }
        }

        // === Ремонт брони (деталей) ===
        if (armor <= EPS)
            return repaired;

        vc3::vector<hta::ai::VehiclePart*> damaged = GetDamagedParts(vehicle);
        if (damaged.empty())
            return repaired;

        float remaining = armor;

        while (remaining > EPS && !damaged.empty()) {
            // Находим минимальный дефицит среди оставшихся повреждённых деталей
            float minDeficit = (std::numeric_limits<float>::max)();
            for (hta::ai::VehiclePart* part : damaged) {
                float d = CalcPartDeficit(part);
                if (d > EPS)
                    minDeficit = (std::min)(minDeficit, d);
            }

            if (minDeficit <= EPS)
                break;

            size_t count = damaged.size();
            float perPart = remaining / static_cast<float>(count);
            float add = (std::min)(perPart, minDeficit);

            // Добавляем одинаковое количество прочности всем оставшимся деталям
            for (hta::ai::VehiclePart* part : damaged) {
                float current = part->m_durability.m_value.m_value;
                part->m_durability.m_value.set(current + add);
            }

            remaining -= add * static_cast<float>(count);

            // Удаляем полностью починенные детали из списка
            damaged.erase(std::remove_if(damaged.begin(), damaged.end(), [](hta::ai::VehiclePart* p) { return CalcPartDeficit(p) <= EPS; }), damaged.end());
        }
        return true;
    }

    bool TryRepair(hta::ai::Vehicle* playerVehicle, hta::CStr& name) {
        for (auto wu : RepairWares) {
            if (name == wu.Ware.c_str()) {
                return SmartRepair(wu.Units, wu.Armor);
            }
        }

        return false;
    }

    bool TryRefuel(hta::ai::Vehicle* playerVehicle, hta::CStr& name) {
        for (auto wu : RefuelWares) {
            if (name == wu.Ware.c_str()) {
                const int current = playerVehicle->GetFuel();
                const int max = playerVehicle->GetMaxFuel();

                if (current >= max) {
                    return false;
                }

                int amount = wu.Units;
                if (current + amount > max) {
                    amount = max - current;
                }

                Refuel(nullptr, amount);
                return true;
            }
        }

        return false;
    }

    int __fastcall OnMouseButton0Hook(hta::m3d::ui::DragDropItemsWnd* self, void* _, uint32_t state, const hta::PointBase<float>* at) {
        auto app = hta::CMiracle3d::Instance();
        auto impulse = (hta::m3d::GameImpulse*)app->m_pImpulses;

        if (!hta::m3d::ui::DragDropItemsWnd::m_dragSlot && impulse->m_curKeys.IsThere(0x105)) // ctrl
        {
            hta::ai::GeomRepositoryItem repositoryItem = self->GetItemFromOrigin(*at);
            if (repositoryItem.IsValid()) {
                auto repositoryObj = repositoryItem.GetObj();
                if (repositoryObj && repositoryObj->IsKindOf((hta::m3d::Class*)0x00A0238C)) // Ware class
                {
                    hta::CStr name = hta::ai::PrototypeManager::Instance()->GetPrototypeName(repositoryObj->m_prototypeId);

                    auto playerVehicle = hta::ai::Player::Instance()->GetVehicle();
                    if (TryRepair(playerVehicle, name) || TryRefuel(playerVehicle, name)) {
                        playerVehicle->m_repository->GiveUpThingByObjId(repositoryItem.m_objId);
                        app->m_pInterfaceManager->RemoveWindow(0x24); // Info window
                        return 1;
                    }
                }
            }
        }

        auto OnMouseButton0 = (int (__fastcall*)(hta::m3d::ui::DragDropItemsWnd* dragDropItemsWnd, void* _, uint32_t state, const hta::PointBase<float>* at))0x00443840;

        return OnMouseButton0(self, _, state, at);
    }

    void Apply() {
        LOG_INFO("Feature enabled");
        const kraken::Config& config = kraken::Config::Instance();
        for (const auto& wu : config.ware_units.value) {
            if (wu.Type == configstructs::WareType::REPAIR) {
                RepairWares.push_back(wu);
            } else if (wu.Type == configstructs::WareType::REFUEL) {
                RefuelWares.push_back(wu);
            }
        }
        kraken::routines::OverrideValue(reinterpret_cast<void*>(0x009CB32C), (void*)&OnMouseButton0Hook);
        if (config.wares.value) {
            routines::Override(32, (void*)0x0047FD7F, "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90");
        }
    }
}