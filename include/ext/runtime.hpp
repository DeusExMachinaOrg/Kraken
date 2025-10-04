#ifndef KRAKEN_EXT_RUNTIME
#define KRAKEN_EXT_RUNTIME

#include <functional>

namespace kraken::runtime {
    using Runtime     = void*;                       // Opaque lua_State* object
    using Initializer = std::function<void(void)>;   // Initialization callback
    using Method      = std::function<int(Runtime)>;

    void    Init(void);
    void    OnInit(Initializer func);
    void    OnLoad(Initializer func);
    Runtime GetRuntime(void);
};

#endif