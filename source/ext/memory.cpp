// Engine-heap hooks for the vc3 container library.
//
// kraken.dll is injected into hta.exe and shares ownership of objects with the
// engine. The vc3::* containers are a source-level clone of the engine STL and
// must allocate/free on the SAME heap the engine uses, otherwise a buffer the
// game allocates (e.g. PhysicBodyPrototypeInfo::m_collisionInfos via the native
// LoadFromXML) and kraken later frees in its own destructor produces a
// cross-heap free and crashes.
//
// The engine routes every allocation through the memory manager exposed as
// Kernel::g_mar (the same manager the game's global operator new/delete and
// std::allocator use). vc3::allocator calls the two hooks below so all vc3
// buffers land on the engine heap.
//
// These are deliberately NOT a global operator new/delete override: that would
// also capture kraken's own static-init allocations (e.g. G_CONFIG, CRT iostream
// /locale facets) which run during DllMain, before the engine — and thus
// Kernel::Instance() — exists. vc3 containers, by contrast, never allocate until
// first use at runtime, by which point g_mar is valid.

#include <cstddef>
#include <cstdint>

#include "hta/m3d/Kernel.hpp"

namespace vc3 {
    void* _EngineAlloc(std::size_t size) {
        return hta::m3d::Kernel::Instance()->g_mar.AllocMem(static_cast<uint32_t>(size), "", 0);
    }

    void _EngineFree(void* ptr) {
        if (ptr) {
            hta::m3d::Kernel::Instance()->g_mar.FreeMem(ptr, "", 0);
        }
    }
}
