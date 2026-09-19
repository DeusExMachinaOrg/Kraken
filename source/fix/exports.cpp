#define LOGGER "exports"

#include "fix/exports.hpp"

#include "ext/scriptexports.hpp"

#include "hta/ai/Vehicle.hpp"
#include "hta/m3d/Context.hpp"

namespace kraken::fix::exports {
    namespace {
        int __fastcall ExportGetOutOfDifficultPlace(hta::m3d::Context* context) {
            auto* vehicle = static_cast<hta::ai::Vehicle*>(context->asObject(0, "Vehicle"));
            vehicle->GetOutOfDifficultPlace();
            return 0;
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
    }
}
