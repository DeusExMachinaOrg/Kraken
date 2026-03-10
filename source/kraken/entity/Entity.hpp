#pragma once
#ifndef KRAKEN_ENTITY_ENTITY_HPP
#define KRAKEN_ENTITY_ENTITY_HPP

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "common/string.hpp"

namespace kraken::entity {
    struct Entity;
    struct Scene;

    enum class eTraverse : uint8_t {
        DFS,       // depth-first (subtree)
        BFS,       // breadth-first (level-order)
        Bubble,    // walk up: self -> parent -> root
        Children,  // direct children only (no descendants)
    };

    enum class eOrder : uint8_t {
        Forward,  // natural order
        Reverse,  // reversed
    };

    enum class eFlags : uint64_t {
        EMPTY  = 0,
        LINKED = 1,
        STATIC = 2,
        DIRTY  = 4,
    };

    eFlags operator &  (eFlags l, eFlags r);
    eFlags operator |  (eFlags l, eFlags r);
    eFlags operator ^  (eFlags l, eFlags r);
    bool   operator == (eFlags l, eFlags r);
    bool   operator != (eFlags l, eFlags r);

    using Groups  = uint64_t;
    using Visitor = std::function<bool(struct Entity&)>;

    struct Entity {
        friend struct Scene;
    protected:
        String  mLabel    { ""            };
        Groups  mGroups   {               };
        eFlags  mFlags    { eFlags::EMPTY };
        Scene*  mTreeRoot { nullptr       };
        Entity* mTreeBase { nullptr       };
        Entity* mTreeHead { nullptr       };
        Entity* mTreeTail { nullptr       };
        Entity* mTreeNext { nullptr       };
        Entity* mTreePrev { nullptr       };
        size_t  mTreeSize { 0             };
    public:
        Entity();
        virtual ~Entity();
    public:
        void AddObject(Entity* child);
        bool HasObject(Entity* child);
        void DelObject(Entity* child);
    public:
        void Traverse(eTraverse mode, eOrder order, Visitor fn);
    public:
        Entity(Entity&&) = delete;
        Entity(const Entity&) = delete;
    public:
        Entity& operator=(Entity&&) = delete;
        Entity& operator=(const Entity&) = delete;
    };
};

#endif
