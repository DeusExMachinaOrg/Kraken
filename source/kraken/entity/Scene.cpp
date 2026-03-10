#include <entity/Scene.hpp>

#include <stack>
#include <queue>

namespace kraken::entity {
    Scene::Scene() {
    };

    Scene::~Scene() {
        delete mRoot;
    };

    void Scene::Attach(Entity* entity) {
        if (!entity) return;
        entity->mTreeRoot = this;
        if (entity->mGroups) {
            mGroupIndex[entity->mGroups].insert(entity);
        }
        if (entity->mLabel != "") {
            mLabelIndex[entity->mLabel] = entity;
        }
    };

    void Scene::Detach(Entity* entity) {
        if (!entity) return;
        if (entity->mGroups) {
            auto it = mGroupIndex.find(entity->mGroups);
            if (it != mGroupIndex.end()) {
                it->second.erase(entity);
                if (it->second.empty())
                    mGroupIndex.erase(it);
            }
        }
        if (entity->mLabel != "") {
            mLabelIndex.erase(entity->mLabel);
        }
        entity->mTreeRoot = nullptr;
    };

    Entity* Scene::FindByLabel(const String& label) {
        auto it = mLabelIndex.find(label);
        if (it != mLabelIndex.end())
            return it->second;
        return nullptr;
    };

    void Scene::FindByGroup(Groups mask, Visitor fn) {
        for (auto& [group, entities] : mGroupIndex) {
            if (group & mask) {
                for (auto* entity : entities) {
                    if (!fn(*entity)) return;
                }
            }
        }
    };
};
