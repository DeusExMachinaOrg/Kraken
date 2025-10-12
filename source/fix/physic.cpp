#include "float.h"

#include <stdint.h>
#include <smmintrin.h>
#include <immintrin.h>

#include "routines.hpp"
#include "fix/physic.hpp"

namespace kraken::fix::physic {
    #define RIGHT_SHIFT(T, base, offset) ((T*)(reinterpret_cast<char*>(base) + offset))

    struct dxQuickStepParameters;
    struct dxContactParameters;
    struct dxWorld;
    struct dObject;
    struct dxAutoDisable;
    struct dxDamping;
    struct dMass;
    struct dxGeom;
    struct dxBody;
    struct dxJointInfo1;
    struct dxJointInfo2;
    struct dxJointNode;
    struct dJointFeedback;
    struct dxJointVtable;
    struct dxJointBreakInfo;
    struct dxJoint;
    struct dxJointGroup;
    struct dObStack;
    struct dObStackArena;
    typedef dxJointGroup* dJointGroupID;

    #define dDOTpq(a,b,p,q) ((a)[0]*(b)[0] + (a)[p]*(b)[q] + (a)[2*(p)]*(b)[2*(q)])

    inline float dDOT   (const float *a, const float *b) { return dDOTpq(a,b,1,1); }
    inline float dDOT13 (const float *a, const float *b) { return dDOTpq(a,b,1,3); }
    inline float dDOT31 (const float *a, const float *b) { return dDOTpq(a,b,3,1); }
    inline float dDOT33 (const float *a, const float *b) { return dDOTpq(a,b,3,3); }
    inline float dDOT14 (const float *a, const float *b) { return dDOTpq(a,b,1,4); }
    inline float dDOT41 (const float *a, const float *b) { return dDOTpq(a,b,4,1); }
    inline float dDOT44 (const float *a, const float *b) { return dDOTpq(a,b,4,4); }

    #define dPAD(a) (((a) > 1) ? ((((a)-1)|3)+1) : (a))

    #define dCROSS(a,op,b,c) \
        (a)[0] op ((b)[1]*(c)[2] - (b)[2]*(c)[1]); \
        (a)[1] op ((b)[2]*(c)[0] - (b)[0]*(c)[2]); \
        (a)[2] op ((b)[0]*(c)[1] - (b)[1]*(c)[0]);

    enum {
        // if this flag is set, the joint wil break
        dJOINT_BROKEN =             0x0001,
        // if this flag is set, the joint wil be deleted when it breaks
        dJOINT_DELETE_ON_BREAK =    0x0002,
        // if this flag is set, the joint can break at a certain force on body 1
        dJOINT_BREAK_AT_B1_FORCE =  0x0004,
        // if this flag is set, the joint can break at a certain torque on body 1
        dJOINT_BREAK_AT_B1_TORQUE = 0x0008,
        // if this flag is set, the joint can break at a certain force on body 2
        dJOINT_BREAK_AT_B2_FORCE =  0x0010,
        // if this flag is set, the joint can break at a certain torque on body 2
        dJOINT_BREAK_AT_B2_TORQUE = 0x0020
    };

    #define dMULTIPLYOP0_331(A,op,B,C) \
        (A)[0] op dDOT((B),(C)); \
        (A)[1] op dDOT((B+4),(C)); \
        (A)[2] op dDOT((B+8),(C));
    #define dMULTIPLYOP1_331(A,op,B,C) \
        (A)[0] op dDOT41((B),(C)); \
        (A)[1] op dDOT41((B+1),(C)); \
        (A)[2] op dDOT41((B+2),(C));
    #define dMULTIPLYOP0_133(A,op,B,C) \
        (A)[0] op dDOT14((B),(C)); \
        (A)[1] op dDOT14((B),(C+1)); \
        (A)[2] op dDOT14((B),(C+2));
    #define dMULTIPLYOP0_333(A,op,B,C) \
        (A)[0] op dDOT14((B),(C)); \
        (A)[1] op dDOT14((B),(C+1)); \
        (A)[2] op dDOT14((B),(C+2)); \
        (A)[4] op dDOT14((B+4),(C)); \
        (A)[5] op dDOT14((B+4),(C+1)); \
        (A)[6] op dDOT14((B+4),(C+2)); \
        (A)[8] op dDOT14((B+8),(C)); \
        (A)[9] op dDOT14((B+8),(C+1)); \
        (A)[10] op dDOT14((B+8),(C+2));
    #define dMULTIPLYOP1_333(A,op,B,C) \
        (A)[0] op dDOT44((B),(C)); \
        (A)[1] op dDOT44((B),(C+1)); \
        (A)[2] op dDOT44((B),(C+2)); \
        (A)[4] op dDOT44((B+1),(C)); \
        (A)[5] op dDOT44((B+1),(C+1)); \
        (A)[6] op dDOT44((B+1),(C+2)); \
        (A)[8] op dDOT44((B+2),(C)); \
        (A)[9] op dDOT44((B+2),(C+1)); \
        (A)[10] op dDOT44((B+2),(C+2));
    #define dMULTIPLYOP2_333(A,op,B,C) \
        (A)[0] op dDOT((B),(C)); \
        (A)[1] op dDOT((B),(C+4)); \
        (A)[2] op dDOT((B),(C+8)); \
        (A)[4] op dDOT((B+4),(C)); \
        (A)[5] op dDOT((B+4),(C+4)); \
        (A)[6] op dDOT((B+4),(C+8)); \
        (A)[8] op dDOT((B+8),(C)); \
        (A)[9] op dDOT((B+8),(C+4)); \
        (A)[10] op dDOT((B+8),(C+8));

    #define DECL template <class TA, class TB, class TC> inline void

    DECL dMULTIPLY0_331(TA *A, const TB *B, const TC *C) { dMULTIPLYOP0_331(A,=,B,C) }
    DECL dMULTIPLY1_331(TA *A, const TB *B, const TC *C) { dMULTIPLYOP1_331(A,=,B,C) }
    DECL dMULTIPLY0_133(TA *A, const TB *B, const TC *C) { dMULTIPLYOP0_133(A,=,B,C) }
    DECL dMULTIPLY0_333(TA *A, const TB *B, const TC *C) { dMULTIPLYOP0_333(A,=,B,C) }
    DECL dMULTIPLY1_333(TA *A, const TB *B, const TC *C) { dMULTIPLYOP1_333(A,=,B,C) }
    DECL dMULTIPLY2_333(TA *A, const TB *B, const TC *C) { dMULTIPLYOP2_333(A,=,B,C) }

    DECL dMULTIPLYADD0_331(TA *A, const TB *B, const TC *C) { dMULTIPLYOP0_331(A,+=,B,C) }
    DECL dMULTIPLYADD1_331(TA *A, const TB *B, const TC *C) { dMULTIPLYOP1_331(A,+=,B,C) }
    DECL dMULTIPLYADD0_133(TA *A, const TB *B, const TC *C) { dMULTIPLYOP0_133(A,+=,B,C) }
    DECL dMULTIPLYADD0_333(TA *A, const TB *B, const TC *C) { dMULTIPLYOP0_333(A,+=,B,C) }
    DECL dMULTIPLYADD1_333(TA *A, const TB *B, const TC *C) { dMULTIPLYOP1_333(A,+=,B,C) }
    DECL dMULTIPLYADD2_333(TA *A, const TB *B, const TC *C) { dMULTIPLYOP2_333(A,+=,B,C) }

    void dSolveLCP (int n, float *A, float *x, float *b, float *w, int nub, float *lo, float *hi, int *findex);

    typedef void (__fastcall* PFNdxStepBody) (dxBody *b, float h);
    PFNdxStepBody dxStepBody = (PFNdxStepBody) 0x008FC8F0;

    void MultiplyAdd1_8q1(float *A, float *B, float *C, int q) {
        int k;
        float sum;
        sum = 0;
        for (k=0; k<q; k++) sum += B[k*8] * C[k];
        A[0] += sum;
        sum = 0;
        for (k=0; k<q; k++) sum += B[1+k*8] * C[k];
        A[1] += sum;
        sum = 0;
        for (k=0; k<q; k++) sum += B[2+k*8] * C[k];
        A[2] += sum;
        sum = 0;
        for (k=0; k<q; k++) sum += B[4+k*8] * C[k];
        A[4] += sum;
        sum = 0;
        for (k=0; k<q; k++) sum += B[5+k*8] * C[k];
        A[5] += sum;
        sum = 0;
        for (k=0; k<q; k++) sum += B[6+k*8] * C[k];
        A[6] += sum;
    }

    void MultiplyAdd2_p8r(float *A, float *B, float *C, int p, int r, int Askip) {
        int i,j;
        float sum,*bb,*cc;
        bb = B;
        for (i=p; i; i--) {
            cc = C;
            for (j=r; j; j--) {
                sum = bb[0]*cc[0];
                sum += bb[1]*cc[1];
                sum += bb[2]*cc[2];
                sum += bb[4]*cc[4];
                sum += bb[5]*cc[5];
                sum += bb[6]*cc[6];
                *(A++) += sum;
                cc += 8;
            }
            A += Askip - r;
            bb += 8;
        }
    }

    void Multiply2_p8r(float *A, float *B, float *C, int p, int r, int Askip) {
        int i,j;
        float sum,*bb,*cc;
        bb = B;
        for (i=p; i; i--) {
            cc = C;
            for (j=r; j; j--) {
                sum = bb[0]*cc[0];
                sum += bb[1]*cc[1];
                sum += bb[2]*cc[2];
                sum += bb[4]*cc[4];
                sum += bb[5]*cc[5];
                sum += bb[6]*cc[6];
                *(A++) = sum; 
                cc += 8;
            }
            A += Askip - r;
            bb += 8;
        }
    }

    void Multiply0_p81(float *A, float *B, float *C, int p) {
        int i;
        float sum;
        for (i=p; i; i--) {
            sum =  B[0]*C[0];
            sum += B[1]*C[1];
            sum += B[2]*C[2];
            sum += B[4]*C[4];
            sum += B[5]*C[5];
            sum += B[6]*C[6];
            *(A++) = sum;
            B += 8;
        }
    }

    void MultiplyAdd0_p81(float *A, float *B, float *C, int p) {
        int i;
        float sum;
        for (i=p; i; i--) {
            sum =  B[0]*C[0];
            sum += B[1]*C[1];
            sum += B[2]*C[2];
            sum += B[4]*C[4];
            sum += B[5]*C[5];
            sum += B[6]*C[6];
            *(A++) += sum;
            B += 8;
        }
    }

    static void Multiply1_8q1(float *A, float *B, float *C, int q) {
        int k;
        float sum;
        sum = 0;
        for (k=0; k<q; k++)
             sum += B[k*8] * C[k];
        A[0] = sum;
        sum = 0;
        for (k=0; k<q; k++)
             sum += B[1+k*8] * C[k];
        A[1] = sum;
        sum = 0;
        for (k=0; k<q; k++)
             sum += B[2+k*8] * C[k];
        A[2] = sum;
        sum = 0;
        for (k=0; k<q; k++)
             sum += B[4+k*8] * C[k];
        A[4] = sum;
        sum = 0;
        for (k=0; k<q; k++)
             sum += B[5+k*8] * C[k];
        A[5] = sum;
        sum = 0;
        for (k=0; k<q; k++)
             sum += B[6+k*8] * C[k];
        A[6] = sum;
    }

