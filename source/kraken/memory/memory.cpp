#include "memory/memory.hpp"


namespace kraken::memory {

    // Game exe CRT pointers (IAT entries, dereference to get actual function)
    CRT_MALLOC  gMalloc  = (CRT_MALLOC)  0x00965162;
    CRT_REALLOC gRealloc = (CRT_REALLOC) 0x0096519E;
    CRT_FREE    gFree    = (CRT_FREE)    0x00965168;

    Memory& Memory::Instance(void) {
        static Memory instance;
        return instance;
    }

    Memory::Memory()  = default;
    Memory::~Memory() = default;

    void* Memory::Malloc(size_t size) {
        std::lock_guard<std::mutex> lock(mMutex);

        if (size <= 0x010) return mPool_010.Alloc(size);
        if (size <= 0x020) return mPool_020.Alloc(size);
        if (size <= 0x030) return mPool_030.Alloc(size);
        if (size <= 0x040) return mPool_040.Alloc(size);
        if (size <= 0x080) return mPool_080.Alloc(size);
        if (size <= 0x100) return mPool_100.Alloc(size);
        if (size <= 0x200) return mPool_200.Alloc(size);
        if (size <= 0x300) return mPool_300.Alloc(size);
        if (size <= 0x400) return mPool_400.Alloc(size);
        if (size <= 0x800) return mPool_800.Alloc(size);

        auto* raw = static_cast<char*>(gMalloc(ItemMeta::Size(size)));
        auto& meta = *reinterpret_cast<ItemMeta*>(raw);
        meta.mPool = nullptr;
        meta.mPage = nullptr;
        meta.mNext = nullptr;
        meta.mSize = size;
        mUsed += size;
        return raw + sizeof(ItemMeta);
    }

    void* Memory::Realloc(void* data, size_t size) {
        if (!data) return Malloc(size);
        if (!size) { Free(data); return nullptr; }

        auto& item = ItemMeta::Fetch(data);

        void* newData = Malloc(size);
        if (newData) {
            std::memcpy(newData, data, item.mSize < size ? item.mSize : size);
            Free(data);
        }
        return newData;
    }

    void Memory::Free(void* data) {
        if (!data) return;

        std::lock_guard<std::mutex> lock(mMutex);

        auto& item = ItemMeta::Fetch(data);

        mUsed -= item.mSize;

        if (!item.mPool) {
            gFree(&item);
            return;
        }

        size_t size = item.mSize;
        if (size <= 0x010) { mPool_010.Free(&item); return; }
        if (size <= 0x020) { mPool_020.Free(&item); return; }
        if (size <= 0x030) { mPool_030.Free(&item); return; }
        if (size <= 0x040) { mPool_040.Free(&item); return; }
        if (size <= 0x080) { mPool_080.Free(&item); return; }
        if (size <= 0x100) { mPool_100.Free(&item); return; }
        if (size <= 0x200) { mPool_200.Free(&item); return; }
        if (size <= 0x300) { mPool_300.Free(&item); return; }
        if (size <= 0x400) { mPool_400.Free(&item); return; }
        if (size <= 0x800) { mPool_800.Free(&item); return; }
    }

    void* Memory::operator new(size_t size) {
        return gMalloc(size);
    }

    void Memory::operator delete(void* ptr) {
        gFree(ptr);
    }
};

void* operator new(size_t size) {
    return kraken::memory::Memory::Instance().Malloc(size);
};

void operator delete(void* data) {
    kraken::memory::Memory::Instance().Free(data);
};
