#pragma once

namespace hta::m3d {
    struct Class;
    struct Context;
}

namespace kraken::scriptexports {
    // The engine invokes METHOD exports with Context* in ECX.
    using Method = int (__fastcall*)(hta::m3d::Context*);

    struct MethodInfo {
        const char* name;
        Method method;
        const char* returns = "";
        const char* params = "";
        const char* description = "";
    };

    // Registers a normal engine Lua method before the class receives a script handle.
    bool AddMethod(hta::m3d::Class& targetClass, const MethodInfo& method);

}
