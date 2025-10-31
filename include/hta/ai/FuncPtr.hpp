#pragma once

#include "ai/BaseFunction.hpp"
#include "stdafx.hpp"

namespace hta::ai {
    template <typename T>
    struct FuncPtr {
        /* Size=0x8 */
        /* 0x0000 */             // vtable pointer (4 bytes, implicit due to virtual destructor)
        /* 0x0004 */ T* m_pFunc; // 4 bytes pointer

        FuncPtr(T*);
        FuncPtr(const FuncPtr<T>&);
        const FuncPtr<T>& operator=(T*);
        virtual ~FuncPtr();
    };

    template <typename T, typename R>
    struct FuncPtrOneArg : public FuncPtr<BaseFunctionOneArg<T, R>> {
        /* Size=0x8 */
        /* 0x0000: fields for FuncPtr<BaseFunctionOneArg<T, R>> */

        FuncPtrOneArg(BaseFunctionOneArg<T, R>*);
        R operator()(T);
        const FuncPtrOneArg<T, R>& operator=(BaseFunctionOneArg<T, R>*);
        virtual ~FuncPtrOneArg();
    };

    template <typename T, typename R>
    struct FuncPtrOneArgRef : public FuncPtr<BaseFunctionOneArgRef<T, R>> {
        /* Size=0x8 */
        /* 0x0000: fields for FuncPtr<BaseFunctionOneArgRef<T, R>> */

        FuncPtrOneArgRef(BaseFunctionOneArgRef<T, R>*);
        R operator()(T&);
        const FuncPtrOneArgRef<T, R>& operator=(BaseFunctionOneArgRef<T, R>*);
        virtual ~FuncPtrOneArgRef();
    };

    template <typename T1, typename T2, typename R>
    struct FuncPtrTwoArgsRef : public FuncPtr<BaseFunctionTwoArgsRef<T1, T2, R>> {
        /* Size=0x8 */
        /* 0x0000: fields for FuncPtr<BaseFunctionTwoArgsRef<T1, T2, R>> */

        FuncPtrTwoArgsRef(BaseFunctionTwoArgsRef<T1, T2, R>*);
        R operator()(const T1&, T2&);
        const FuncPtrTwoArgsRef<T1, T2, R>& operator=(BaseFunctionTwoArgsRef<T1, T2, R>*);
        virtual ~FuncPtrTwoArgsRef();
    };
};