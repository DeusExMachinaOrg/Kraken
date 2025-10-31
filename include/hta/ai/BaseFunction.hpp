#pragma once

namespace hta::ai {
    struct BaseFunction {
        /* Size=0x4 */

        virtual ~BaseFunction();
        BaseFunction(const BaseFunction&);
        BaseFunction();
    };

    static_assert(sizeof(BaseFunction) == 0x4);
    template <typename T, typename R> struct BaseFunctionOneArg : public BaseFunction {
        virtual R Execute(T);
        BaseFunctionOneArg(const BaseFunctionOneArg<T, R>&);
        BaseFunctionOneArg();
        virtual ~BaseFunctionOneArg();
    };

    template <typename T, typename R> struct BaseFunctionOneArgRef : public BaseFunction {
        virtual R Execute(T&);
        BaseFunctionOneArgRef(const BaseFunctionOneArgRef<T, R>&);
        BaseFunctionOneArgRef();
        virtual ~BaseFunctionOneArgRef();
    };

    template <typename T1, typename T2, typename R> struct BaseFunctionTwoArgsRef : public BaseFunction {
        virtual R Execute(const T1&, T2&);
        BaseFunctionTwoArgsRef(const BaseFunctionTwoArgsRef<T1, T2, R>&);
        BaseFunctionTwoArgsRef();
        virtual ~BaseFunctionTwoArgsRef();
    };
};