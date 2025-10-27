#ifndef HTA_CONTAINERS
#define HTA_CONTAINERS

#include <vector>
#include <map>
#include <set>

namespace hta {
    template <typename T>
    inline T clamp(T v, T min, T max) {
        if (v < min) return min;
        if (v > max) return max;
        return v;
    };

    template<typename T>
    struct vector {
        char pad[3];
        T* _Myfirst;
        T* _Mylast;
        T* _Myend;

        inline T* data() { return _Myfirst; }
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
    struct map : public std::map<K, T> {
    #if !defined(_DEBUG)
        char pad[4];
    #endif
    };

    template<typename K>
    struct set : public std::set<K> {
    #if !defined(_DEBUG)
        char pad[4];
    #endif
    };

    template<typename K>
    struct deque {
        char pad[4];
        float **_Map;
        unsigned int _Mapsize;
        unsigned int _Myoff;
        unsigned int _Mysize;
    };

    template<typename K>
    struct list {
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
};

#endif