#pragma once

#include "utils.hpp"
#include "hta/CStr.h"
#include "hta/CVector.h"

namespace m3d {
    namespace cmn {
        struct XmlFile;
        struct XmlNode;
    };

    struct AnimatedModel;

    struct RoadSet {
        /* Size=0x188 */
        /* 0x0000 */ public: CStr m_name;
        /* 0x000c */ public: stable_size_vector<AnimatedModel*> m_roadModels[4];
        /* 0x004c */ public: stable_size_vector<stable_size_vector<stable_size_vector<unsigned int>>> m_boundVerts[4];
        /* 0x008c */ public: stable_size_vector<stable_size_vector<stable_size_vector<unsigned int>>> m_fakeBoundVerts[4];
        /* 0x00cc */ public: stable_size_vector<stable_size_vector<stable_size_vector<unsigned int>>> m_cliffBorders[4];
        /* 0x010c */ public: float m_minX[4];
        /* 0x011c */ public: float m_minZ[4];
        /* 0x012c */ public: float m_maxX[4];
        /* 0x013c */ public: float m_maxZ[4];
        /* 0x014c */ public: float m_sizeZ[4];
        /* 0x015c */ public: float m_sizeX[4];
        /* 0x016c */ public: CVector m_scale;
        /* 0x0178 */ public: CStr m_wheeltraceTexName;
        /* 0x0184 */ public: int32_t m_soilType;
    
        void Clear();
        int32_t ReadFromXmlNode(cmn::XmlFile*, cmn::XmlNode*);
        RoadSet(const RoadSet&);
        RoadSet();
        ~RoadSet();
    };
};