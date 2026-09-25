#pragma once

#include <cstdint>
#include <string>

namespace kraken::ext::uibookstest::probes {

    int32_t NotifyButton(void* box, void* button, uint32_t message);

    void RunNavProbes(void* box, std::string& fail);
    void RunScrollProbes(void* box, float contentWidth, float contentHeight, std::string& fail);
    void RunPagesModeProbe(std::string& fail);
    void RunAutoScrollProbe(void* box, float contentWidth, float contentHeight, std::string& fail);
    bool RunBandSyncProbe(std::string& fail);

}
