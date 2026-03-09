#pragma once
#ifndef KRAKEN_MEMORY_PAGE_HPP
#define KRAKEN_MEMORY_PAGE_HPP

#include "memory/item.hpp"
#include "memory/shared.hpp"

namespace kraken::memory {
    template<size_t S> struct Pool;

    template<size_t S>
    struct Page {
    private:
        static constexpr size_t L = (PAGE_SIZE - VOID_SIZE * 7) / sizeof(Item<S>);
    public:
        Pool<S>* mPool     { nullptr };
        Item<S>* mFree     { nullptr };
        size_t   mUsed     { 0       };
        Page<S>* mNextPage { nullptr };
        Page<S>* mPrevPage { nullptr };
        Page<S>* mNextFree { nullptr };
        Page<S>* mPrevFree { nullptr };
        Item<S>  mData[L]  {         };
    public:
        Page();
       ~Page();
    public:
        bool IsFull() const { return mUsed == L; }
        bool IsFree() const { return mUsed == 0; }
    public:
        void Lock(Item<S>& item);
        void Free(Item<S>& item);
    public:
        void* operator new    (size_t size);
        void  operator delete (void* ptr);
    };

    template<size_t S>
    Page<S>::Page() {
        for (size_t i = 0; i < L; i++) {
            mData[i].mPool = nullptr;
            mData[i].mPage = this;
            mData[i].mNext = (i + 1 < L) ? &mData[i + 1] : nullptr;
            mData[i].mSize = S;
        }
        mFree = &mData[0];
    }

    template<size_t S>
    Page<S>::~Page() {
    };

    template<size_t S>
    void Page<S>::Lock(Item<S>& item) {
        mFree = item.GetNext();
        item.mNext = nullptr;
        mUsed++;
        memset(item.mData, 0, std::size(item.mData));
    };

    template<size_t S>
    void Page<S>::Free(Item<S>& item) {
        assert(item.mPage == this);
        item.mSize = 0;
        item.mNext = mFree;
        mFree = &item;
        mUsed--;
    };

    template<size_t S>
    void* Page<S>::operator new(size_t size) {
        return gMalloc(size);
    };

    template<size_t S>
    void Page<S>::operator delete(void* data) {
        gFree(data);
    };
};

#endif
