#include <entity/Entity.hpp>

#include <stack>
#include <queue>

namespace kraken::entity {
    eFlags operator & (eFlags l, eFlags r) {
        return static_cast<eFlags>(static_cast<uint64_t>(l) & static_cast<uint64_t>(r));
    }
    eFlags operator | (eFlags l, eFlags r) {
        return static_cast<eFlags>(static_cast<uint64_t>(l) | static_cast<uint64_t>(r));
    }
    eFlags operator ^ (eFlags l, eFlags r) {
        return static_cast<eFlags>(static_cast<uint64_t>(l) ^ static_cast<uint64_t>(r));
    }
    bool operator == (eFlags l, eFlags r) {
        return (static_cast<uint64_t>(l) & static_cast<uint64_t>(r)) == static_cast<uint64_t>(r);
    }
    bool operator != (eFlags l, eFlags r) {
        return (static_cast<uint64_t>(l) & static_cast<uint64_t>(r)) != static_cast<uint64_t>(r);
    }

    Entity::Entity() {
    };

    Entity::~Entity() {
        // unlink from parent
        if (mTreeBase) {
            mTreeBase->DelObject(this);
        }
        // delete all children
        auto* child = mTreeHead;
        while (child) {
            auto* next = child->mTreeNext;
            child->mTreeBase = nullptr;
            delete child;
            child = next;
        }
    };

    void Entity::AddObject(Entity* child) {
        if (!child || child == this || child->mTreeBase == this) return;

        // remove from previous parent
        if (child->mTreeBase)
            child->mTreeBase->DelObject(child);

        child->mTreeBase = this;
        child->mTreeRoot = mTreeRoot;
        child->mTreeNext = nullptr;
        child->mTreePrev = mTreeTail;

        if (mTreeTail)
            mTreeTail->mTreeNext = child;
        else
            mTreeHead = child;

        mTreeTail = child;
        mTreeSize++;
    };

    bool Entity::HasObject(Entity* child) {
        if (!child) return false;
        auto* node = mTreeHead;
        while (node) {
            if (node == child) return true;
            node = node->mTreeNext;
        }
        return false;
    };

    void Entity::DelObject(Entity* child) {
        if (!child || child->mTreeBase != this) return;

        if (child->mTreePrev)
            child->mTreePrev->mTreeNext = child->mTreeNext;
        else
            mTreeHead = child->mTreeNext;

        if (child->mTreeNext)
            child->mTreeNext->mTreePrev = child->mTreePrev;
        else
            mTreeTail = child->mTreePrev;

        child->mTreeBase = nullptr;
        child->mTreeRoot = nullptr;
        child->mTreeNext = nullptr;
        child->mTreePrev = nullptr;
        mTreeSize--;
    };

    void Entity::Traverse(eTraverse mode, eOrder order, Visitor fn) {
        switch (mode) {
            case eTraverse::DFS: {
                std::stack<Entity*> stack;
                stack.push(this);
                while (!stack.empty()) {
                    auto* node = stack.top();
                    stack.pop();
                    if (!fn(*node)) return;
                    if (order == eOrder::Forward) {
                        for (auto* c = node->mTreeTail; c; c = c->mTreePrev)
                            stack.push(c);
                    } else {
                        for (auto* c = node->mTreeHead; c; c = c->mTreeNext)
                            stack.push(c);
                    }
                }
            } break;

            case eTraverse::BFS: {
                std::queue<Entity*> queue;
                queue.push(this);
                while (!queue.empty()) {
                    auto* node = queue.front();
                    queue.pop();
                    if (!fn(*node)) return;
                    if (order == eOrder::Forward) {
                        for (auto* c = node->mTreeHead; c; c = c->mTreeNext)
                            queue.push(c);
                    } else {
                        for (auto* c = node->mTreeTail; c; c = c->mTreePrev)
                            queue.push(c);
                    }
                }
            } break;

            case eTraverse::Bubble: {
                if (order == eOrder::Forward) {
                    for (auto* node = this; node; node = node->mTreeBase)
                        if (!fn(*node)) return;
                } else {
                    std::stack<Entity*> path;
                    for (auto* node = this; node; node = node->mTreeBase)
                        path.push(node);
                    while (!path.empty()) {
                        if (!fn(*path.top())) return;
                        path.pop();
                    }
                }
            } break;

            case eTraverse::Children: {
                if (order == eOrder::Forward) {
                    for (auto* c = mTreeHead; c; c = c->mTreeNext)
                        if (!fn(*c)) return;
                } else {
                    for (auto* c = mTreeTail; c; c = c->mTreePrev)
                        if (!fn(*c)) return;
                }
            } break;
        }
    };
};
