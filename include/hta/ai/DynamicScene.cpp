#include "hta/ai/DynamicScene.hpp"

namespace ai {
    DynamicScene* DynamicScene::Instance() {
        return *(DynamicScene**)(0x00A12958);
    };

    const DynamicScene::SoilProps& DynamicScene::GetSoilProps(uint32_t x, uint32_t y) const {
        FUNC(0x00605220, const SoilProps&, __thiscall, jump, const DynamicScene*, int32_t x, int32_t y);
        return jump(this, x, y);
    };
};