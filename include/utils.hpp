#ifndef __KRAKEN_UTILS_HPP__
#define __KRAKEN_UTILS_HPP__

#include <vector>
#include <map>
#include <set>

template <typename T>
inline T clamp(T v, T min, T max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
};

template<typename T>
struct stable_size_vector {
    char pad[3];
    T* _Myfirst;
    T* _Mylast;
    T* _Myend;

    inline size_t size() const { return static_cast<size_t>(_Mylast - _Myfirst); }
    inline bool empty() const { return _Myfirst == _Mylast; }
    inline T& back() { return _Mylast[-1]; }
    inline T& front() { return *_Myfirst; }

    using iterator = T*;
    using const_iterator = const T*;

    inline iterator begin() noexcept { return _Myfirst; }
    inline const_iterator begin() const noexcept { return _Myfirst; }
    inline const_iterator cbegin() const noexcept { return _Myfirst; }

    inline iterator end() noexcept { return _Mylast; }
    inline const_iterator end() const noexcept { return _Mylast; }
    inline const_iterator cend() const noexcept { return _Mylast; }
};

template<typename K, typename T>
struct stable_size_map : public std::map<K, T> {
#if !defined(_DEBUG)
    char pad[4];
#endif
};

template<typename K>
struct stable_size_set : public std::set<K> {
#if !defined(_DEBUG)
    char pad[4];
#endif
};

template <class Out, class In>
Out unsafe_cast(In x) {
    union {
        In a;
        Out b;
    };
    a = x;
    return b;
};

#endif