#pragma once
#ifndef KRAKEN_MEMORY_ITEM_HPP
#define KRAKEN_MEMORY_ITEM_HPP

#include "memory/shared.hpp"

namespace kraken::memory {
    static constexpr size_t VOID_SIZE = sizeof(void*);
    static constexpr size_t PAGE_SIZE = 65536;

    template<size_t S> struct Page;
    template<size_t S> struct Pool;

    struct ItemMeta {
        void*  mPool { nullptr };
        void*  mPage { nullptr };
        void*  mNext { nullptr };
        size_t mSize { 0       };

        static constexpr size_t Size(size_t size) {
            return sizeof(ItemMeta) + size;
        }

        static ItemMeta& Fetch(void* ptr) {
            return *reinterpret_cast<ItemMeta*>(static_cast<char*>(ptr) - sizeof(ItemMeta));
        }
    };

    template<size_t S>
    struct Item : ItemMeta {
        char mData[S];

        Pool<S>* GetPool() const { return static_cast<Pool<S>*>(mPool); }
        Page<S>* GetPage() const { return static_cast<Page<S>*>(mPage); }
        Item<S>* GetNext() const { return static_cast<Item<S>*>(mNext); }

        static constexpr size_t Size(size_t size) {
            return sizeof(ItemMeta) + size;
        }

        static Item<S>& Fetch(void* ptr) {
            return *reinterpret_cast<Item<S>*>(static_cast<char*>(ptr) - sizeof(ItemMeta));
        }
    };
};

#endif
