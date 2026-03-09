#pragma once
#ifndef KRAKEN_MEMORY_POOL_HPP
#define KRAKEN_MEMORY_POOL_HPP

#include "memory/page.hpp"
#include "memory/shared.hpp"

namespace kraken::memory {

    template<size_t S>
    struct Pool {
    public:
        Page<S>* mPageHead { nullptr };
        Page<S>* mFreeHead { nullptr };
    public:
        Pool();
       ~Pool();
    public:
        void* Alloc(size_t size);
        void  Lock(Item<S>& item);
        void  Free(Item<S>& item);
        void  Free(void* ptr);
    private:
        void  PagePush(Page<S>* page);
        void  PageRemove(Page<S>* page);
        void  FreePush(Page<S>* page);
        void  FreeRemove(Page<S>* page);
    };

    template<size_t S>
    Pool<S>::Pool() = default;

    template<size_t S>
    Pool<S>::~Pool() {
        auto* page = mPageHead;
        while (page) {
            auto* next = page->mNextPage;
            delete page;
            page = next;
        }
    }

    template<size_t S>
    void Pool<S>::PagePush(Page<S>* page) {
        page->mNextPage = mPageHead;
        page->mPrevPage = nullptr;
        if (mPageHead) mPageHead->mPrevPage = page;
        mPageHead = page;
    }

    template<size_t S>
    void Pool<S>::PageRemove(Page<S>* page) {
        if (page->mPrevPage) page->mPrevPage->mNextPage = page->mNextPage;
        else                 mPageHead = page->mNextPage;
        if (page->mNextPage) page->mNextPage->mPrevPage = page->mPrevPage;
        page->mNextPage = nullptr;
        page->mPrevPage = nullptr;
    }

    template<size_t S>
    void Pool<S>::FreePush(Page<S>* page) {
        page->mNextFree = mFreeHead;
        page->mPrevFree = nullptr;
        if (mFreeHead) mFreeHead->mPrevFree = page;
        mFreeHead = page;
    }

    template<size_t S>
    void Pool<S>::FreeRemove(Page<S>* page) {
        if (page->mPrevFree) page->mPrevFree->mNextFree = page->mNextFree;
        else                 mFreeHead = page->mNextFree;
        if (page->mNextFree) page->mNextFree->mPrevFree = page->mPrevFree;
        page->mNextFree = nullptr;
        page->mPrevFree = nullptr;
    }

    template<size_t S>
    void* Pool<S>::Alloc(size_t size) {
        if (!mFreeHead) {
            auto* page = new Page<S>();
            page->mPool = this;
            PagePush(page);
            FreePush(page);
        }

        auto* page = mFreeHead;
        auto& item = *page->mFree;
        Lock(item);
        item.mSize = size;
        return item.mData;
    }

    template<size_t S>
    void Pool<S>::Lock(Item<S>& item) {
        auto* page = item.GetPage();
        item.mPool = this;
        page->Lock(item);

        if (page->IsFull()) {
            FreeRemove(page);
        }
    }

    template<size_t S>
    void Pool<S>::Free(Item<S>& item) {
        assert(item.mPool == this);
        auto* page = item.GetPage();
        bool wasFull = page->IsFull();
        page->Free(item);

        if (page->IsFree()) {
//
//  Need to find who use dangled pointer for small allocation
//  When enable it we have a crash in a map transition
//  See:
//    m3d::Landscape::Release
//    m3d::Landscape::ReleaseOdeCollisionData
//
//            if (!wasFull) FreeRemove(page);
//            PageRemove(page);
//            delete page;
        } else if (wasFull) {
            FreePush(page);
        }
    }

    template<size_t S>
    void Pool<S>::Free(void* ptr) {
        Free(*reinterpret_cast<Item<S>*>(ptr));
    }
};

#endif
