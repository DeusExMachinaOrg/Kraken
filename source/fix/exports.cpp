#define LOGGER "exports"

#include "fix/exports.hpp"

#include "ext/scriptexports.hpp"

#include "hta/ai/Affix.hpp"
#include "hta/ai/AffixManager.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/VehiclePart.hpp"
#include "hta/m3d/Context.hpp"

namespace kraken::fix::exports {
    namespace {
        int __fastcall ExportGetOutOfDifficultPlace(hta::m3d::Context* context) {
            auto* vehicle = static_cast<hta::ai::Vehicle*>(context->asObject(0, "Vehicle"));
            vehicle->GetOutOfDifficultPlace();
            return 0;
        }

        int __fastcall ExportGetAffixes(hta::m3d::Context* context) {
            auto* part = static_cast<hta::ai::VehiclePart*>(context->asObject(0, "VehiclePart"));
            auto* server = hta::ai::CServer::Instance();
            auto* affixManager = server ? server->GetAffixManager() : nullptr;
            if (!part || !affixManager)
                return 0;

            int32_t resultCount = 0;
            const auto pushAffixNames = [context, affixManager, &resultCount](const auto& affixIds) {
                for (const int32_t affixId : affixIds) {
                    const hta::ai::Affix* affix = affixManager->GetAffixById(affixId);
                    if (!affix)
                        continue;

                    context->pushString(affix->GetName().c_str());
                    ++resultCount;
                }
            };

            // Preserve each object's stored order, with prefixes before suffixes.
            pushAffixNames(part->m_appliedPrefixIds);
            pushAffixNames(part->m_appliedSuffixIds);
            return resultCount;
        }
    }

    void Apply() {
        kraken::scriptexports::AddMethod(*hta::ai::Vehicle::p_classObject, {
            "GetOutOfDifficultPlace",
            ExportGetOutOfDifficultPlace,
            "",
            "",
            "Moves the vehicle to a nearby valid position.",
        });

        kraken::scriptexports::AddMethod(*hta::ai::VehiclePart::p_classObject, {
            "GetAffixes",
            ExportGetAffixes,
            "string...",
            "",
            "Returns applied affix names as multiple values, prefixes first and then suffixes.",
        });
    }
}
