#pragma once

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
}

namespace ai {
    enum GeomType : int32_t {
        GEOM_TYPE_NONE = 0x0000,
        GEOM_TYPE_BOX = 0x0001,
        GEOM_TYPE_SPHERE = 0x0002,
        GEOM_TYPE_CYLINDER = 0x0003,
        GEOM_TYPE_RAY = 0x0004,
        GEOM_TYPE_TRIMESH = 0x0005,
        GEOM_TYPE_FROM_MODEL = 0x0006,
    };


    struct Geom {
        struct CellAabb {
            /* Size=0x10 */
            /* 0x0000 */ public: int32_t x0;
            /* 0x0004 */ public: int32_t z0;
            /* 0x0008 */ public: int32_t x1;
            /* 0x000c */ public: int32_t z1;

            CellAabb();
            ai::Geom::CellAabb& operator+=(const ai::Geom::CellAabb&);
        };

        /* Size=0x18 */
        /* 0x0004 */ protected: dxGeom* m_geomId;
        /* 0x0008 */ private: ai::Geom::CellAabb m_curAabb;

        Geom(dxGeom*, void (*)(dxGeom*));
        Geom(const ai::Geom&);
        virtual ~Geom();
        dxGeom* GetGeomId() const;
        CVector GetPosition() const;
        Quaternion GetRotation() const;
        void SetPosition(const CVector&);
        void SetRotation(const Quaternion&);
        void SetDirection(const CVector&);
        void* GetData() const;
        void SetData(void*);
        void SetBody(dxBody*);
        void UnlinkFromBody();
        dxSpace* GetSpace() const;
        void RelinkToSpace(dxSpace*);
        void Enable();
        void Disable();
        bool IsEnabled() const;
        Aabb GetAabb() const;
        int32_t GetGeomClass() const;
        ai::Geom::CellAabb CountCellAabb() const;
        void LinkToCollisionCells(int32_t, ai::Geom::CellAabb*);
        void UnlinkFromCollisionCells(int32_t);
        void RelinkToCollisionCells(int32_t);
        void CheckCollisionCells();
        ai::Geom::CellAabb GetCollisionCellAabb() const;
        virtual void DumpPhysicInfo(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
    };
};