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

template<typename K, typename V>
class stable_size_map;

template<typename K, typename V>
struct MapNode {
    MapNode* _Left;
    MapNode* _Parent;
    MapNode* _Right;

    struct Pair {
        const K first;
        V second;
    } _Myval;

    char _Color;
    char _Isnil;
    char _pad[2];
};

template<typename K, typename V>
class MapIterator {
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = typename MapNode<K, V>::Pair;
    using pointer = value_type*;
    using reference = value_type&;

    MapIterator(MapNode<K, V>* node = nullptr, MapNode<K, V>* head = nullptr)
        : node_(node), head_(head)
    {
    }

    reference operator*() const { return node_->_Myval; }
    pointer operator->() const { return &node_->_Myval; }

    bool operator==(const MapIterator& other) const { return node_ == other.node_; }
    bool operator!=(const MapIterator& other) const { return node_ != other.node_; }

    MapIterator& operator++() { node_ = increment(node_); return *this; }
    MapIterator operator++(int) { auto tmp = *this; ++(*this); return tmp; }

private:
    MapNode<K, V>* node_;
    MapNode<K, V>* head_;

    static MapNode<K, V>* increment(MapNode<K, V>* n)
    {
        if (!n || n->_Isnil) return nullptr;

        if (n->_Right && !n->_Right->_Isnil) {
            n = n->_Right;
            while (n->_Left && !n->_Left->_Isnil)
                n = n->_Left;
            return n;
        }
        else {
            auto* p = n->_Parent;
            while (p && !p->_Isnil && n == p->_Right) {
                n = p;
                p = p->_Parent;
            }
            return p;
        }
    }

    friend class stable_size_map<K, V>;
};

template<typename K, typename V>
struct stable_size_map {
    using iterator = MapIterator<K, V>;
    using const_iterator = MapIterator<K, V>;

    stable_size_map() = default;
    explicit stable_size_map(MapNode<K, V>* head, unsigned int size = 0)
        : _Myhead(head), _Mysize(size)
    {
    }

    iterator begin()
    {
        if (!_Myhead) return end();
        auto* node = _Myhead->_Parent;
        while (node && node->_Left && !node->_Left->_Isnil)
            node = node->_Left;
        return iterator(node, _Myhead);
    }

    iterator end() { return iterator(_Myhead, _Myhead); }

    size_t size() const { return _Mysize; }
    bool empty() const { return _Mysize == 0; }

private:
    void* _;
    MapNode<K, V>* _Myhead;
    unsigned int _Mysize;
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

inline int RoughSign(float value)
{
	if (value > 0.000001)
		return 1;
	if (value >= -0.000001)
		return 0;
	return -1;
}

#endif