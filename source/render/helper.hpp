#pragma once

#include <unordered_map>
#include <unordered_set>

namespace kraken::render {
    template<typename T> using ViewPtr = T*;

    template <typename T> class Registry {
      private:
        std::unordered_map<int32_t, T*> mItems{};
        std::unordered_set<int32_t> mSlots{};

      public:
        using iterator       = typename std::unordered_map<int32_t, T*>::iterator;
        using const_iterator = typename std::unordered_map<int32_t, T*>::const_iterator;
      public:
        int32_t GetSlot() const {
            if (mSlots.size())
                return *mSlots.begin();
            return mItems.size();
        }
        void AddItem(int32_t key, T* val) {
            mSlots.erase(key);
            mItems[key] = val;
        }
        void DelItem(int32_t key) {
            mItems.erase(key);
            mSlots.insert(key);
        }
        T* GetItem(int32_t key) const {
            auto iter = mItems.find(key);
            if (iter != mItems.end())
                return iter->second;
            return nullptr;
        }
        int32_t PushItem(T* item) {
            int32_t slot = this->GetSlot();
            this->AddItem(slot, item);
            return slot;
        }
        void Reset() {
            this->mItems.clear();
            this->mSlots.clear();
        }
        iterator begin() {
            return mItems.begin();
        }
        iterator end() {
            return mItems.end();
        }
        const_iterator begin() const {
            return mItems.begin();
        }
        const_iterator end() const {
            return mItems.end();
        }
        const_iterator cbegin() const {
            return mItems.cbegin();
        }
        const_iterator cend() const {
            return mItems.cend();
        }
        size_t size() const {
            return mItems.size();
        }
        bool empty() const {
            return mItems.empty();
        }
    };
};