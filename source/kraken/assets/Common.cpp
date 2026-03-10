#include "assets/Common.hpp"

namespace kraken::assets {
    size_t Asset::GetRef(void) {
        return mRefCount;
    }

    size_t Asset::AddRef(void) {
        return ++mRefCount;
    }

    size_t Asset::DecRef(void) {
        if (mRefCount > 1)
            return --mRefCount;
        delete this;
        return 0;
    }

    Group<Asset>* Asset::GetOwner(void) {
        return mOwner;
    }

    void Asset::SetOwner(Group<Asset>* owner) {
        mOwner = owner;
    }
}