    template <typename T>
    struct HeapArray
    {
        T *ptr          = nullptr;
        DWORD* checksum = nullptr;
        size_t size     = 0;
        size_t count    = 0;
        inline HeapArray() {};
        inline HeapArray(HeapArray& array) {
            this->ptr      = array.ptr;
            this->checksum = array.checksum;
            this->size     = array.size;
            this->count    = array.count;
        };
        inline HeapArray(size_t count) {
            this->count = count;
            this->size = sizeof(T) * count + sizeof(DWORD);
            if (this->size & 0xF)
                this->size = ((this->size >> 4) << 4) + 0x10;

            this->ptr = (T *)malloc(this->size);
            if (!this->ptr)
                DebugBreak();

            this->checksum = RIGHT_SHIFT(DWORD, this->ptr, this->size - sizeof(DWORD));
            *this->checksum = 0xDEADBEEF;
        };
        inline ~HeapArray() {
            if (*this->checksum != 0xDEADBEEF)
                DebugBreak();
            free(this->ptr);
        };
        inline void SetZero() {
            memset(this->ptr, 0, this->size - sizeof(DWORD));
        };
        inline void SetValue(T value) {
            for (size_t i = 0; i < this->count; i++)
                this->ptr[i] = value;
        };
        inline void Check() {
            if (*this->checksum != 0xDEADBEEF)
                DebugBreak();
        }
    };

    struct dObStackArena {
        dObStackArena* next;
        int            used;
    };

    struct dObStack {
        dObStackArena* first;
        dObStackArena* last;
        dObStackArena* current_arena;
        int              current_ofs;
    };

    struct dxJointGroup {
        int num;
        dObStack stack;
    };

    struct dxJointGroupID {

    };

    struct dxQuickStepParameters {
        int num_iterations;
        float w;
    };

    struct dxContactParameters {
        float max_vel;
        float min_depth;
    };

    struct dxAutoDisable {
        float linear_threshold;
        float angular_threshold;
        float idle_time;
        int   idle_steps;
    };

    struct dxDamping {
        float m_linearDamping;
        float m_angularDamping;
    };

    struct dxWorld {
        dxBody*               m_firstEnabledBody;
        dxBody*               m_firstDisabledBody;
        dxJoint*              firstjoint;
        int                   nb;
        int                   nj;
        float                 gravity[4];
        float                 global_erp;
        float                 global_cfm;
        dxAutoDisable         adis;
        dxDamping             m_damping;
        int                   adis_flag;
        bool                  m_dampingFlag;
        dxQuickStepParameters qs;
        dxContactParameters   contactp;
    };

    struct dObject {
        dxWorld*  world;
        dObject*  next;
        dObject** tome;
        void*     userdata;
        int32_t   tag;
    };

    struct dMass {
        float mass;
        float c[4];
        float I[12];
    };

    struct dxGeom {};

    struct dxBody: dObject {
        dxJointNode*  firstjoint;
        int32_t       flags;
        dxGeom*       geom;
        dMass         mass;
        float         invI[12];
        float         invMass;
        float         pos[4];
        float         q[4];
        float         R[12];
        float         lvel[4];
        float         avel[4];
        float         facc[4];
        float         tacc[4];
        float         finite_rot_axis[4];
        dxAutoDisable adis;
        dxDamping     m_damping;
        float         adis_timeleft;
        int           adis_stepsleft;
        void (__fastcall* m_movedCallback)(dxBody *);
        void (__fastcall* m_changeEnabledStateCallback)(dxBody *);
    };

    struct dxJointInfo1 {
        int32_t m;
        int32_t nub;
    };

    struct dxJointInfo2 {
        float   fps;
        float   erp;
        float*  J1l;
        float*  J1a;
        float*  J2l;
        float*  J2a;
        int32_t rowskip;
        float*  c;
        float*  cfm;
        float*  lo;
        float*  hi;
        int*    findex;
    };

    struct dxJointNode {
        dxJoint*     joint;
        dxBody*      body;
        dxJointNode* next;
    };

    struct dJointFeedback {
        float f1[4];
        float t1[4];
        float f2[4];
        float t2[4];
    };

    struct dxJointVtable {
        int32_t size;
        void (__fastcall *init    )(dxJoint *);
        void (__fastcall *getInfo1)(dxJoint *, dxJointInfo1 *);
        void (__fastcall *getInfo2)(dxJoint *, dxJointInfo2 *);
        int32_t typenum;
    };

    struct dxJointBreakInfo {
        int32_t flags;
        float   b1MaxF[3];
        float   b1MaxT[3];
        float   b2MaxF[3];
        float   b2MaxT[3];
        void (__fastcall *callback)(dxJoint *);
    };

    struct dxJoint : dObject {
        dxJointVtable*    vtable;
        int32_t           flags;
        dxJointNode       node[2];
        dJointFeedback*   feedback;
        float             lambda[6];
        dxJointBreakInfo* breakInfo;
    };

    enum {
        dxBodyFlagFiniteRotation = 1,        // use finite rotations
        dxBodyFlagFiniteRotationAxis = 2,    // use finite rotations only along axis
        dxBodyDisabled = 4,            // body is disabled
        dxBodyNoGravity = 8,            // body is not influenced by gravity
        dxBodyAutoDisable = 16        // enable auto-disable on body
    };

    float last = 0;
    float step = 1 / 120;

