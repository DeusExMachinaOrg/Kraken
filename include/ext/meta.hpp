# pragma once

namespace hta {
    class CStr;
    namespace ai {
        class CServer;
        class PrototypeInfo;
    }
}

namespace kraken::meta {
    hta::ai::PrototypeInfo* __fastcall CreatePrototypeInfoByClassName(hta::ai::CServer& self, void*, const hta::CStr& className);
    void Init();
}