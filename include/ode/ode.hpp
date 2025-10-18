#ifndef ODE_HPP
#define ODE_HPP

#include <stdint.h>

struct dMass;
struct dJointFeedback;
struct dxBody;
struct dObject;
struct dxWorld;
struct dxGeom;
struct dxSpace;
struct dxJointNode;
struct dxJoint;
struct dxJointBreakInfo;

struct dBody { /* Size=0x4 */
/* 0x0000 */ dxBody* _id;
};

struct dContactGeom { /* Size=0x2c */
/* 0x0000 */ public: float pos[4];
/* 0x0010 */ public: float normal[4];
/* 0x0020 */ public: float depth;
/* 0x0024 */ public: dxGeom* g1;
/* 0x0028 */ public: dxGeom* g2;
};

struct dSurfaceParameters { /* Size=0x2c */
/* 0x0000 */ public: int32_t mode;
/* 0x0004 */ public: float mu;
/* 0x0008 */ public: float mu2;
/* 0x000c */ public: float bounce;
/* 0x0010 */ public: float bounce_vel;
/* 0x0014 */ public: float soft_erp;
/* 0x0018 */ public: float soft_cfm;
/* 0x001c */ public: float motion1;
/* 0x0020 */ public: float motion2;
/* 0x0024 */ public: float slip1;
/* 0x0028 */ public: float slip2;
};

struct dContact { /* Size=0x68 */
/* 0x0000 */ public: dSurfaceParameters surface;
/* 0x002c */ public: dContactGeom geom;
/* 0x0058 */ public: float fdir1[4];
};

struct dBase {
    /* Size=0x1 */
};


struct dxTriMeshData; // IceMaths / ODE Unknown

const auto dBodyGetAngularVel   = (float* (__fastcall*)(dxBody*))(0x007C4760);
const auto dGeomSphereGetRadius = (double (__fastcall*)(dxGeom*))(0x00751860);

#endif