    void __fastcall dInternalStepIsland_x2(dxWorld* world, dxBody*const* body, int nb, dxJoint*const* _joint, int nj, float stepsize) {
        last += stepsize;
        if (last < step) return;
        stepsize = last;
        last = 0;

        int i, j, k;
        float stepsize1 = 1.0 / stepsize;

        for (i = 0; i < nb; i++)
            body[i]->tag = i;

        HeapArray<float>    I     (3*nb*4);
        HeapArray<float>    invI  (3*nb*4);
        HeapArray<dxJoint*> joint (nj);
        memcpy(joint.ptr, _joint, nj*sizeof(dxJoint*));

        for (i=0; i<nb; i++) {
            float tmp[12];
            dMULTIPLY2_333(tmp,             body[i]->mass.I, body[i]->R);
            dMULTIPLY0_333(I.ptr + i * 12,  body[i]->R,      tmp);
            dMULTIPLY2_333(tmp,             body[i]->invI,   body[i]->R);
            dMULTIPLY0_333(invI.ptr + i*12, body[i]->R,      tmp);
            dMULTIPLY0_331(tmp,             I.ptr + i * 12,  body[i]->avel);
            dCROSS(body[i]->tacc,-=,body[i]->avel,tmp);
        }

        for (i=0; i<nb; i++) {
            if ((body[i]->flags & dxBodyNoGravity)==0) {
                body[i]->facc[0] += body[i]->mass.mass * world->gravity[0];
                body[i]->facc[1] += body[i]->mass.mass * world->gravity[1];
                body[i]->facc[2] += body[i]->mass.mass * world->gravity[2];
            }
        }

        int m = 0;
        HeapArray<dxJointInfo1> info (nj);
        HeapArray<int> ofs (nj);
        for (i=0, j=0; j<nj; j++) {    // i=dest, j=src
            joint.ptr[j]->vtable->getInfo1(joint.ptr[j], info.ptr + i);
            if (info.ptr[i].m > 0) {
                joint.ptr[i] = joint.ptr[j];
                joint.ptr[i]->tag = i;
                i++;
            }
            else {
                joint.ptr[j]->tag = -1;
            }
        }
        nj = i;

        for (i=0; i<nj; i++)
            if (info.ptr[i].nub == info.ptr[i].m) {
                ofs.ptr[i] = m;
                m += info.ptr[i].m;
            }
        int nub = m;

        for (i=0; i<nj; i++)
            if (info.ptr[i].nub > 0 && info.ptr[i].nub < info.ptr[i].m) {
                ofs.ptr[i] = m;
                m += info.ptr[i].m;
            }

        for (i=0; i<nj; i++) if (info.ptr[i].nub == 0) {
            ofs.ptr[i] = m;
            m += info.ptr[i].m;
        }

        HeapArray<float> cforce (nb * 8);
        cforce.SetZero();

        if (m > 0) {
            HeapArray<float> c (m);
            c.SetZero();

            HeapArray<float> cfm (m);
            cfm.SetValue(world->global_cfm);

            HeapArray<float> lo (m);
            lo.SetValue(-FLT_MAX);

            HeapArray<float> hi (m);
            hi.SetValue(FLT_MAX);

            HeapArray<int32_t> findex (m);
            findex.SetValue(-1);

            HeapArray<float> J (2 * m * 8);
            J.SetZero();

            dxJointInfo2 Jinfo;

            Jinfo.rowskip = 8;
            Jinfo.fps     = stepsize1;
            Jinfo.erp     = world->global_erp;
            for (i=0; i<nj; i++) {
                Jinfo.J1l    = J.ptr + 2 * 8 * ofs.ptr[i];
                Jinfo.J1a    = Jinfo.J1l + 4;
                Jinfo.J2l    = Jinfo.J1l + 8 * info.ptr[i].m;
                Jinfo.J2a    = Jinfo.J2l + 4;
                Jinfo.c      = c.ptr + ofs.ptr[i];
                Jinfo.cfm    = cfm.ptr + ofs.ptr[i];
                Jinfo.lo     = lo.ptr + ofs.ptr[i];
                Jinfo.hi     = hi.ptr + ofs.ptr[i];
                Jinfo.findex = findex.ptr + ofs.ptr[i];

                joint.ptr[i]->vtable->getInfo2(joint.ptr[i], &Jinfo);

                for (j=0; j<info.ptr[i].m; j++) {
                    if (findex.ptr[ofs.ptr[i] + j] >= 0) findex.ptr[ofs.ptr[i] + j] += ofs.ptr[i];
                }
            }

            HeapArray<float> JinvM (2*m*8);
            JinvM.SetZero();

            for (i=0; i<nj; i++) {
                int b = joint.ptr[i]->node[0].body->tag;
                float body_invMass = body[b]->invMass;
                float *body_invI   = invI.ptr + b * 12;
                float *Jsrc        = J.ptr + 2 * 8 * ofs.ptr[i];
                float *Jdst        = JinvM.ptr + 2 * 8 * ofs.ptr[i];

                for (j=info.ptr[i].m-1; j>=0; j--) {
                    for (k=0; k<3; k++)
                        Jdst[k] = Jsrc[k] * body_invMass;

                    dMULTIPLY0_133 (Jdst+4,Jsrc+4,body_invI);
                    Jsrc += 8;
                    Jdst += 8;
                }

                if (joint.ptr[i]->node[1].body) {
                    b = joint.ptr[i]->node[1].body->tag;
                    body_invMass = body[b]->invMass;
                    body_invI    = invI.ptr + b * 12;

                    for (j=info.ptr[i].m-1; j>=0; j--) {
                        for (k=0; k<3; k++)
                            Jdst[k] = Jsrc[k] * body_invMass;

                        dMULTIPLY0_133 (Jdst+4,Jsrc+4,body_invI);

                        Jsrc += 8;
                        Jdst += 8;
                    }
                }
            }

            int mskip = dPAD(m);

            HeapArray<float> A (m * mskip);
            A.SetZero();

            for (i=0; i<nb; i++) {
                for (dxJointNode *n1=body[i]->firstjoint; n1; n1=n1->next) {
                    for (dxJointNode *n2=n1->next; n2; n2=n2->next) {
                        int j1 = n1->joint->tag;
                        int j2 = n2->joint->tag;

                        if (ofs.ptr[j1] < ofs.ptr[j2]) {
                            int tmp = j1;
                            j1 = j2;
                            j2 = tmp;
                        }

                        if (j1==-1 || j2==-1)
                            continue;

                        int jb1 = (joint.ptr[j1]->node[1].body == body[i]);
                        int jb2 = (joint.ptr[j2]->node[1].body == body[i]);

                        MultiplyAdd2_p8r(
                            A.ptr     + ofs.ptr[j1] * mskip + ofs.ptr[j2],
                            JinvM.ptr + 2 * 8 * ofs.ptr[j1] + jb1 * 8 * info.ptr[j1].m,
                            J.ptr     + 2 * 8 * ofs.ptr[j2] + jb2 * 8 * info.ptr[j2].m,
                            info.ptr[j1].m,
                            info.ptr[j2].m,
                            mskip
                        );
                    }
                }
            }

            for (i=0; i<nj; i++) {
                Multiply2_p8r(
                    A.ptr     + ofs.ptr[i] * (mskip+1),
                    JinvM.ptr + 2 * 8 * ofs.ptr[i],
                    J.ptr     + 2 * 8 * ofs.ptr[i],
                    info.ptr[i].m,
                    info.ptr[i].m,
                    mskip
                );

                if (joint.ptr[i]->node[1].body) {
                    MultiplyAdd2_p8r(
                        A.ptr     + ofs.ptr[i] * (mskip+1),
                        JinvM.ptr + 2 * 8 * ofs.ptr[i] + 8 * info.ptr[i].m,
                        J.ptr     + 2 * 8 * ofs.ptr[i] + 8 * info.ptr[i].m,
                        info.ptr[i].m,
                        info.ptr[i].m,
                        mskip
                    );
                }
            }

            // TODO: [INVESTIAGE] Unknown for cycle
            // Need resolve what cycle here in source
            // see dInternalStepIsland_x2:899

            for (i=0; i<m; i++)
                A.ptr[i * mskip + i] += cfm.ptr[i] * stepsize1;


            HeapArray<float> tmp1 (nb * 8);
            for (i=0; i<nb; i++) {
                float body_invMass = body[i]->invMass;
                float *body_invI   = invI.ptr + i * 12;

                for (j=0; j<3; j++)
                    tmp1.ptr[i*8+j] = body[i]->facc[j] * body_invMass + body[i]->lvel[j] * stepsize1;
                
                dMULTIPLY0_331(tmp1.ptr + i * 8 + 4, body_invI ,body[i]->tacc);
                
                for (j=0; j<3; j++)
                    tmp1.ptr[i * 8 + 4 + j] += body[i]->avel[j] * stepsize1;
            }

            HeapArray<float> rhs (m);
            for (i=0; i<nj; i++) {
                float *JJ = J.ptr + 2 * 8 * ofs.ptr[i];
                Multiply0_p81 (
                    rhs.ptr + ofs.ptr[i],
                    JJ,
                    tmp1.ptr + 8 * joint.ptr[i]->node[0].body->tag,
                    info.ptr[i].m
                );

                if (joint.ptr[i]->node[1].body) {
                    MultiplyAdd0_p81 (
                        rhs.ptr + ofs.ptr[i],
                        JJ + 8 * info.ptr[i].m,
                        tmp1.ptr + 8 * joint.ptr[i]->node[1].body->tag,
                        info.ptr[i].m
                    );
                }
            }

            for (i=0; i<m; i++)
                rhs.ptr[i] = c.ptr[i]*stepsize1 - rhs.ptr[i];

            HeapArray<float> lambda (m);
            HeapArray<float> residual (m);

            dSolveLCP(
                m,
                A.ptr,
                lambda.ptr,
                rhs.ptr,
                residual.ptr,
                nub,
                lo.ptr,
                hi.ptr,
                findex.ptr
            );

            for (i=0; i<nj; i++) {
                float *JJ = J.ptr + 2 * 8 * ofs.ptr[i];

                dxBody*           b1  = joint.ptr[i]->node[0].body;
                dxBody*           b2  = joint.ptr[i]->node[1].body;
                dJointFeedback*   fb  = joint.ptr[i]->feedback;
                dxJointBreakInfo* jBI = joint.ptr[i]->breakInfo;

                if (jBI || fb) {
                    dJointFeedback temp_fb;

                    float data1[8];
                    float data2[8];

                    Multiply1_8q1(data1, JJ, lambda.ptr + ofs.ptr[i], info.ptr[i].m);

                    float* cf1 = cforce.ptr + 8 * b1->tag;

                    cf1[0] += (temp_fb.f1[0] = data1[0]);
                    cf1[1] += (temp_fb.f1[1] = data1[1]);
                    cf1[2] += (temp_fb.f1[2] = data1[2]);
                    cf1[4] += (temp_fb.t1[0] = data1[4]);
                    cf1[5] += (temp_fb.t1[1] = data1[5]);
                    cf1[6] += (temp_fb.t1[2] = data1[6]);

                    if (b2) {
                        Multiply1_8q1 (data2, JJ + 8 * info.ptr[i].m, lambda.ptr + ofs.ptr[i], info.ptr[i].m);

                        float *cf2 = cforce.ptr + 8 * b2->tag;

                        cf2[0] += (temp_fb.f2[0] = data2[0]);
                        cf2[1] += (temp_fb.f2[1] = data2[1]);
                        cf2[2] += (temp_fb.f2[2] = data2[2]);
                        cf2[4] += (temp_fb.t2[0] = data2[4]);
                        cf2[5] += (temp_fb.t2[1] = data2[5]);
                        cf2[6] += (temp_fb.t2[2] = data2[6]);
                    }

                    if (fb) {
                        fb->f1[0] = temp_fb.f1[0];
                        fb->f1[1] = temp_fb.f1[1];
                        fb->f1[2] = temp_fb.f1[2];
                        fb->t1[0] = temp_fb.t1[0];
                        fb->t1[1] = temp_fb.t1[1];
                        fb->t1[2] = temp_fb.t1[2];

                        if (b2) {
                            fb->f2[0] = temp_fb.f2[0];
                            fb->f2[1] = temp_fb.f2[1];
                            fb->f2[2] = temp_fb.f2[2];
                            fb->t2[0] = temp_fb.t2[0];
                            fb->t2[1] = temp_fb.t2[1];
                            fb->t2[2] = temp_fb.t2[2];
                        }
                    }

                    if (jBI) {
                        float relCF1[3];
                        float relCT1[3];

                        dMULTIPLY1_331(&relCF1[0],b1->R,&temp_fb.f1[0]);
                        dMULTIPLY1_331(&relCT1[0],b1->R,&temp_fb.t1[0]);

                        if (jBI->flags & dJOINT_BREAK_AT_B1_FORCE) {
                            for (int i = 0; i < 3; i++) {
                                if (relCF1[i] > jBI->b1MaxF[i]) {
                                    jBI->flags |= dJOINT_BROKEN;
                                    goto doneCheckingBreaks;
                                }
                            }
                        }

                        if (jBI->flags & dJOINT_BREAK_AT_B1_TORQUE) {
                            for (int i = 0; i < 3; i++) {
                                if (relCT1[i] > jBI->b1MaxT[i]) {
                                    jBI->flags |= dJOINT_BROKEN;
                                    goto doneCheckingBreaks;
                                }
                            }
                        }

                        if (b2) {
                            float relCF2[3];
                            float relCT2[3];

                            dMULTIPLY1_331(&relCF2[0],b2->R,&temp_fb.f2[0]);
                            dMULTIPLY1_331(&relCT2[0],b2->R,&temp_fb.t2[0]);

                            if (jBI->flags & dJOINT_BREAK_AT_B2_FORCE) {
                                for (int i = 0; i < 3; i++) {
                                    if (relCF2[i] > jBI->b2MaxF[i]) {
                                        jBI->flags |= dJOINT_BROKEN;
                                        goto doneCheckingBreaks;
                                    }
                                }
                            }

                            if (jBI->flags & dJOINT_BREAK_AT_B2_TORQUE) {
                                for (int i = 0; i < 3; i++) {
                                    if (relCT2[i] > jBI->b2MaxT[i]) {
                                        jBI->flags |= dJOINT_BROKEN;
                                        goto doneCheckingBreaks;
                                    }
                                }
                            }
                        }

                        doneCheckingBreaks:;
                    }
                }
                else {
                    MultiplyAdd1_8q1(cforce.ptr + 8 * b1->tag, JJ, lambda.ptr + ofs.ptr[i], info.ptr[i].m);

                    if (b2) {
                        MultiplyAdd1_8q1 (
                            cforce.ptr + 8 * b2->tag,
                            JJ + 8 * info.ptr[i].m,
                            lambda.ptr + ofs.ptr[i],
                            info.ptr[i].m
                        );
                    }
                }
            }
        }

        for (i=0; i<nb; i++) {
            for (j=0; j<3; j++) cforce.ptr[i*8+j] += body[i]->facc[j];
            for (j=0; j<3; j++) cforce.ptr[i*8+4+j] += body[i]->tacc[j];
        }

        for (i=0; i < nb*8; i++)
            cforce.ptr[i] *= stepsize;

        for (i=0; i<nb; i++) {
            float body_invMass = body[i]->invMass;
            float *body_invI = invI.ptr + i * 12;
            for (j=0; j<3; j++) body[i]->lvel[j] += body_invMass * cforce.ptr[i*8+j];
            dMULTIPLYADD0_331 (body[i]->avel,body_invI,cforce.ptr + i * 8 + 4);
        }

        for (i=0; i<nb; i++)
            dxStepBody(body[i],stepsize);

        // TODO: [INVESTIAGE] Unknown for cycle
        // Need resolve what cycle here in source
        // see dInternalStepIsland_x2:1393

        for (i=0; i<nb; i++) {
            body[i]->facc[0] = 0;
            body[i]->facc[1] = 0;
            body[i]->facc[2] = 0;
            body[i]->facc[3] = 0;
            body[i]->tacc[0] = 0;
            body[i]->tacc[1] = 0;
            body[i]->tacc[2] = 0;
            body[i]->tacc[3] = 0;
        }
    };

