#include "ext/runtime.hpp"
#include "routines.hpp"

#include <list>

namespace kraken::runtime {
    const auto ScriptServer_Init = (int (__fastcall*)(Runtime* self, void* _))(0x006231D0);

    static struct {
        Runtime*                mHandle { nullptr };
        std::list<Initializer>  mOnInit {         };
        std::list<Initializer>  mOnLoad {         };
    } self {};

    int __fastcall _Wrapper(Runtime* obj, void* _) {
        int error = ScriptServer_Init(obj, NULL);
        self.mHandle = *(Runtime**)0x00A12DF8;
        if (error == 0)
            for (auto init : self.mOnLoad)
                init();
        return error;
    };

    void Init(void) {
        routines::ChangeCall((void*) 0x0058C645, _Wrapper);
        for (auto init : self.mOnInit)
            init();
    };

    void OnInit(Initializer func) {
        self.mOnInit.push_back(func);
    };

    void OnLoad(Initializer func) {
        self.mOnLoad.push_back(func);
    };

    Runtime GetRuntime(void) {
        return self.mHandle;
    };
};