#pragma once

#include "ode/ode.hpp"
#include "hta/CVector.h"
#include "hta/Quaternion.h"
#include "hta/m3d/Class.h"
#include "hta/m3d/Object.h"

namespace m3d {
    class GeomObject : public Object {
        /* Size=0x7c */
        /* 0x0000: fields for Object */
        /* 0x0034 */ CVector m_translation;
        /* 0x0040 */ Quaternion m_rotation;
        /* 0x0050 */ dxTriMeshData* m_TriData;
        /* 0x0054 */ CVector* m_Vertices;
        /* 0x0058 */ int32_t* m_Indices;
        /* 0x005c */ bool m_needToDeleteInUnlink;
        /* 0x0060 */ dxGeom* m_geom;
        /* 0x0064 */ bool m_bMayBeEnabled;
        /* 0x0068 */ PointBase<int> m_startCell;
        /* 0x0070 */ PointBase<int> m_endCell;
        /* 0x0078 */ int32_t m_enabledCellsCount;

        static Class m_classGeomObject;

        GeomObject();
        GeomObject(const GeomObject&);
        virtual ~GeomObject();
        virtual Object* Clone();
        virtual Class* GetClass() const;
        void Release();
        void IncEnabledCellsCount();
        void DecEnabledCellsCount();
        void SetGeom(dxGeom*);
        dxGeom* GetGeom() const;
        void SetEnabled(bool);
        void SetMayBeEnabled(bool);
        void SetBounds(const PointBase<int>&, const PointBase<int>&);
        const PointBase<int>& GetStartCell();
        const PointBase<int>& GetEndCell();

        static Object* CreateObject();
        static Class* GetBaseClass();
    };
};