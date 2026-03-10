#pragma once
#ifndef KRAKEN_ENTITY_SCENE_HPP
#define KRAKEN_ENTITY_SCENE_HPP

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "common/string.hpp"
#include "entity/entity.hpp"

namespace kraken::entity {
    struct Define;
    struct Entity;
    struct Scene;

    struct Weather;

    struct Scene {
    public:
        using GroupIndex = std::unordered_map<Groups, std::unordered_set<Entity*>>;
        using LabelIndex = std::unordered_map<String, Entity*>;
    public:
        Entity*    mRoot        { nullptr };
        Weather*   mWeather     { nullptr };
        GroupIndex mGroupIndex  {         };
        LabelIndex mLabelIndex  {         };
    public:
        Scene();
       ~Scene();
    public:
        void    Attach(Entity* entity);
        void    Detach(Entity* entity);
    public:
        Entity* FindByLabel(const String& label);
        void    FindByGroup(Groups mask, Visitor fn);
    public:
        Scene(Scene&&) = delete;
        Scene(const Scene&) = delete;
        Scene& operator=(Scene&&) = delete;
        Scene& operator=(const Scene&) = delete;
    };
};

#endif
