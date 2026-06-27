#define LOGGER "wareuse"

#include "fix/wareuse.hpp"
#include "configstructs.hpp"
#include "ext/logger.hpp"
#include "routines.hpp"
#include "stdafx.hpp"

#include "hta/CMiracle3d.hpp"
#include "hta/DragSlot.hpp"
#include "hta/PointBase.hpp"
#include "hta/ai/GeomRepository.hpp"
#include "hta/ai/Player.hpp"
#include "hta/ai/PrototypeManager.hpp"
#include "hta/m3d/GameImpulse.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/ScriptServer.hpp"
#include "hta/m3d/ui/DragDropItemsWnd.hpp"
#include "hta/m3d/ui/GarageWnd.hpp"
#include "hta/m3d/ui/GfxServer.hpp"
#include "hta/m3d/ui/VehiclePartWnd.hpp"

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

    // Outcome of trying to use a ware. `handled` means the click was consumed by us
    // (suppress the vanilla handler); `consume` means the ware should be spent.
    struct WareUseResult {
        bool handled = false;
        bool consume = false;
    };

    void RunScript(const std::string& script) {
        if (script.empty())
            return;
        if (auto* kernel = hta::m3d::Kernel::Instance()) {
            if (auto* scriptServer = kernel->m_scriptServer) {
                scriptServer->execute(script.c_str(), "Kraken");
            }
        }
    }

    WareUseResult TryRepair(hta::ai::Vehicle* playerVehicle, hta::CStr& name) {
        for (const auto& wu : RepairWares) {
            if (name == wu.Ware.c_str()) {
                bool repaired = SmartRepair(wu.Units, wu.Armor);
                bool hasScript = !wu.Script.empty();

                // Nothing to restore and no script to run: let the vanilla handler deal
                // with the click (e.g. clicking a repair kit at full health).
                if (!repaired && !hasScript)
                    return {};

                RunScript(wu.Script);
                if (!wu.Sound.empty())
                    hta::m3d::ui::GfxServer::Instance()->PlayControlSound(wu.Sound.c_str(), 0);
                return { true, wu.Consume };
            }
        }

        return {};
    }

    WareUseResult TryRefuel(hta::ai::Vehicle* playerVehicle, hta::CStr& name) {
        for (const auto& wu : RefuelWares) {
            if (name == wu.Ware.c_str()) {
                bool hasScript = !wu.Script.empty();

                const int current = playerVehicle->GetFuel();
                const int max = playerVehicle->GetMaxFuel();

                bool refueled = false;
                if (wu.Units > EPS && current < max) {
                    int amount = static_cast<int>(wu.Units);
                    if (current + amount > max)
                        amount = max - current;

                    Refuel(nullptr, amount);
                    refueled = true;
                }

                // Tank already full (or no fuel to add) and no script: defer to vanilla.
                if (!refueled && !hasScript)
                    return {};

                RunScript(wu.Script);
                if (!wu.Sound.empty())
                    hta::m3d::ui::GfxServer::Instance()->PlayControlSound(wu.Sound.c_str(), 0);
                return { true, wu.Consume };
            }
        }

        return {};
    }

    // Trampoline back into the real DragDropItemsWnd::OnMouseButton0 (0x00443840).
    // We detour that function at its entry, so calling 0x00443840 directly would
    // re-enter our hook. Instead re-execute the 5 prologue bytes the detour
    // overwrote (push ebx; mov ebx, [esp+8]) and jump past the patch.
    __declspec(naked) int __fastcall OnMouseButton0_Original(hta::m3d::ui::DragDropItemsWnd*, void*, uint32_t, const hta::PointBase<float>*) {
        static constexpr auto kResume = 0x00443845;
        __asm {
            push ebx
            mov  ebx, [esp + 8]
            jmp  kResume
        }
    }

    int __fastcall OnMouseButton0Hook(hta::m3d::ui::DragDropItemsWnd* self, void* _, uint32_t state, const hta::PointBase<float>* at) {
        auto app = hta::CMiracle3d::Instance();
        auto impulse = (hta::m3d::GameImpulse*)app->m_pImpulses;

        // The engine fires OnMouseButton0 twice per click: once on button-down
        // (state != 0) and once on button-up (state == 0). Only act on the press,
        // otherwise a ware with Consume=0 (not spent) gets used twice per click.
        if (state && !hta::m3d::ui::DragDropItemsWnd::m_dragSlot && impulse->m_curKeys.IsThere(0x105)) // ctrl
        {
            hta::ai::GeomRepositoryItem repositoryItem = self->GetItemFromOrigin(*at);
            if (repositoryItem.IsValid()) {
                auto repositoryObj = repositoryItem.GetObj();
                auto playerVehicle = hta::ai::Player::Instance()->GetVehicle();

                // Decide whether the player may use this ware, and how to spend it.
                //  - Repository-backed windows: the item carries its repository
                //    (m_parentRepository). Allow the player's own inventory
                //    (GetVehicle() == player) and the ground-loot repository the
                //    player is standing over (Vehicle::m_groundRepository). A
                //    trader's repository is neither, so shop goods stay blocked.
                //    Spend through that repository (GiveUpThingByObjId).
                //  - Equipped active slot (VehiclePartWnd): the ware is mounted as a
                //    vehicle part and has no backing repository (owner == null). It
                //    is owned if the slot's vehicle id matches the player's. Spend
                //    by clearing the slot, which removes the mounted part.
                hta::ai::GeomRepository* owner = repositoryItem.m_parentRepository;
                hta::m3d::ui::VehiclePartWnd* equippedSlot = nullptr;
                bool owned = false;
                if (owner) {
                    owned = playerVehicle
                            && (owner->GetVehicle() == playerVehicle
                                || owner == playerVehicle->m_groundRepository);
                } else if (playerVehicle) {
                    if (auto* partWnd = self->cast<hta::m3d::ui::VehiclePartWnd>()) {
                        if (partWnd->m_vehicleId == playerVehicle->m_objId) {
                            owned = true;
                            equippedSlot = partWnd;
                        }
                    }
                }

                if (repositoryObj && owned)
                {
                    hta::CStr name = hta::ai::PrototypeManager::Instance()->GetPrototypeName(repositoryObj->m_prototypeId);

                    WareUseResult result = TryRepair(playerVehicle, name);
                    if (!result.handled)
                        result = TryRefuel(playerVehicle, name);

                    if (result.handled) {
                        if (result.consume) {
                            if (equippedSlot)
                                equippedSlot->SetItemObjId(-1); // clear slot -> remove the mounted ware
                            else
                                owner->GiveUpThingByObjId(repositoryItem.m_objId);
                        }
                        app->m_pInterfaceManager->RemoveWindow(0x24); // Info window
                        return 1;
                    }
                }
            }
        }

        return OnMouseButton0_Original(self, _, state, at);
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
        // Inventory and active equipped-item slots are different DragDropItemsWnd
        // subclasses, but none of them override OnMouseButton0 — every one
        // dispatches straight to DragDropItemsWnd::OnMouseButton0 (0x00443840).
        // Patching individual class vtables is whack-a-mole (the active slot is
        // yet another subclass), so detour the shared function itself. Now ctrl+
        // click activation fires for a ware wherever it sits; GetItemFromOrigin is
        // virtual, so it resolves to that window's item, and unhandled clicks fall
        // through to the original via OnMouseButton0_Original.
        kraken::routines::Redirect(5, reinterpret_cast<void*>(0x00443840), (void*)&OnMouseButton0Hook);
        if (config.wares.value) {
            routines::Override(32, (void*)0x0047FD7F, "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90");
        }
    }
}