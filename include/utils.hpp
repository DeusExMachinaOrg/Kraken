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
struct stable_size_vector : public std::vector<T> {
#if !defined(_DEBUG)
    char pad[4];
#endif
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

#endif