    void __fastcall dSolveL1 (const float *L, float *B, int n, int lskip1) {
        /* declare variables - Z matrix, p and q vectors, etc */
        float Z11,Z21,Z31,Z41,p1,q1,p2,p3,p4,*ex;
        const float *ell;
        int lskip2,lskip3,i,j;
        /* compute lskip values */
        lskip2 = 2*lskip1;
        lskip3 = 3*lskip1;
        /* compute all 4 x 1 blocks of X */
        for (i=0; i <= n-4; i+=4) {
            /* compute all 4 x 1 block of X, from rows i..i+4-1 */
            /* set the Z matrix to 0 */
            Z11=0;
            Z21=0;
            Z31=0;
            Z41=0;
            ell = L + i*lskip1;
            ex = B;
            /* the inner loop that computes outer products and adds them to Z */
            for (j=i-12; j >= 0; j -= 12) {
                /* load p and q values */
                p1=ell[0];
                q1=ex[0];
                p2=ell[lskip1];
                p3=ell[lskip2];
                p4=ell[lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[1];
                q1=ex[1];
                p2=ell[1+lskip1];
                p3=ell[1+lskip2];
                p4=ell[1+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[2];
                q1=ex[2];
                p2=ell[2+lskip1];
                p3=ell[2+lskip2];
                p4=ell[2+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[3];
                q1=ex[3];
                p2=ell[3+lskip1];
                p3=ell[3+lskip2];
                p4=ell[3+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[4];
                q1=ex[4];
                p2=ell[4+lskip1];
                p3=ell[4+lskip2];
                p4=ell[4+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[5];
                q1=ex[5];
                p2=ell[5+lskip1];
                p3=ell[5+lskip2];
                p4=ell[5+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[6];
                q1=ex[6];
                p2=ell[6+lskip1];
                p3=ell[6+lskip2];
                p4=ell[6+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[7];
                q1=ex[7];
                p2=ell[7+lskip1];
                p3=ell[7+lskip2];
                p4=ell[7+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[8];
                q1=ex[8];
                p2=ell[8+lskip1];
                p3=ell[8+lskip2];
                p4=ell[8+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[9];
                q1=ex[9];
                p2=ell[9+lskip1];
                p3=ell[9+lskip2];
                p4=ell[9+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[10];
                q1=ex[10];
                p2=ell[10+lskip1];
                p3=ell[10+lskip2];
                p4=ell[10+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* load p and q values */
                p1=ell[11];
                q1=ex[11];
                p2=ell[11+lskip1];
                p3=ell[11+lskip2];
                p4=ell[11+lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* advance pointers */
                ell += 12;
                ex += 12;
                /* end of inner loop */
            }
            /* compute left-over iterations */
            j += 12;
            for (; j > 0; j--) {
                /* load p and q values */
                p1=ell[0];
                q1=ex[0];
                p2=ell[lskip1];
                p3=ell[lskip2];
                p4=ell[lskip3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                Z21 += p2 * q1;
                Z31 += p3 * q1;
                Z41 += p4 * q1;
                /* advance pointers */
                ell += 1;
                ex += 1;
            }
            /* finish computing the X(i) block */
            Z11 = ex[0] - Z11;
            ex[0] = Z11;
            p1 = ell[lskip1];
            Z21 = ex[1] - Z21 - p1*Z11;
            ex[1] = Z21;
            p1 = ell[lskip2];
            p2 = ell[1+lskip2];
            Z31 = ex[2] - Z31 - p1*Z11 - p2*Z21;
            ex[2] = Z31;
            p1 = ell[lskip3];
            p2 = ell[1+lskip3];
            p3 = ell[2+lskip3];
            Z41 = ex[3] - Z41 - p1*Z11 - p2*Z21 - p3*Z31;
            ex[3] = Z41;
            /* end of outer loop */
        }
        /* compute rows at end that are not a multiple of block size */
        for (; i < n; i++) {
            /* compute all 1 x 1 block of X, from rows i..i+1-1 */
            /* set the Z matrix to 0 */
            Z11=0;
            ell = L + i*lskip1;
            ex = B;
            /* the inner loop that computes outer products and adds them to Z */
            for (j=i-12; j >= 0; j -= 12) {
                /* load p and q values */
                p1=ell[0];
                q1=ex[0];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[1];
                q1=ex[1];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[2];
                q1=ex[2];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[3];
                q1=ex[3];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[4];
                q1=ex[4];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[5];
                q1=ex[5];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[6];
                q1=ex[6];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[7];
                q1=ex[7];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[8];
                q1=ex[8];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[9];
                q1=ex[9];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[10];
                q1=ex[10];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* load p and q values */
                p1=ell[11];
                q1=ex[11];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* advance pointers */
                ell += 12;
                ex += 12;
                /* end of inner loop */
            }
            /* compute left-over iterations */
            j += 12;
            for (; j > 0; j--) {
                /* load p and q values */
                p1=ell[0];
                q1=ex[0];
                /* compute outer product and add it to the Z matrix */
                Z11 += p1 * q1;
                /* advance pointers */
                ell += 1;
                ex += 1;
            }
            /* finish computing the X(i) block */
            Z11 = ex[0] - Z11;
            ex[0] = Z11;
        }
    }

    void __fastcall dSolveL1T (const float *L, float *B, int n, int lskip1) {
        /* declare variables - Z matrix, p and q vectors, etc */
        float Z11,m11,Z21,m21,Z31,m31,Z41,m41,p1,q1,p2,p3,p4,*ex;
        const float *ell;
        int lskip2,lskip3,i,j;
        /* special handling for L and B because we're solving L1 *transpose* */
        L = L + (n-1)*(lskip1+1);
        B = B + n-1;
        lskip1 = -lskip1;
        /* compute lskip values */
        lskip2 = 2*lskip1;
        lskip3 = 3*lskip1;
        /* compute all 4 x 1 blocks of X */
        for (i=0; i <= n-4; i+=4) {
            /* compute all 4 x 1 block of X, from rows i..i+4-1 */
            /* set the Z matrix to 0 */
            Z11=0;
            Z21=0;
            Z31=0;
            Z41=0;
            ell = L - i;
            ex = B;
            /* the inner loop that computes outer products and adds them to Z */
            for (j=i-4; j >= 0; j -= 4) {
                /* load p and q values */
                p1=ell[0];
                q1=ex[0];
                p2=ell[-1];
                p3=ell[-2];
                p4=ell[-3];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                m21 = p2 * q1;
                m31 = p3 * q1;
                m41 = p4 * q1;
                ell += lskip1;
                Z11 += m11;
                Z21 += m21;
                Z31 += m31;
                Z41 += m41;
                /* load p and q values */
                p1=ell[0];
                q1=ex[-1];
                p2=ell[-1];
                p3=ell[-2];
                p4=ell[-3];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                m21 = p2 * q1;
                m31 = p3 * q1;
                m41 = p4 * q1;
                ell += lskip1;
                Z11 += m11;
                Z21 += m21;
                Z31 += m31;
                Z41 += m41;
                /* load p and q values */
                p1=ell[0];
                q1=ex[-2];
                p2=ell[-1];
                p3=ell[-2];
                p4=ell[-3];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                m21 = p2 * q1;
                m31 = p3 * q1;
                m41 = p4 * q1;
                ell += lskip1;
                Z11 += m11;
                Z21 += m21;
                Z31 += m31;
                Z41 += m41;
                /* load p and q values */
                p1=ell[0];
                q1=ex[-3];
                p2=ell[-1];
                p3=ell[-2];
                p4=ell[-3];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                m21 = p2 * q1;
                m31 = p3 * q1;
                m41 = p4 * q1;
                ell += lskip1;
                ex -= 4;
                Z11 += m11;
                Z21 += m21;
                Z31 += m31;
                Z41 += m41;
                /* end of inner loop */
            }
            /* compute left-over iterations */
            j += 4;
            for (; j > 0; j--) {
                /* load p and q values */
                p1=ell[0];
                q1=ex[0];
                p2=ell[-1];
                p3=ell[-2];
                p4=ell[-3];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                m21 = p2 * q1;
                m31 = p3 * q1;
                m41 = p4 * q1;
                ell += lskip1;
                ex -= 1;
                Z11 += m11;
                Z21 += m21;
                Z31 += m31;
                Z41 += m41;
            }
            /* finish computing the X(i) block */
            Z11 = ex[0] - Z11;
            ex[0] = Z11;
            p1 = ell[-1];
            Z21 = ex[-1] - Z21 - p1*Z11;
            ex[-1] = Z21;
            p1 = ell[-2];
            p2 = ell[-2+lskip1];
            Z31 = ex[-2] - Z31 - p1*Z11 - p2*Z21;
            ex[-2] = Z31;
            p1 = ell[-3];
            p2 = ell[-3+lskip1];
            p3 = ell[-3+lskip2];
            Z41 = ex[-3] - Z41 - p1*Z11 - p2*Z21 - p3*Z31;
            ex[-3] = Z41;
            /* end of outer loop */
        }
        /* compute rows at end that are not a multiple of block size */
        for (; i < n; i++) {
            /* compute all 1 x 1 block of X, from rows i..i+1-1 */
            /* set the Z matrix to 0 */
            Z11=0;
            ell = L - i;
            ex = B;
            /* the inner loop that computes outer products and adds them to Z */
            for (j=i-4; j >= 0; j -= 4) {
                /* load p and q values */
                p1=ell[0];
                q1=ex[0];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                ell += lskip1;
                Z11 += m11;
                /* load p and q values */
                p1=ell[0];
                q1=ex[-1];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                ell += lskip1;
                Z11 += m11;
                /* load p and q values */
                p1=ell[0];
                q1=ex[-2];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                ell += lskip1;
                Z11 += m11;
                /* load p and q values */
                p1=ell[0];
                q1=ex[-3];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                ell += lskip1;
                ex -= 4;
                Z11 += m11;
                /* end of inner loop */
            }
            /* compute left-over iterations */
            j += 4;
            for (; j > 0; j--) {
                /* load p and q values */
                p1=ell[0];
                q1=ex[0];
                /* compute outer product and add it to the Z matrix */
                m11 = p1 * q1;
                ell += lskip1;
                ex -= 1;
                Z11 += m11;
            }
            /* finish computing the X(i) block */
            Z11 = ex[0] - Z11;
            ex[0] = Z11;
        }
    }

    float __fastcall dDot (const float *a, const float *b, int n) {
        size_t base = 0;

        float sum = 0.0f;
        for (; base < n; base++) {
            sum += a[base] * b[base];
        };

        return sum;
    }

    int __fastcall dFactorCholesky (float *A, int n) {
        int i,j,k,nskip;
        float sum,*a,*b,*aa,*bb,*cc;
        nskip = dPAD (n);

        HeapArray<float> recip (n);
        aa = A;
        for (i=0; i<n; i++) {
            bb = A;
            cc = A + i*nskip;

            for (j=0; j<i; j++) {
                sum = *cc;
                a = aa;
                b = bb;
                for (k=j; k; k--)
                    sum -= (*(a++))*(*(b++));
                *cc = sum * recip.ptr[j];
                bb += nskip;
                cc++;
            }

            sum = *cc;
            a = aa;

            for (k=i; k; k--, a++)
                sum -= (*a)*(*a);

            if (sum <= 0.0f)
                return 0;

            *cc = sqrtf(sum);
            recip.ptr[i] = 1.0f / *cc;
            aa += nskip;
        }
        return 1;
    };


    int __fastcall dIsPositiveDefinite (const float *A, int n) {
        int nskip = dPAD (n);
        HeapArray<float> Acopy (nskip * n);
        memcpy(Acopy.ptr, A, nskip * n * sizeof(float));
        return dFactorCholesky (Acopy.ptr, n);
    };

    void __fastcall dSolveCholesky (const float *L, float *b, int n) {
        int i,k,nskip;
        float sum;
        nskip = dPAD (n);
        HeapArray<float> y (n);

        for (i=0; i<n; i++) {
            sum = 0;
            for (k=0; k < i; k++) sum += L[i*nskip+k]*y.ptr[k];
            y.ptr[i] = (b[i]-sum)/L[i*nskip+i];
        }
        for (i=n-1; i >= 0; i--) {
            sum = 0;
            for (k=i+1; k < n; k++) sum += L[k*nskip+i]*b[k];
            b[i] = (y.ptr[i]-sum)/L[i*nskip+i];
        }
    }

    int __fastcall dInvertPDMatrix (const float *A, float *Ainv, int n)
    {
        int i,j,nskip;

        nskip = dPAD (n);

        HeapArray<float> L (nskip * n);
        memcpy(L.ptr,A,nskip*n*sizeof(float));
        HeapArray<float> x (n);

        if (dFactorCholesky(L.ptr, n) == 0) return 0;
            memset(Ainv, 0, n * nskip * sizeof(float));

            for (i=0; i<n; i++) {
                for (j=0; j<n; j++) x.ptr[j] = 0;
                    x.ptr[i] = 1;
                
                dSolveCholesky (L.ptr,x.ptr,n);

                for (j=0; j<n; j++)
                    Ainv[j*nskip+i] = x.ptr[j];
        }
        return 1;
    }

    #define M_SQRT1_2 0.7071067811865475244008443621048490f

    void __fastcall dLDLTAddTL (float *L, float *d, const float *a, int n, int nskip) {
        int j,p;
        float W11,W21,alpha1,alpha2,alphanew,gamma1,gamma2,k1,k2,Wp,ell,dee;

        if (n < 2) return;

        HeapArray<float> W1 (n);
        HeapArray<float> W2 (n);

        W1.ptr[0] = 0;
        W2.ptr[0] = 0;
        for (j=1; j<n; j++)
            W1.ptr[j] = W2.ptr[j] = a[j] * M_SQRT1_2;

        W11 = (0.5f * a[0]+1) * M_SQRT1_2;
        W21 = (0.5f * a[0]-1) * M_SQRT1_2;

        alpha1=1;
        alpha2=1;

        dee = d[0];
        alphanew = alpha1 + (W11*W11)*dee;
        dee /= alphanew;
        gamma1 = W11 * dee;
        dee *= alpha1;
        alpha1 = alphanew;
        alphanew = alpha2 - (W21*W21)*dee;
        dee /= alphanew;
        gamma2 = W21 * dee;
        alpha2 = alphanew;
        k1 = 1.0f - W21*gamma1;
        k2 = W21*gamma1*W11 - W21;
        for (p=1; p<n; p++) {
            Wp = W1.ptr[p];
            ell = L[p*nskip];
            W1.ptr[p] =    Wp - W11*ell;
            W2.ptr[p] = k1*Wp +  k2*ell;
        }

        for (j=1; j<n; j++) {
            dee = d[j];
            alphanew = alpha1 + (W1.ptr[j] * W1.ptr[j]) * dee;
            dee /= alphanew;
            gamma1 = W1.ptr[j] * dee;
            dee *= alpha1;
            alpha1 = alphanew;
            alphanew = alpha2 - (W2.ptr[j] * W2.ptr[j])*dee;
            dee /= alphanew;
            gamma2 = W2.ptr[j] * dee;
            dee *= alpha2;
            d[j] = dee;
            alpha2 = alphanew;

            k1 = W1.ptr[j];
            k2 = W2.ptr[j];
            for (p=j+1; p<n; p++) {
                ell = L[p*nskip+j];
                Wp = W1.ptr[p] - k1 * ell;
                ell += gamma1 * Wp;
                W1.ptr[p] = Wp;
                Wp = W2.ptr[p] - k2 * ell;
                ell -= gamma2 * Wp;
                W2.ptr[p] = Wp;
                L[p*nskip+j] = ell;
            }
        }
    }

    #define _GETA(i,j) (A[i][j])
    #define GETA(i,j) ((i > j) ? _GETA(i,j) : _GETA(j,i))

    void dRemoveRowCol (float *A, int n, int nskip, int r) {
        int i;
        if (r >= n-1) return;
        if (r > 0) {
            for (i=0; i<r; i++)
            memmove (A+i*nskip+r,A+i*nskip+r+1,(n-r-1)*sizeof(float));
            for (i=r; i<(n-1); i++)
            memcpy (A+i*nskip,A+i*nskip+nskip,r*sizeof(float));
        }
        for (i=r; i<(n-1); i++)
            memcpy (A+i*nskip+r,A+i*nskip+nskip+r+1,(n-r-1)*sizeof(float));
    }

    void __fastcall dLDLTRemove (float **A, const int *p, float *L, float *d, int n1, int n2, int r, int nskip) {
        int i;

        if (r==n2-1) {
            return;		// deleting last row/col is easy
        }
        else if (r==0) {
            HeapArray<float> a (n2);
            for (i=0; i<n2; i++) a.ptr[i] = -GETA(p[i],p[0]);
            a.ptr[0] += 1.0f;
            dLDLTAddTL (L,d,a.ptr,n2,nskip);
        }
        else {
            HeapArray<float> t (r);
            HeapArray<float> a (n2 - r);

            for (i=0; i<r; i++)
                t.ptr[i] = L[r*nskip+i] / d[i];

            for (i=0; i<(n2-r); i++)
                a.ptr[i] = dDot(L+(r+i)*nskip,t.ptr,r) - GETA(p[r+i],p[r]);
            
            a.ptr[0] += 1.0f;
            dLDLTAddTL (L + r*nskip+r, d+r, a.ptr, n2-r, nskip);
        }

        // snip out row/column r from L and d
        dRemoveRowCol(L,n2,nskip,r);
        if (r < (n2-1)) memmove (d+r,d+r+1,(n2-r-1)*sizeof(float));
    }

    typedef void* (__thiscall* PFNdObStack_Rewind)(dObStack* self);
    PFNdObStack_Rewind dObStack_Rewind = (PFNdObStack_Rewind) 0x008FC6F0;

    typedef void* (__thiscall* PFNdObStack_Next)(dObStack* self, int num_bytes);
    PFNdObStack_Next dObStack_Next = (PFNdObStack_Next) 0x008FC720;

    typedef void* (__thiscall* PFNdObStack_FreeAll)(dObStack* self);
    PFNdObStack_FreeAll dObStack_FreeAll = (PFNdObStack_FreeAll) 0x008FC6C0;

    void removeJointReferencesFromAttachedBodies(dxJoint *j) {
        for (int i=0; i<2; i++) {
            dxBody *body = j->node[i].body;
            if (body) {
                dxJointNode *n = body->firstjoint;
                dxJointNode *last = 0;
                while (n) {
                    if (n->joint == j) {
                        if (last) last->next = n->next;
                        else body->firstjoint = n->next;
                        break;
                    }
                    last = n;
                    n = n->next;
                }
            }
        }
        j->node[0].body = 0;
        j->node[0].next = 0;
        j->node[1].body = 0;
        j->node[1].next = 0;
    }

    void removeObjectFromList (dObject *obj) {
        if (obj->next) obj->next->tome = obj->tome;
        *(obj->tome) = obj->next;
        // safeguard
        obj->next = 0;
        obj->tome = 0;
    }


    void __fastcall dJointGroupEmpty (dJointGroupID group) {
        // the joints in this group are detached starting from the most recently
        // added (at the top of the stack). this helps ensure that the various
        // linked lists are not traversed too much, as the joints will hopefully
        // be at the start of those lists.
        // if any group joints have their world pointer set to 0, their world was
        // previously destroyed. no special handling is required for these joints.

        int i;
        HeapArray<dxJoint*> jlist (group->num);
        dxJoint *j = (dxJoint*) dObStack_Rewind(&group->stack);
        for (i=0; i < group->num; i++) {
            jlist.ptr[i] = j;
            j = (dxJoint*) dObStack_Next(&group->stack, j->vtable->size);
        }
        for (i=group->num-1; i >= 0; i--) {
            if (jlist.ptr[i]->world) {
            removeJointReferencesFromAttachedBodies(jlist.ptr[i]);
            removeObjectFromList (jlist.ptr[i]);
            jlist.ptr[i]->world->nj--;
            }
        }
        group->num = 0;
        dObStack_FreeAll(&group->stack);
    };

    struct dLCP {
        int     n;
        int     nskip;
        int     nub;
        float** A;
        float*  Adata;
        float*  x;
        float*  b;
        float*  w;
        float*  lo;
        float*  hi;
        float*  L;
        float*  d;
        float*  Dell;
        float*  ell;
        float*  tmp;
        int*    state;
        int*    findex;
        int*    p;
        int*    C;
        int     nC;
        int     nN;

        dLCP (int _n, int _nub, float* _Adata, float* _x, float* _b, float* _w,
              float* _lo, float* _hi, float* _L, float* _d,
              float* _Dell, float* _ell, float* _tmp,
              int* _state, int* _findex, int* _p, int* _C, float** Arows);

        inline int getNub() { return nub; }
        inline void transfer_i_to_C (int i);
        inline void transfer_i_to_N (int i) { nN++; }			// because we can assume C and N span 1:i-1
        inline void transfer_i_from_N_to_C (int i);
        inline void transfer_i_from_C_to_N (int i);
        inline int numC() { return nC; }
        inline int numN() { return nN; }
        inline int indexC (int i) { return i; }
        inline int indexN (int i) { return i+nC; }
        inline float Aii (int i) { return this->A[i][i]; }
        inline float AiC_times_qC (int i, float *q) { return dDot (this->A[i],q,nC); }
        inline float AiN_times_qN (int i, float *q) { return dDot (this->A[i]+nC,q+nC,nN); }
        inline void pN_equals_ANC_times_qC (float *p, float *q);
        inline void pN_plusequals_ANi (float *p, int i, int sign=1);
        inline void pC_plusequals_s_times_qC (float *p, float s, float *q) { for (int i=0; i<nC; i++) p[i] += s*q[i]; }
        inline void pN_plusequals_s_times_qN (float *p, float s, float *q) { for (int i=0; i<nN; i++) p[i+nC] += s*q[i+nC]; }
        inline void solve1 (float *a, int i, int dir=1, int only_transfer=0);
        inline void unpermute();
    };

    void dSetZero (float *a, int n) {
        while (n > 0) {
            *(a++) = 0;
            n--;
        }
    }

    static void swapRowsAndCols (float** A, int n, int i1, int i2, int nskip,
			     int do_fast_row_swaps) {
        int i;

        for (i=i1+1; i<i2; i++) A[i1][i] = A[i][i1];
        for (i=i1+1; i<i2; i++) A[i][i1] = A[i2][i];
        A[i1][i2] = A[i1][i1];
        A[i1][i1] = A[i2][i1];
        A[i2][i1] = A[i2][i2];
        // swap rows, by swapping row pointers
        if (do_fast_row_swaps) {
            float *tmpp;
            tmpp = A[i1];
            A[i1] = A[i2];
            A[i2] = tmpp;
        }
        else {
            HeapArray<float> tmprow (n);
            memcpy (tmprow.ptr,A[i1],n * sizeof(float));
            memcpy (A[i1],A[i2],n * sizeof(float));
            memcpy (A[i2],tmprow.ptr,n * sizeof(float));
        }
        // swap columns the hard way
        for (i=i2+1; i<n; i++) {
            float tmp = A[i][i1];
            A[i][i1] = A[i][i2];
            A[i][i2] = tmp;
        }
    }

    void swapProblem (float** A, float *x, float *b, float *w, float *lo,
			 float *hi, int *p, int *state, int *findex,
			 int n, int i1, int i2, int nskip,
			 int do_fast_row_swaps)
        {
        float tmp;
        int tmpi;
        if (i1==i2)
            return;

        swapRowsAndCols (A,n,i1,i2,nskip,do_fast_row_swaps);
        tmp = x[i1];
        x[i1] = x[i2];
        x[i2] = tmp;
        tmp = b[i1];
        b[i1] = b[i2];
        b[i2] = tmp;
        tmp = w[i1];
        w[i1] = w[i2];
        w[i2] = tmp;
        tmp = lo[i1];
        lo[i1] = lo[i2];
        lo[i2] = tmp;
        tmp = hi[i1];
        hi[i1] = hi[i2];
        hi[i2] = tmp;
        tmpi = p[i1];
        p[i1] = p[i2];
        p[i2] = tmpi;
        tmpi = state[i1];
        state[i1] = state[i2];
        state[i2] = tmpi;
        if (findex) {
            tmpi = findex[i1];
            findex[i1] = findex[i2];
            findex[i2] = tmpi;
        }
    }

    void dLCP::solve1 (float *a, int i, int dir, int only_transfer)
    {
        // the `Dell' and `ell' that are computed here are saved. if index i is
        // later added to the factorization then they can be reused.
        //
        // @@@ question: do we need to solve for entire delta_x??? yes, but
        //     only if an x goes below 0 during the step.

        int j;
        if (nC > 0) {
            float *aptr = A[i];
        #   ifdef NUB_OPTIMIZATIONS
            // if nub>0, initial part of aptr[] is guaranteed unpermuted
            for (j=0; j<nub; j++) Dell[j] = aptr[j];
            for (j=nub; j<nC; j++) Dell[j] = aptr[C[j]];
        #   else
            for (j=0; j<nC; j++) Dell[j] = aptr[C[j]];
        #   endif
            dSolveL1 (L,Dell,nC,nskip);

            for (j=0; j<nC; j++) ell[j] = Dell[j] * d[j];

            if (!only_transfer) {
            for (j=0; j<nC; j++) tmp[j] = ell[j];
            dSolveL1T (L,tmp,nC,nskip);

            if (dir > 0) {
            for (j=0; j<nC; j++) a[C[j]] = -tmp[j];
            }
            else {
            for (j=0; j<nC; j++) a[C[j]] = tmp[j];
            }
            }
        }
    }


    void dLCP::unpermute()
    {
        // now we have to un-permute x and w
        int j;
        HeapArray<float> tmp (n);
        memcpy (tmp.ptr,x,n*sizeof(float));
        for (j=0; j<n; j++) x[p[j]] = tmp.ptr[j];
        memcpy (tmp.ptr,w,n*sizeof(float));
        for (j=0; j<n; j++) w[p[j]] = tmp.ptr[j];
    }

    void dLCP::pN_equals_ANC_times_qC (float *p, float *q) {
    // we could try to make this matrix-vector multiplication faster using
    // outer product matrix tricks, e.g. with the dMultidotX() functions.
    // but i tried it and it actually made things slower on random 100x100
    // problems because of the overhead involved. so we'll stick with the
    // simple method for now.
    for (int i=0; i<nN; i++) p[i+nC] = dDot (A[i+nC],q,nC);
    }


    void dLCP::pN_plusequals_ANi (float *p, int i, int sign) {
        float *aptr = A[i]+nC;
        if (sign > 0) {
            for (int i=0; i<nN; i++) p[i+nC] += aptr[i];
        }
        else {
            for (int i=0; i<nN; i++) p[i+nC] -= aptr[i];
        }
    }

    void dLCP::transfer_i_from_C_to_N (int i) {
        // remove a row/column from the factorization, and adjust the
        // indexes (black magic!)
        int j,k;
        for (j=0; j<nC; j++) if (C[j]==i) {
            dLDLTRemove (A,C,L,d,n,nC,j,nskip);
            for (k=0; k<nC; k++) if (C[k]==nC-1) {
                C[k] = C[j];
                if (j < (nC-1)) memmove (C+j,C+j+1,(nC-j-1)*sizeof(int));
                break;
            }
            break;
        }
        swapProblem (A,x,b,w,lo,hi,p,state,findex,n,i,nC-1,nskip,1);
        nC--;
        nN++;
    }

    void dLCP::transfer_i_from_N_to_C (int i) {
        int j;
        if (nC > 0) {
            float *aptr = A[i];
            // if nub>0, initial part of aptr unpermuted
            for (j=0; j<nub; j++) Dell[j] = aptr[j];
            for (j=nub; j<nC; j++) Dell[j] = aptr[C[j]];
            dSolveL1 (L,Dell,nC,nskip);
            for (j=0; j<nC; j++) ell[j] = Dell[j] * d[j];
            for (j=0; j<nC; j++) L[nC*nskip+j] = ell[j];
            d[nC] = 1.0f / (A[i][i] - dDot(ell,Dell,nC));
        }
        else {
            d[0] = 1.0f / (A[i][i]);
        }
        swapProblem (A,x,b,w,lo,hi,p,state,findex,n,nC,i,nskip,1);
        C[nC] = nC;
        nN--;
        nC++;

        // @@@ TO DO LATER
        // if we just finish here then we'll go back and re-solve for
        // delta_x. but actually we can be more efficient and incrementally
        // update delta_x here. but if we do this, we wont have ell and Dell
        // to use in updating the factorization later.
    }

    void dLCP::transfer_i_to_C (int i) {
        int j;
        if (nC > 0) {
            // ell,Dell were computed by solve1(). note, ell = D \ L1solve (L,A(i,C))
            for (j=0; j<nC; j++) L[nC*nskip+j] = ell[j];
            d[nC] = 1.0f / (A[i][i] - dDot(ell,Dell,nC));
        }
        else {
            d[0] = 1.0f / (A[i][i]);
        }
        swapProblem (A,x,b,w,lo,hi,p,state,findex,n,nC,i,nskip,1);
        C[nC] = nC;
        nC++;
    }
    

    void dSolveL1_1 (const float *L, float *B, int n, int lskip1) {
        /* declare variables - Z matrix, p and q vectors, etc */
        float Z11,m11,Z21,m21,p1,q1,p2,*ex;
        const float *ell;
        int i,j;
        /* compute all 2 x 1 blocks of X */
        for (i=0; i < n; i+=2) {
            /* compute all 2 x 1 block of X, from rows i..i+2-1 */
            /* set the Z matrix to 0 */
            Z11=0;
            Z21=0;
            ell = L + i*lskip1;
            ex = B;
            /* the inner loop that computes outer products and adds them to Z */
            for (j=i-2; j >= 0; j -= 2) {
            /* compute outer product and add it to the Z matrix */
            p1=ell[0];
            q1=ex[0];
            m11 = p1 * q1;
            p2=ell[lskip1];
            m21 = p2 * q1;
            Z11 += m11;
            Z21 += m21;
            /* compute outer product and add it to the Z matrix */
            p1=ell[1];
            q1=ex[1];
            m11 = p1 * q1;
            p2=ell[1+lskip1];
            m21 = p2 * q1;
            /* advance pointers */
            ell += 2;
            ex += 2;
            Z11 += m11;
            Z21 += m21;
            /* end of inner loop */
            }
            /* compute left-over iterations */
            j += 2;
            for (; j > 0; j--) {
            /* compute outer product and add it to the Z matrix */
            p1=ell[0];
            q1=ex[0];
            m11 = p1 * q1;
            p2=ell[lskip1];
            m21 = p2 * q1;
            /* advance pointers */
            ell += 1;
            ex += 1;
            Z11 += m11;
            Z21 += m21;
            }
            /* finish computing the X(i) block */
            Z11 = ex[0] - Z11;
            ex[0] = Z11;
            p1 = ell[lskip1];
            Z21 = ex[1] - Z21 - p1*Z11;
            ex[1] = Z21;
            /* end of outer loop */
        }
    }

    void _dSolveL1_2 (float *L, float *B, int n, int lskip1) {  
        for (size_t j = 0; j < n * lskip1; j+= lskip1) {
            float* p = B;
            float* q = B + n;
            float* l = L + j;
            for (size_t i = 0; i < n; i++) {
                p[i] += l[i]/ l[j] * p[i];
                q[i] += l[i]/ l[j] * q[i];
            }
        };
    };

    void dSolveL1_2 (const float *L, float *B, int n, int lskip1) {  
        /* declare variables - Z matrix, p and q vectors, etc */
        float Z11,m11,Z12,m12,Z21,m21,Z22,m22,p1,q1,p2,q2,*ex;
        const float *ell;
        int i,j;
        /* compute all 2 x 2 blocks of X */
        for (i=0; i < n; i+=2) {
            /* compute all 2 x 2 block of X, from rows i..i+2-1 */
            /* set the Z matrix to 0 */
            Z11=0;
            Z12=0;
            Z21=0;
            Z22=0;
            ell = L + i*lskip1;
            ex = B;
            /* the inner loop that computes outer products and adds them to Z */
            for (j=i-2; j >= 0; j -= 2) {
            /* compute outer product and add it to the Z matrix */
            p1=ell[0];
            q1=ex[0];
            m11 = p1 * q1;
            q2=ex[lskip1];
            m12 = p1 * q2;
            p2=ell[lskip1];
            m21 = p2 * q1;
            m22 = p2 * q2;
            Z11 += m11;
            Z12 += m12;
            Z21 += m21;
            Z22 += m22;
            /* compute outer product and add it to the Z matrix */
            p1=ell[1];
            q1=ex[1];
            m11 = p1 * q1;
            q2=ex[1+lskip1];
            m12 = p1 * q2;
            p2=ell[1+lskip1];
            m21 = p2 * q1;
            m22 = p2 * q2;
            /* advance pointers */
            ell += 2;
            ex += 2;
            Z11 += m11;
            Z12 += m12;
            Z21 += m21;
            Z22 += m22;
            /* end of inner loop */
            }
            /* compute left-over iterations */
            j += 2;
            for (; j > 0; j--) {
            /* compute outer product and add it to the Z matrix */
            p1=ell[0];
            q1=ex[0];
            m11 = p1 * q1;
            q2=ex[lskip1];
            m12 = p1 * q2;
            p2=ell[lskip1];
            m21 = p2 * q1;
            m22 = p2 * q2;
            /* advance pointers */
            ell += 1;
            ex += 1;
            Z11 += m11;
            Z12 += m12;
            Z21 += m21;
            Z22 += m22;
            }
            /* finish computing the X(i) block */
            Z11 = ex[0] - Z11;
            ex[0] = Z11;
            Z12 = ex[lskip1] - Z12;
            ex[lskip1] = Z12;
            p1 = ell[lskip1];
            Z21 = ex[1] - Z21 - p1*Z11;
            ex[1] = Z21;
            Z22 = ex[1+lskip1] - Z22 - p1*Z12;
            ex[1+lskip1] = Z22;
            /* end of outer loop */
        }
    }

    void dFactorLDLT (float *A, float *d, int n, int nskip1) {
        int i,j;
        float sum,*ell,*dee,dd,p1,p2,q1,q2,Z11,m11,Z21,m21,Z22,m22;
        if (n < 1) return;
        
        for (i=0; i<=n-2; i += 2) {
            /* solve L*(D*l)=a, l is scaled elements in 2 x i block at A(i,0) */
            dSolveL1_2 (A,A+i*nskip1,i,nskip1);
            /* scale the elements in a 2 x i block at A(i,0), and also */
            /* compute Z = the outer product matrix that we'll need. */
            Z11 = 0;
            Z21 = 0;
            Z22 = 0;
            ell = A+i*nskip1;
            dee = d;
            for (j=i-6; j >= 0; j -= 6) {
            p1 = ell[0];
            p2 = ell[nskip1];
            dd = dee[0];
            q1 = p1*dd;
            q2 = p2*dd;
            ell[0] = q1;
            ell[nskip1] = q2;
            m11 = p1*q1;
            m21 = p2*q1;
            m22 = p2*q2;
            Z11 += m11;
            Z21 += m21;
            Z22 += m22;
            p1 = ell[1];
            p2 = ell[1+nskip1];
            dd = dee[1];
            q1 = p1*dd;
            q2 = p2*dd;
            ell[1] = q1;
            ell[1+nskip1] = q2;
            m11 = p1*q1;
            m21 = p2*q1;
            m22 = p2*q2;
            Z11 += m11;
            Z21 += m21;
            Z22 += m22;
            p1 = ell[2];
            p2 = ell[2+nskip1];
            dd = dee[2];
            q1 = p1*dd;
            q2 = p2*dd;
            ell[2] = q1;
            ell[2+nskip1] = q2;
            m11 = p1*q1;
            m21 = p2*q1;
            m22 = p2*q2;
            Z11 += m11;
            Z21 += m21;
            Z22 += m22;
            p1 = ell[3];
            p2 = ell[3+nskip1];
            dd = dee[3];
            q1 = p1*dd;
            q2 = p2*dd;
            ell[3] = q1;
            ell[3+nskip1] = q2;
            m11 = p1*q1;
            m21 = p2*q1;
            m22 = p2*q2;
            Z11 += m11;
            Z21 += m21;
            Z22 += m22;
            p1 = ell[4];
            p2 = ell[4+nskip1];
            dd = dee[4];
            q1 = p1*dd;
            q2 = p2*dd;
            ell[4] = q1;
            ell[4+nskip1] = q2;
            m11 = p1*q1;
            m21 = p2*q1;
            m22 = p2*q2;
            Z11 += m11;
            Z21 += m21;
            Z22 += m22;
            p1 = ell[5];
            p2 = ell[5+nskip1];
            dd = dee[5];
            q1 = p1*dd;
            q2 = p2*dd;
            ell[5] = q1;
            ell[5+nskip1] = q2;
            m11 = p1*q1;
            m21 = p2*q1;
            m22 = p2*q2;
            Z11 += m11;
            Z21 += m21;
            Z22 += m22;
            ell += 6;
            dee += 6;
            }
            /* compute left-over iterations */
            j += 6;
            for (; j > 0; j--) {
            p1 = ell[0];
            p2 = ell[nskip1];
            dd = dee[0];
            q1 = p1*dd;
            q2 = p2*dd;
            ell[0] = q1;
            ell[nskip1] = q2;
            m11 = p1*q1;
            m21 = p2*q1;
            m22 = p2*q2;
            Z11 += m11;
            Z21 += m21;
            Z22 += m22;
            ell++;
            dee++;
            }
            /* solve for diagonal 2 x 2 block at A(i,i) */
            Z11 = ell[0] - Z11;
            Z21 = ell[nskip1] - Z21;
            Z22 = ell[1+nskip1] - Z22;
            dee = d + i;
            /* factorize 2 x 2 block Z,dee */
            /* factorize row 1 */
            dee[0] = 1.0f / Z11;
            /* factorize row 2 */
            sum = 0;
            q1 = Z21;
            q2 = q1 * dee[0];
            Z21 = q2;
            sum += q1*q2;
            dee[1] = 1.0f / (Z22 - sum);
            /* done factorizing 2 x 2 block */
            ell[nskip1] = Z21;
        }
        /* compute the (less than 2) rows at the bottom */
        switch (n-i) {
            case 0:
            break;
            
            case 1:
            dSolveL1_1 (A,A+i*nskip1,i,nskip1);
            /* scale the elements in a 1 x i block at A(i,0), and also */
            /* compute Z = the outer product matrix that we'll need. */
            Z11 = 0;
            ell = A+i*nskip1;
            dee = d;
            for (j=i-6; j >= 0; j -= 6) {
            p1 = ell[0];
            dd = dee[0];
            q1 = p1*dd;
            ell[0] = q1;
            m11 = p1*q1;
            Z11 += m11;
            p1 = ell[1];
            dd = dee[1];
            q1 = p1*dd;
            ell[1] = q1;
            m11 = p1*q1;
            Z11 += m11;
            p1 = ell[2];
            dd = dee[2];
            q1 = p1*dd;
            ell[2] = q1;
            m11 = p1*q1;
            Z11 += m11;
            p1 = ell[3];
            dd = dee[3];
            q1 = p1*dd;
            ell[3] = q1;
            m11 = p1*q1;
            Z11 += m11;
            p1 = ell[4];
            dd = dee[4];
            q1 = p1*dd;
            ell[4] = q1;
            m11 = p1*q1;
            Z11 += m11;
            p1 = ell[5];
            dd = dee[5];
            q1 = p1*dd;
            ell[5] = q1;
            m11 = p1*q1;
            Z11 += m11;
            ell += 6;
            dee += 6;
            }
            /* compute left-over iterations */
            j += 6;
            for (; j > 0; j--) {
            p1 = ell[0];
            dd = dee[0];
            q1 = p1*dd;
            ell[0] = q1;
            m11 = p1*q1;
            Z11 += m11;
            ell++;
            dee++;
            }
            /* solve for diagonal 1 x 1 block at A(i,i) */
            Z11 = ell[0] - Z11;
            dee = d + i;
            /* factorize 1 x 1 block Z,dee */
            /* factorize row 1 */
            dee[0] = 1.0f / Z11;
            /* done factorizing 1 x 1 block */
            break;
            
            default: *((char*)0)=0;  /* this should never happen! */
        }
    }

    void dVectorScale (float *a, const float *d, int n) {
        for (int i=0; i<n; i++) a[i] *= d[i];
    }


    void dSolveLDLT (const float *L, const float *d, float *b, int n, int nskip) {
        dSolveL1 (L,b,n,nskip);
        dVectorScale (b,d,n);
        dSolveL1T (L,b,n,nskip);
    }

    dLCP::dLCP (int _n, int _nub, float *_Adata, float *_x, float *_b, float *_w,
                float *_lo, float *_hi, float* _L, float *_d,
                float *_Dell, float *_ell, float *_tmp,
                int *_state, int *_findex, int *_p, int *_C, float **Arows){
        n = _n;
        nub = _nub;
        Adata = _Adata;
        A = 0;
        x = _x;
        b = _b;
        w = _w;
        lo = _lo;
        hi = _hi;
        L = _L;
        d = _d;
        Dell = _Dell;
        ell = _ell;
        tmp = _tmp;
        state = _state;
        findex = _findex;
        p = _p;
        C = _C;
        nskip = dPAD(n);
        dSetZero (x,n);

        int k;

        A = Arows;
        for (k=0; k<n; k++) A[k] = Adata + k*nskip;

        nC = 0;
        nN = 0;
        for (k=0; k<n; k++) p[k]=k;		// initially unpermuted

        /*
        // for testing, we can do some random swaps in the area i > nub
        if (nub < n) {
            for (k=0; k<100; k++) {
            int i1,i2;
            do {
            i1 = dRandInt(n-nub)+nub;
            i2 = dRandInt(n-nub)+nub;
            }
            while (i1 > i2); 
            //printf ("--> %d %d\n",i1,i2);
            swapProblem (A,x,b,w,lo,hi,p,state,findex,n,i1,i2,nskip,0);
            }
        }
        */

        // permute the problem so that *all* the unbounded variables are at the
        // start, i.e. look for unbounded variables not included in `nub'. we can
        // potentially push up `nub' this way and get a bigger initial factorization.
        // note that when we swap rows/cols here we must not just swap row pointers,
        // as the initial factorization relies on the data being all in one chunk.
        // variables that have findex >= 0 are *not* considered to be unbounded even
        // if lo=-inf and hi=inf - this is because these limits may change during the
        // solution process.

        for (k=nub; k<n; k++) {
            if (findex && findex[k] >= 0) continue;
            if (lo[k]==-FLT_MAX && hi[k]==FLT_MAX) {
            swapProblem (A,x,b,w,lo,hi,p,state,findex,n,nub,k,nskip,0);
            nub++;
            }
        }

        // if there are unbounded variables at the start, factorize A up to that
        // point and solve for x. this puts all indexes 0..nub-1 into C.
        if (nub > 0) {
            for (k=0; k<nub; k++) memcpy (L+k*nskip,A[k],(k+1)*sizeof(float));
            dFactorLDLT (L,d,nub,nskip);

            memcpy (x,b,nub*sizeof(float));
            dSolveLDLT ((const float*) L,d,x,nub,nskip);
            dSetZero (w,nub);
            for (k=0; k<nub; k++) C[k] = k;
            nC = nub;
        }

        // permute the indexes > nub such that all findex variables are at the end
        if (findex) {
            int num_at_end = 0;
            for (k=n-1; k >= nub; k--) {
            if (findex[k] >= 0) {
            swapProblem (A,x,b,w,lo,hi,p,state,findex,n,k,n-1-num_at_end,nskip,1);
            num_at_end++;
            }
            }
        }

        // print info about indexes
        /*
        for (k=0; k<n; k++) {
            if (k<nub) printf ("C");
            else if (lo[k]==-dInfinity && hi[k]==dInfinity) printf ("c");
            else printf (".");
        }
        printf ("\n");
        */
    }

    void dSolveLCP (int n, float *A, float *x, float *b, float *w, int nub, float *lo, float *hi, int *findex) {
        int i,k,hit_first_friction_index = 0;
        int nskip = dPAD(n);

        // if all the variables are unbounded then we can just factor, solve,
        // and return
        if (nub >= n) {
            dFactorLDLT (A,w,n,nskip);		// use w for d
            dSolveLDLT (A,w,b,n,nskip);
            memcpy (x,b,n*sizeof(float));
            dSetZero (w,n);
            return;
        }

        HeapArray<float>  L       (n * nskip);
        HeapArray<float>  d       (n);
        HeapArray<float>  delta_x (n);
        HeapArray<float>  delta_w (n);
        HeapArray<float>  Dell    (n);
        HeapArray<float>  ell     (n);
        HeapArray<float*> Arows   (n);
        HeapArray<int>    p       (n);
        HeapArray<int>    C       (n);

        int dir;
        float dirf;

        // for i in N, state[i] is 0 if x(i)==lo(i) or 1 if x(i)==hi(i)
        HeapArray<int> state (n);

        // create LCP object. note that tmp is set to delta_w to save space, this
        // optimization relies on knowledge of how tmp is used, so be careful!
        dLCP lcp (n, nub, A, x, b, w, lo, hi, L.ptr, d.ptr, Dell.ptr, ell.ptr, delta_w.ptr, state.ptr, findex, p.ptr, C.ptr, Arows.ptr);
        nub = lcp.getNub();

        // loop over all indexes nub..n-1. for index i, if x(i),w(i) satisfy the
        // LCP conditions then i is added to the appropriate index set. otherwise
        // x(i),w(i) is driven either +ve or -ve to force it to the valid region.
        // as we drive x(i), x(C) is also adjusted to keep w(C) at zero.
        // while driving x(i) we maintain the LCP conditions on the other variables
        // 0..i-1. we do this by watching out for other x(i),w(i) values going
        // outside the valid region, and then switching them between index sets
        // when that happens.

        for (i=nub; i<n; i++) {
            // the index i is the driving index and indexes i+1..n-1 are "dont care",
            // i.e. when we make changes to the system those x's will be zero and we
            // don't care what happens to those w's. in other words, we only consider
            // an (i+1)*(i+1) sub-problem of A*x=b+w.

            // if we've hit the first friction index, we have to compute the lo and
            // hi values based on the values of x already computed. we have been
            // permuting the indexes, so the values stored in the findex vector are
            // no longer valid. thus we have to temporarily unpermute the x vector. 
            // for the purposes of this computation, 0*infinity = 0 ... so if the
            // contact constraint's normal force is 0, there should be no tangential
            // force applied.

            if (hit_first_friction_index == 0 && findex && findex[i] >= 0) {
                // un-permute x into delta_w, which is not being used at the moment
                for (k=0; k<n; k++) delta_w.ptr[p.ptr[k]] = x[k];

                // set lo and hi values
                for (k=i; k<n; k++) {
                    float wfk = delta_w.ptr[findex[k]];
                    if (wfk == 0) {
                        hi[k] = 0;
                        lo[k] = 0;
                    }
                    else {
                        hi[k] = fabsf(hi[k] * wfk);
                        lo[k] = -hi[k];
                    }
                }
                hit_first_friction_index = 1;
            }

            // thus far we have not even been computing the w values for indexes
            // greater than i, so compute w[i] now.
            w[i] = lcp.AiC_times_qC (i,x) + lcp.AiN_times_qN (i,x) - b[i];

            // if lo=hi=0 (which can happen for tangential friction when normals are
            // 0) then the index will be assigned to set N with some state. however,
            // set C's line has zero size, so the index will always remain in set N.
            // with the "normal" switching logic, if w changed sign then the index
            // would have to switch to set C and then back to set N with an inverted
            // state. this is pointless, and also computationally expensive. to
            // prevent this from happening, we use the rule that indexes with lo=hi=0
            // will never be checked for set changes. this means that the state for
            // these indexes may be incorrect, but that doesn't matter.

            // see if x(i),w(i) is in a valid region
            if (lo[i]==0 && w[i] >= 0) {
                lcp.transfer_i_to_N (i);
                state.ptr[i] = 0;
            }
            else if (hi[i]==0 && w[i] <= 0) {
                lcp.transfer_i_to_N (i);
                state.ptr[i] = 1;
            }
            else if (w[i]==0) {
                // this is a degenerate case. by the time we get to this test we know
                // that lo != 0, which means that lo < 0 as lo is not allowed to be +ve,
                // and similarly that hi > 0. this means that the line segment
                // corresponding to set C is at least finite in extent, and we are on it.
                // NOTE: we must call lcp.solve1() before lcp.transfer_i_to_C()
                lcp.solve1 (delta_x.ptr,i,0,1);
                lcp.transfer_i_to_C (i);
            }
            else {
                // we must push x(i) and w(i)
                for (;;) {
                // find direction to push on x(i)
                    if (w[i] <= 0) {
                        dir = 1;
                        dirf = 1.0f;
                    }
                    else {
                        dir = -1;
                        dirf = -1.0f;
                    }

                    // compute: delta_x(C) = -dir*A(C,C)\A(C,i)
                    lcp.solve1 (delta_x.ptr,i,dir);
                    // note that delta_x[i] = dirf, but we wont bother to set it

                    // compute: delta_w = A*delta_x ... note we only care about
                        // delta_w(N) and delta_w(i), the rest is ignored
                    lcp.pN_equals_ANC_times_qC (delta_w.ptr,delta_x.ptr);
                    lcp.pN_plusequals_ANi (delta_w.ptr,i,dir);
                        delta_w.ptr[i] = lcp.AiC_times_qC (i,delta_x.ptr) + lcp.Aii(i)*dirf;

                    // find largest step we can take (size=s), either to drive x(i),w(i)
                    // to the valid LCP region or to drive an already-valid variable
                    // outside the valid region.

                    int cmd = 1;		// index switching command
                    int si = 0;		// si = index to switch if cmd>3
                    float s = -w[i] / delta_w.ptr[i];
                    if (dir > 0) {
                        if (hi[i] < FLT_MAX) {
                            float s2 = (hi[i] - x[i]) / dirf;		// step to x(i)=hi(i)
                            if (s2 < s) {
                                s = s2;
                                cmd = 3;
                            }
                        }
                    }
                    else {
                        if (lo[i] > -FLT_MAX) {
                            float s2 = (lo[i] - x[i]) / dirf;		// step to x(i)=lo(i)
                            if (s2 < s) {
                                s = s2;
                                cmd = 2;
                            }
                        }
                    }

                    for (k=0; k < lcp.numN(); k++) {
                        if ((state.ptr[lcp.indexN(k)]==0 && delta_w.ptr[lcp.indexN(k)] < 0) ||
                            (state.ptr[lcp.indexN(k)]!=0 && delta_w.ptr[lcp.indexN(k)] > 0)) {
                            // don't bother checking if lo=hi=0
                            if (lo[lcp.indexN(k)] == 0 && hi[lcp.indexN(k)] == 0)
                                continue;

                            float s2 = -w[lcp.indexN(k)] / delta_w.ptr[lcp.indexN(k)];
                            if (s2 < s) {
                                s = s2;
                                cmd = 4;
                                si = lcp.indexN(k);
                            }
                        }
                    }

                for (k=nub; k < lcp.numC(); k++) {
                    if (delta_x.ptr[lcp.indexC(k)] < 0 && lo[lcp.indexC(k)] > -FLT_MAX) {
                        float s2 = (lo[lcp.indexC(k)]-x[lcp.indexC(k)]) /
                        delta_x.ptr[lcp.indexC(k)];
                        if (s2 < s) {
                        s = s2;
                        cmd = 5;
                        si = lcp.indexC(k);
                        }
                    }
                    if (delta_x.ptr[lcp.indexC(k)] > 0 && hi[lcp.indexC(k)] < FLT_MAX) {
                        float s2 = (hi[lcp.indexC(k)]-x[lcp.indexC(k)]) / delta_x.ptr[lcp.indexC(k)];
                        if (s2 < s) {
                            s = s2;
                            cmd = 6;
                            si = lcp.indexC(k);
                        }
                    }
                }

                //static char* cmdstring[8] = {0,"->C","->NL","->NH","N->C",
                //			     "C->NL","C->NH"};
                //printf ("cmd=%d (%s), si=%d\n",cmd,cmdstring[cmd],(cmd>3) ? si : i);

                // if s <= 0 then we've got a problem. if we just keep going then
                // we're going to get stuck in an infinite loop. instead, just cross
                // our fingers and exit with the current solution.
                if (s <= 0) {
                    if (i < (n-1)) {
                        dSetZero(x+i,n-i);
                        dSetZero(w+i,n-i);
                    }
                    goto done;
                }

                // apply x = x + s * delta_x
                lcp.pC_plusequals_s_times_qC (x,s,delta_x.ptr);
                x[i] += s * dirf;

                // apply w = w + s * delta_w
                lcp.pN_plusequals_s_times_qN (w,s,delta_w.ptr);
                w[i] += s * delta_w.ptr[i];

                // switch indexes between sets if necessary
                switch (cmd) {
                case 1:		// done
                    w[i] = 0;
                    lcp.transfer_i_to_C (i);
                    break;
                case 2:		// done
                    x[i] = lo[i];
                    state.ptr[i] = 0;
                    lcp.transfer_i_to_N (i);
                    break;
                case 3:		// done
                    x[i] = hi[i];
                    state.ptr[i] = 1;
                    lcp.transfer_i_to_N (i);
                    break;
                case 4:		// keep going
                    w[si] = 0;
                    lcp.transfer_i_from_N_to_C (si);
                    break;
                case 5:		// keep going
                    x[si] = lo[si];
                    state.ptr[si] = 0;
                    lcp.transfer_i_from_C_to_N (si);
                    break;
                case 6:		// keep going
                    x[si] = hi[si];
                    state.ptr[si] = 1;
                    lcp.transfer_i_from_C_to_N (si);
                    break;
                }

                if (cmd <= 3) break;
                }
            }
        }

        done:
        lcp.unpermute();
    };

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

    int __fastcall FillDefaultContactParameters(dContact *contact, uint32_t numContacts) {
        for (;numContacts>0;contact++,numContacts--) {
            contact->surface.mode       = 12312;
            contact->surface.mu         = 3.0f;
            contact->surface.slip1      = 1e-5f;
            contact->surface.slip2      = 1e-5f;
            contact->surface.soft_erp   = 0.8f;
            contact->surface.soft_cfm   = 1e-5f;
            contact->surface.bounce     = 1e-3f;
            contact->surface.bounce_vel = 1e-3f;
        }
        return 1;
    };


    void Apply() {
        routines::Redirect(0x0073, (void*) 0x0088D360, (void*) &FillDefaultContactParameters);
        routines::Redirect(0x2350, (void*) 0x008FF580, (void*) &dInternalStepIsland_x2);
        routines::Redirect(0x07B0, (void*) 0x00921A10, (void*) &dSolveL1);
        routines::Redirect(0x05B0, (void*) 0x00921460, (void*) &dSolveL1T);
        routines::Redirect(0x00D0, (void*) 0x009169E0, (void*) &dDot);
        routines::Redirect(0x0150, (void*) 0x0088B030, (void*) &dFactorCholesky);
        routines::Redirect(0x0330, (void*) 0x0088A400, (void*) &dSolveCholesky);
        routines::Redirect(0x0200, (void*) 0x0088B180, (void*) &dInvertPDMatrix);
        routines::Redirect(0x0060, (void*) 0x0088B380, (void*) &dIsPositiveDefinite);
        routines::Redirect(0x06E0, (void*) 0x0088A820, (void*) &dLDLTAddTL);
        routines::Redirect(0x03F0, (void*) 0x0088B3E0, (void*) &dLDLTRemove);
        routines::Redirect(0x00C0, (void*) 0x007C4FD0, (void*) &dJointGroupEmpty);
        routines::Redirect(0x0860, (void*) 0x00956450, (void*) &dSolveLCP);
    };
};