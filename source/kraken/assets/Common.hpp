#pragma once
#ifndef KRAKEN_ASSETS_COMMON_HPP
#define KRAKEN_ASSETS_COMMON_HPP

#include <unordered_map>
#include <vector>
#include <span>

#include "common/string.hpp"

namespace kraken::assets {
    template<typename ASSET> struct Group;

    struct Asset {
    public:
        size_t        mRefCount { 0           };
        String        mLabel    {             };
        Group<Asset>* mOwner    { nullptr     };
    public:
        size_t        GetRef(void);
        size_t        AddRef(void);
        size_t        DecRef(void);
        Group<Asset>* GetOwner(void);
        void          SetOwner(Group<Asset>* owner);
    public:
        virtual ~Asset() = default;
    };

    struct Codec {
        virtual std::span<const String> Exts() const = 0;
        virtual Asset* Load(const String& path) = 0;
        virtual void   Save(const String& path, const Asset& asset) = 0;
        virtual ~Codec() = default;
    };

    template<typename ASSET>
    class Ref {
    private:
        ASSET* mData { nullptr };
    public:
        Ref() = default;

        Ref(ASSET* other) : mData(other) {
            if (mData) mData->AddRef();
        }

        Ref(Ref<ASSET>&& other) : mData(other.mData) {
            other.mData = nullptr;
        }

        Ref(const Ref<ASSET>& other) : mData(other.mData) {
            if (mData) mData->AddRef();
        }

        ~Ref() {
            if (mData) mData->DecRef();
        }
    public:
        Ref<ASSET>& operator = (ASSET* other) {
            if (mData != other) {
                if (mData) mData->DecRef();
                mData = other;
                if (mData) mData->AddRef();
            }
            return *this;
        }

        Ref<ASSET>& operator = (Ref<ASSET>&& other) {
            if (mData != other.mData) {
                if (mData) mData->DecRef();
                mData = other.mData;
                other.mData = nullptr;
            }
            return *this;
        }

        Ref<ASSET>& operator = (const Ref<ASSET>& other) {
            if (mData != other.mData) {
                if (mData) mData->DecRef();
                mData = other.mData;
                if (mData) mData->AddRef();
            }
            return *this;
        }
    public:
        ASSET& operator *  () { return *mData; }
        ASSET* operator -> () { return  mData; }
    public:
        operator bool() const { return mData != nullptr; }
    };

    template<typename ASSET>
    struct Group {
    private:
        std::unordered_map<String, Ref<ASSET>>  mItems;
        std::unordered_map<String, Codec*>      mCodecs;
    public:
        Ref<ASSET> Get(const String& name) {
            auto it = mItems.find(name);
            if (it != mItems.end())
                return it->second;
            return {};
        }

        void Attach(ASSET* asset) {
            asset->mOwner = reinterpret_cast<Group<Asset>*>(this);
            mItems[asset->mLabel] = asset;
        }

        void Detach(ASSET* asset) {
            asset->mOwner = nullptr;
            mItems.erase(asset->mLabel);
        }

        void Unload() {
            for (auto& [name, ref] : mItems) {
                ref->mOwner = nullptr;
            }
            mItems.clear();
        }

        size_t Size() const {
            return mItems.size();
        }

        void AttachCodec(Codec* codec) {
            for (auto& ext : codec->Exts()) {
                mCodecs[ext] = codec;
            }
        }

        void DetachCodec(Codec* codec) {
            for (auto& ext : codec->Exts()) {
                mCodecs.erase(ext);
            }
        }

        void ResetCodec() {
            mCodecs.clear();
        }

        Ref<ASSET> Load(const String& path) {
            auto ext = ExtOf(path);
            auto it = mCodecs.find(ext);
            if (it == mCodecs.end())
                return {};
            if (auto* asset = static_cast<ASSET*>(it->second->Load(path))) {
                Attach(asset);
                return Ref<ASSET>(asset);
            }
            return {};
        }

        void Save(const String& path) {
            auto ext = ExtOf(path);
            auto it = mCodecs.find(ext);
            if (it == mCodecs.end())
                return;
            if (auto ref = Get(path)) {
                it->second->Save(path, *ref);
            }
        }
    private:
        static String ExtOf(const String& path) {
            const char* str = path;
            const char* dot = nullptr;
            for (const char* p = str; *p; ++p) {
                if (*p == '.') dot = p;
            }
            return dot ? String(dot) : String();
        }
    };
};

#endif