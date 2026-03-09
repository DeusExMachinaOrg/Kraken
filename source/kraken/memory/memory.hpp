#pragma once
#ifndef KRAKEN_MEMORY_MEMORY_HPP
#define KRAKEN_MEMORY_MEMORY_HPP

#include "memory/shared.hpp"
#include "memory/pool.hpp"

namespace kraken::memory {
    class Memory {
    public:
        static Memory& Instance(void);
    public:
        size_t      mOverhead  { 0 };
        size_t      mAllocated { 0 };
        size_t      mUsed      { 0 };
        std::mutex  mMutex     {   };
        Pool<0x010> mPool_010  {   };
        Pool<0x020> mPool_020  {   };
        Pool<0x030> mPool_030  {   };
        Pool<0x040> mPool_040  {   };
        Pool<0x080> mPool_080  {   };
        Pool<0x100> mPool_100  {   };
        Pool<0x200> mPool_200  {   };
        Pool<0x300> mPool_300  {   };
        Pool<0x400> mPool_400  {   };
        Pool<0x800> mPool_800  {   };
    public:
        Memory();
       ~Memory();
    public:
        void* Malloc(size_t size);
        void* Realloc(void* data, size_t size);
        void  Free(void* data);
    public:
        void* operator new    (size_t size);
        void  operator delete (void*  item);
    public:
        Memory(Memory&&) = delete;
        Memory(const Memory&) = delete;
        Memory& operator = (Memory&&) = delete;
        Memory& operator = (const Memory&) = delete;
    };
};

void* operator new    (size_t size);
void  operator delete (void*  data);

#endif
