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
    T& operator[](size_t index) {
        return _Myfirst[index];
    }

    const T& operator[](size_t index) const {
        return _Myfirst[index];
    }
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

template<typename K>
struct stable_size_deque {
    char pad[4];
    float **_Map;
    unsigned int _Mapsize;
    unsigned int _Myoff;
    unsigned int _Mysize;
};

template<typename K>
struct stable_size_list {
    char pad[4];
    void* _Myhead;
    unsigned int _Mysize;
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