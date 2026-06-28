#define LOGGER "gunlights"

#include "ext/logger.hpp"
#include "fix/gunlights.hpp"
#include "config.hpp"

#include "hta/ai/Vehicle.hpp"
#include "hta/ai/VehiclePart.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/ai/StaticAutoGun.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ActionType.hpp"
#include "hta/CStr.hpp"
#include "hta/Enums.hpp"
#include "hta/m3d/WeatherManager.hpp"
#include "hta/m3d/SgAnimatedModelNode.hpp"

#include "routines.hpp"

namespace kraken::fix::gunlights {
    void __fastcall Gun_SetNodeAction(hta::ai::PhysicObj* obj, int, hta::ActionType action, bool forceRestartAction)
    {
        if (hta::ai::Gun* gun = obj->cast<hta::ai::Gun>()) {
            hta::m3d::SgNode* barrel = gun->m_barrelNode;

            vc3::vector<hta::ActionType> actionsBackup;

            if (barrel) {
                if (barrel->cast<hta::m3d::SgAnimatedModelNode>())
                {
                    hta::m3d::SgAnimatedModelNode* sgBarrel = (hta::m3d::SgAnimatedModelNode*)barrel;
                    actionsBackup = sgBarrel->m_effectActions;
                }
            }

            gun->SetNodeAction(action, forceRestartAction);

            if (actionsBackup.size() > 1) {
                gun->m_barrelNode->SetProperty(hta::PROP_DM_ADD_EFFECT_ACTION, &actionsBackup[1]);
            }
        }
    }

    void ActivateHeadLightsPart(hta::ai::VehiclePart* part, bool bActivate)
    {
        if (part->cast<hta::ai::Gun>())
        {
            hta::ai::Gun* gun = (hta::ai::Gun*)part;

            // skip up-to-date headlights
            if (gun->m_barrelNode) {
                if (hta::m3d::SgAnimatedModelNode* barrel = gun->m_barrelNode->cast<hta::m3d::SgAnimatedModelNode>()) {
                    hta::ActionType state = bActivate ? hta::AT_RESERVED2 : hta::AT_RESERVED1;
                    if (barrel->m_effectActions.size() > 1 && barrel->m_effectActions[1] == state)
                        return;
                }
            }

            vc3::vector<hta::ActionType> actions;
            actions.push_back(hta::AT_STAND1);
            if (bActivate)
                actions.push_back(hta::AT_RESERVED2);
            else
                actions.push_back(hta::AT_RESERVED1);

            gun->SetEffectActions(actions);

            hta::m3d::SgNode* barrel = gun->m_barrelNode;
            if (barrel)
            {
                barrel->SetProperty(hta::PROP_DM_ARRAY_EFFECT_ACTIONS, &actions);
            }
        }
    }

    void __fastcall ActivateHeadLights(hta::ai::Vehicle* vehicle, int, bool bActivate)
    {
        const hta::ActionType ogAction = vehicle->m_effectActions[1];
        vehicle->ActivateHeadLights(bActivate);
        if (ogAction == hta::AT_RESERVED2 && bActivate)
        {
            return;
        }

        if (ogAction == hta::AT_RESERVED1 && !bActivate)
        {
            return;
        }

        for (auto const& [name, part] : vehicle->m_vehicleParts) {
            ActivateHeadLightsPart(part, bActivate);
        }
    }

    void __stdcall StaticAutoGun_Update(hta::ai::StaticAutoGun* staticGun)
    {
        static const hta::CStr& str_cannon = *(hta::CStr*)0x00A1ACAC;
        hta::ai::Gun* gun = (hta::ai::Gun*)staticGun->GetPartByName(str_cannon);

        if (gun)
        {
            bool enable =  hta::ai::CServer::Instance()->GetWorld()->GetWeatherManager().m_curDayTime == hta::m3d::GTP_NIGHT_TIME;
            ActivateHeadLightsPart(gun, enable);
        }
    }

    void __declspec(naked) StaticAutoGun_Update_Hook()
    {
        static constexpr auto hExit = 0x00743747;

        __asm
        {
            pushad;
            push ecx;
            call StaticAutoGun_Update;
            popad;

            mov eax, [esp + 0x08];
            sub esp, 0x20;

            jmp hExit;
        }
    }

    void __fastcall Gun_InternalCreateVisualPart(hta::ai::Gun* self, int)
    {
        self->hta::ai::VehiclePart::_InternalCreateVisualPart();
        self->_CreateBarrelNode();

        // HEADLIGHTS
        hta::m3d::Object* parent = self->GetParent();
        if (parent) {
            hta::ai::Vehicle* vehicle = parent->cast<hta::ai::Vehicle>();
            if (vehicle) {
                const bool headlights_activated = vehicle->m_effectActions[1];
                ActivateHeadLightsPart(self, headlights_activated);
            }
        }
    }

    bool& LightActivated = *(bool*)0x00A418DE;

    void __fastcall UpdateHeadlightsControl(hta::ai::Vehicle* vehicle, int) {
        if (!vehicle) return;

        vehicle->SetUpdatingByODE(true);
        LightActivated = vehicle->m_effectActions[1]; // Update state of headlights control
    }

    void __declspec(naked) Player_AddChild_Hook() {
        __asm {
            pushad
            mov ecx, edi
            call UpdateHeadlightsControl
            popad
            push 0x0065230B
            ret
        }
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.gunlights.value == 0)
            return;

        LOG_INFO("Feature enabled");

        kraken::routines::ChangeCall((void*)0x004018D6, ActivateHeadLights);
        kraken::routines::ChangeCall((void*)0x005EC7F1, ActivateHeadLights);

        kraken::routines::ReplaceCall((void*)0x006DE1F0, Gun_SetNodeAction);

        kraken::routines::ReplaceCall((void*)0x006DE20C, Gun_SetNodeAction);

        kraken::routines::ReplaceCall((void*)0x006DE24B, Gun_SetNodeAction);

        kraken::routines::ReplaceCall((void*)0x006DE26B, Gun_SetNodeAction);

        kraken::routines::ReplaceCall((void*)0x006DE2B2, Gun_SetNodeAction);

        kraken::routines::Redirect(0x10, (void*)0x006E4990, Gun_InternalCreateVisualPart);

        kraken::routines::Redirect(12, (void*)0x006522FF, Player_AddChild_Hook);

        kraken::routines::Redirect(7, (void*) 0x00743740, StaticAutoGun_Update_Hook);
    }
}