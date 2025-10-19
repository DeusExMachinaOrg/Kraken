#pragma once

#include "hta/m3d/Object.h"

#include <list>

namespace m3d {
    class ObjectsContainer {
        /* Size=0x300 */
        /* 0x0000 */ stable_size_list<Object*> m_objectsByClassIdx[64];

        void AddObject(Object*);
        void RemoveObject(Object*);
        std::list<Object*>* GetObjectsByClass(Class*);
        const std::list<Object*>* GetObjects() const;
        std::list<Object*>* GetObjects();
        bool empty() const;
        ObjectsContainer(const ObjectsContainer&);
        ObjectsContainer();
        ~ObjectsContainer();
    };
};