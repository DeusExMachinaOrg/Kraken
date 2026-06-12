#define LOGGER "APPENDIX"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <Windows.h>
#include "ext/ai/Appendix.hpp"
#include "ext/logger.hpp"
#include "routines.hpp"
#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/ComplexPhysicObj.hpp"
#include "hta/ai/VehiclePart.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/SceneGraph.hpp"


using namespace hta;

namespace {
    void EnsureAppendixClassRegistered();

    hta::ai::AppendixPrototypeInfo& GetDefaultAppendixPrototypeInfo() {
        static hta::ai::AppendixPrototypeInfo* s_defaultPrototype = nullptr;
        if (!s_defaultPrototype) {
            ::EnsureAppendixClassRegistered();

            void* objectMemory = hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(hta::ai::AppendixPrototypeInfo), 0, 0);
            if (!objectMemory) {
                std::abort();
            }

            s_defaultPrototype = reinterpret_cast<hta::ai::AppendixPrototypeInfo*>(objectMemory);
            ::new (s_defaultPrototype) hta::ai::AppendixPrototypeInfo();
        }
        return *s_defaultPrototype;
    }

    bool g_appendixClassRegistered = false;
    std::unordered_map<hta::ai::Vehicle*, float> g_vehicleThornForce;

    void EnsureAppendixClassRegistered() {
        if (g_appendixClassRegistered)
            return;

        hta::m3d::Kernel::Instance()->AddClass(&hta::ai::Appendix::m_classAppendix);
        g_appendixClassRegistered = true;
    }

    void SetVehicleThornForce(hta::ai::Vehicle* vehicle, float thornForce) {
        if (vehicle) {
            g_vehicleThornForce[vehicle] = thornForce;
        }
    }

    void ClearVehicleThornForce(hta::ai::Vehicle* vehicle) {
        if (vehicle) {
            g_vehicleThornForce.erase(vehicle);
        }
    }

    float GetVehicleThornForce(hta::ai::Vehicle* vehicle) {
        if (!vehicle) {
            return 1.0f;
        }

        auto it = g_vehicleThornForce.find(vehicle);
        return it != g_vehicleThornForce.end() ? it->second : 1.0f;
    }
}

namespace hta {
    namespace ai {
        AppendixPrototypeInfo::AppendixPrototypeInfo()
            : m_appendixType(AppendixType_Default)
            , m_lpName("")
            , m_thornForce(1.0f)
            , m_scale(1.0f)
        {
            m_className = "Appendix";
        }

        Obj* AppendixPrototypeInfo::CreateTargetObject() const {
            ::EnsureAppendixClassRegistered();

            void* objectMemory = hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(Appendix), 0, 0);
            if (!objectMemory) {
                return nullptr;
            }

            Appendix* object = reinterpret_cast<Appendix*>(objectMemory);
            ::new (object) Appendix(const_cast<AppendixPrototypeInfo*>(this));
            return object;
        }

        void* AppendixPrototypeInfo::operator new(size_t size) {
            return m3d::Kernel::Instance()->g_mar.AllocMem(size, "", 0);
        }

        void AppendixPrototypeInfo::operator delete(void* data) {
            m3d::Kernel::Instance()->g_mar.FreeMem(data, "", 0);
        }

        bool AppendixPrototypeInfo::LoadFromXML(m3d::cmn::XmlFile* xmlFile, const m3d::cmn::XmlNode* xmlNode) {
            bool result = GunPrototypeInfo::LoadFromXML(xmlFile, xmlNode);
            if (!xmlNode || xmlNode->IsEmpty())
                return result;

            if (const char* appendixType = xmlNode->GetAttribute("AppendixType")) {
                m_appendixType = static_cast<AppendixType>(std::atoi(appendixType));
            }

            if (const char* thornForce = xmlNode->GetAttribute("ThornForce")) {
                m_thornForce = static_cast<float>(std::atof(thornForce));
            }

            if (const char* loadPoint = xmlNode->GetAttribute("LoadPoint")) {
                m_lpName = loadPoint;
            }

            if (const char* scale = xmlNode->GetAttribute("Scale")) {
                m_scale = static_cast<float>(std::atof(scale));
            }

            return result;
        }

        Appendix::Appendix()
            : Gun(GetDefaultAppendixPrototypeInfo())
        {
            const AppendixPrototypeInfo* prototypeInfo = GetPrototypeInfo();
            m_appendixType = prototypeInfo ? prototypeInfo->m_appendixType : AppendixPrototypeInfo::AppendixType_Default;
            m_lpName = prototypeInfo ? prototypeInfo->m_lpName : "";
            m_thornForce = prototypeInfo ? prototypeInfo->m_thornForce : 1.0f;
            m_scale = prototypeInfo ? prototypeInfo->m_scale : 1.0f;
        }

        Appendix::Appendix(AppendixPrototypeInfo* prototypeInfo)
            : Gun(*prototypeInfo)
        {
            if (prototypeInfo) {
                m_appendixType = prototypeInfo->m_appendixType;
                m_lpName = prototypeInfo->m_lpName;
                m_thornForce = prototypeInfo->m_thornForce;
                m_scale = prototypeInfo->m_scale;
            }
            else {
                m_appendixType = AppendixPrototypeInfo::AppendixType_Default;
                m_lpName = "";
                m_thornForce = 1.0f;
                m_scale = 1.0f;
            }
        }

        Appendix::~Appendix() {
            ClearAppendices();
        }

        m3d::Object* Appendix::Clone() {
            m3d::Object* clonedObject = Gun::Clone();
            if (!clonedObject) {
                return nullptr;
            }

            if (hta::ai::Gun* clonedGun = clonedObject->cast<hta::ai::Gun>()) {
                if (clonedGun->GetClass()->IsKindOf(&Appendix::m_classAppendix)) {
                    Appendix* appendix = reinterpret_cast<Appendix*>(clonedGun);
                    appendix->m_appendixType = m_appendixType;
                    appendix->m_lpName = m_lpName;
                    appendix->m_thornForce = m_thornForce;
                    appendix->m_scale = m_scale;
                }
            }
            return clonedObject;
        }

        m3d::Object* Appendix::CreateObject() {
            ::EnsureAppendixClassRegistered();

            void* objectMemory = hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(Appendix), 0, 0);
            if (!objectMemory) {
                return nullptr;
            }

            Appendix* object = reinterpret_cast<Appendix*>(objectMemory);
            ::new (object) Appendix(&GetDefaultAppendixPrototypeInfo());
            return object;
        }

        m3d::Class* Appendix::GetBaseClass() {
            // The base of Appendix is Gun, so this MUST return Gun's class object,
            // NOT Gun::GetBaseClass() (which returns Gun's *own* base, VehiclePart).
            // Returning VehiclePart here would make IsKindOf(Gun) false for a thorn,
            // so ai::Town::GetWorkshopByObject() fails to find a workshop and the
            // shop reports "Нельзя купить" (GetPriceSmart -> -3). The engine never
            // links the &Gun::m_classGun static into this DLL, so reference the
            // real engine class object by its VA (RVA 0x602354 + image base).
            return reinterpret_cast<m3d::Class*>(0x00A02354);  // &ai::Gun::m_classGun
        }

        m3d::Class* Appendix::GetClass() const {
            return &m_classAppendix;
        }

        const AppendixPrototypeInfo* Appendix::GetPrototypeInfo() const {
            return reinterpret_cast<const AppendixPrototypeInfo*>(Gun::GetPrototypeInfo());
        }

        void Appendix::_InternalCreateVisualPart() {
            // Meridian never builds the appendix's own visual: Appendix::Appendix sets
            // PhysicBody::m_createModel = false (m113 +0x145), which is honored by both
            // _ApplyCurrentModelName (no m_Node) and Gun::_CreateBarrelNode (no barrel),
            // so nothing ever renders at the thorn's own gun mount. HTA's PhysicBody
            // predates that flag, so emulate it: while mounted, create nothing and
            // destroy whatever an earlier code path left at the mount (the "thorn
            // inside the cabin"). A detached appendix (shop/inventory) keeps Gun's
            // visual so the garage thumbnail still renders.
            if (GetOwner()) {
                DestroyOwnVisual();
                return;
            }
            Gun::_InternalCreateVisualPart();
        }

        void Appendix::DestroyOwnVisual() {
            // Remove the barrel before m_Node: ~SgNode re-parents surviving children
            // onto the scene graph's orphan holder, frozen at their world transform and
            // re-linked into the render cells — that is exactly how thorns end up
            // hanging in mid-air.
            if (m_barrelNode) {
                if (hta::m3d::SceneGraph* graph = m_barrelNode->GetGraph()) {
                    graph->RemoveNode(m_barrelNode);
                }
                else if (hta::m3d::Object* parent = m_barrelNode->GetParent()) {
                    parent->RemoveChild(m_barrelNode);
                }
                m_barrelNode = nullptr;
            }

            if (m_Node) {
                if (hta::m3d::SceneGraph* graph = m_Node->GetGraph()) {
                    graph->RemoveNode(m_Node);
                }
                else if (hta::m3d::Object* parent = m_Node->GetParent()) {
                    parent->RemoveChild(m_Node);
                }
                m_Node = nullptr;
            }
        }

        void Appendix::LoadRuntimeValues(hta::m3d::cmn::XmlFile* xmlFile, const hta::m3d::cmn::XmlNode* xmlNode) {
            Gun::LoadRuntimeValues(xmlFile, xmlNode);
            if (!xmlNode || xmlNode->IsEmpty()) {
                return;
            }

            if (const char* loadPoint = xmlNode->GetAttribute("LoadPoint")) {
                m_lpName = loadPoint;
            }

            if (const char* thornForce = xmlNode->GetAttribute("ThornForce")) {
                m_thornForce = static_cast<float>(std::atof(thornForce));
            }

            if (const char* appendixType = xmlNode->GetAttribute("AppendixType")) {
                m_appendixType = static_cast<int32_t>(std::atoi(appendixType));
            }

            if (const char* scale = xmlNode->GetAttribute("Scale")) {
                m_scale = static_cast<float>(std::atof(scale));
            }
        }

        void Appendix::SaveRuntimeValues(hta::m3d::cmn::XmlFile* xmlFile, hta::m3d::cmn::XmlNode* xmlNode) const {
            Gun::SaveRuntimeValues(xmlFile, xmlNode);
            if (!xmlNode) {
                return;
            }

            const hta::ai::AppendixPrototypeInfo* proto = GetPrototypeInfo();
            const int32_t appendixType = m_appendixType;
            const float thornForce = m_thornForce;
            const char* loadPoint = m_lpName.m_charPtr;

            xmlNode->SetAttribute("AppendixType", hta::CStr(appendixType).c_str());
            xmlNode->SetAttribute("ThornForce", hta::CStr(thornForce).c_str());
            xmlNode->SetAttribute("Scale", hta::CStr(m_scale).c_str());
            if (loadPoint && loadPoint[0] != '\0') {
                xmlNode->SetAttribute("LoadPoint", loadPoint);
            }
        }

        bool Appendix::isLookAtPoint(const CVector& lookAt, float eps) const {
            return Gun::isLookAtPoint(lookAt, eps);
        }

        void Appendix::_LaunchShells() {
            Gun::_LaunchShells();
        }

        void Appendix::ClearAppendices() {
            for (hta::m3d::SgNode* node : m_appendices) {
                if (!node)
                    continue;

                // Mirror the original: detach AND release the cloned node through the
                // scene graph (plain RemoveChild would leak the clone).
                if (hta::m3d::SceneGraph* graph = node->GetGraph()) {
                    graph->RemoveNode(node);
                }
                else if (hta::m3d::Object* parent = node->GetParent()) {
                    parent->RemoveChild(node);
                }
            }

            m_appendices.clear();
            m_lpNums.clear();
        }

        void Appendix::SetDependantCfg(hta::m3d::SgNode* parentNode, hta::m3d::AnimatedModel* mdl, hta::m3d::SgNode* node, int lpId) {
            if (!node)
                return;

            if (parentNode) {
                if (hta::m3d::Object* existingParent = node->GetParent()) {
                    if (existingParent != parentNode) {
                        existingParent->RemoveChild(node);
                        parentNode->AddChild(node);
                    }
                }
                else {
                    parentNode->AddChild(node);
                }
            }

            int actualLpId = lpId;
            if (mdl && m_lpName.m_charPtr && m_lpName.m_charPtr[0] != '\0') {
                const int namedLpId = mdl->GetLoadPointIdByName(m_lpName.m_charPtr);
                if (namedLpId >= 0) {
                    actualLpId = namedLpId;
                }
            }

            if (mdl && actualLpId >= 0) {
                hta::CMatrix loadPointMatrix = mdl->GetBoneMatrix(actualLpId);
                node->SetOriginAbs(loadPointMatrix.GetOrigin());
                node->SetRotation(hta::Quaternion(loadPointMatrix));
                node->SetScale(hta::CVector(m_scale));
            }
            else {
                node->SetScale(hta::CVector(m_scale));
            }

            m_appendices.push_back(node);
            m_lpNums.push_back(actualLpId);
        }

        void Appendix::ReconstructCallback() {
            hta::ai::PhysicObj* owner = GetOwner();
            if (!owner) return;
            if (!owner->GetClass()->IsKindOf("ComplexPhysicObj")) return;
            hta::ai::ComplexPhysicObj* vehicle = static_cast<hta::ai::ComplexPhysicObj*>(owner);

            if (owner->GetClass()->IsKindOf("Vehicle")) {
                SetVehicleThornForce(static_cast<hta::ai::Vehicle*>(owner), m_thornForce);
            }

            // While mounted the appendix must not have a visual of its own (see
            // _InternalCreateVisualPart); enforce it on every respread to cover parts
            // that built one before being mounted (shop purchase -> SetPartByName).
            DestroyOwnVisual();
            ClearAppendices();

            const bool hasLpName = m_lpName.m_charPtr && m_lpName.m_charPtr[0] != '\0';
            if (!hasLpName) return;

            using CreateNodeFn = hta::m3d::SgNode*(__fastcall*)(hta::CStr*, void*, hta::CVector*, void*, bool);
            auto createNodeFn = reinterpret_cast<CreateNodeFn>(0x006173B0);
            hta::CStr* modelname = reinterpret_cast<hta::CStr*>(reinterpret_cast<uint8_t*>(this) + 0xC0);

            for (auto it = vehicle->m_vehicleParts.begin(); it != vehicle->m_vehicleParts.end(); ++it) {
                hta::ai::VehiclePart* part = (*it).second;
                if (!part || part == static_cast<hta::ai::VehiclePart*>(this)) continue;

                hta::m3d::AnimatedModel* mdl = part->GetModel();
                if (!mdl) continue;

                for (int lpIndex = 1; ; ++lpIndex) {
                    char lpName[256];
                    _snprintf(lpName, sizeof(lpName), "%s%d", m_lpName.m_charPtr, lpIndex);
                    lpName[sizeof(lpName) - 1] = '\0';

                    const int lpId = mdl->GetLoadPointIdByName(lpName);
                    if (lpId < 0) break;

                    if (part->m_suppressedLPs.find(lpId) != part->m_suppressedLPs.end()) continue;

                    hta::CVector lpScale(m_scale, m_scale, m_scale);
                    hta::m3d::SgNode* node = createNodeFn(modelname, nullptr, &lpScale, nullptr, false);
                    if (!node) break;

                    if (part->m_Node) {
                        part->m_Node->AddChild(node);
                    }

                    const hta::CMatrix lpMatrix = mdl->GetBoneMatrix(lpId);
                    node->SetOriginAbs(lpMatrix.GetOrigin());

                    hta::Quaternion q;
                    using FromMatrixFn = void(__thiscall*)(hta::Quaternion*, const hta::CMatrix*);
                    reinterpret_cast<FromMatrixFn>(0x005FFAF0)(&q, &lpMatrix);
                    node->SetRotation(q);
                    node->SetScale(hta::CVector(m_scale));

                    m_appendices.push_back(node);
                    m_lpNums.push_back(lpId);
                }
            }
        }

        void Appendix::DeattachCallback() {
            if (hta::ai::PhysicObj* owner = GetOwner()) {
                if (owner->GetClass()->IsKindOf("Vehicle")) {
                    hta::ai::Vehicle* vehicle = static_cast<hta::ai::Vehicle*>(owner);
                    ClearVehicleThornForce(vehicle);
                }
            }
            ClearAppendices();
            // Re-arm the engine's "needs visual" gate (Obj+0x51): once detached the
            // owner is gone, so the next CreateVisualPart takes the Gun path in
            // _InternalCreateVisualPart and rebuilds the item's own model.
            m_bMustCreateVisualPart = true;
        }

        m3d::Class Appendix::m_classAppendix {
            "Appendix",
            sizeof(Appendix),
            &Appendix::CreateObject,
            &Appendix::GetBaseClass,
            0,
            nullptr,
            nullptr
        };

        void* Appendix::operator new(size_t size) {
            return m3d::Kernel::Instance()->g_mar.AllocMem(size, "", 0);
        };

        void Appendix::operator delete(void* data) {
            m3d::Kernel::Instance()->g_mar.FreeMem(data, "", 0);
        };
    }
}

namespace kraken::ext::ai {
    hta::ai::PrototypeInfo* CreateAppendixPrototypeInfo(const hta::CStr& className) {
        if (!(className == "Appendix")) {
            return nullptr;
        }

        EnsureAppendixClassRegistered();

        void* objectMemory = hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(hta::ai::AppendixPrototypeInfo), 0, 0);
        if (!objectMemory) {
            return nullptr;
        }

        auto* prototype = reinterpret_cast<hta::ai::AppendixPrototypeInfo*>(objectMemory);
        ::new (prototype) hta::ai::AppendixPrototypeInfo();
        prototype->m_className = "Appendix";
        prototype->m_appendixType = hta::ai::AppendixPrototypeInfo::AppendixType_Default;
        prototype->m_thornForce = 1.0f;
        prototype->m_lpName = "";
        prototype->m_scale = 1.0f;
        return prototype;
    }

    float GetVehicleThornForce(hta::ai::Vehicle* vehicle) {
        return ::GetVehicleThornForce(vehicle);
    }
}

namespace {
    // Re-mounts all currently-equipped appendices on a vehicle. Each ReconstructCallback
    // starts with ClearAppendices(), so calling this is idempotent — safe on equip,
    // unequip, and full visual rebuilds.
    void RespreadVehicleAppendices(hta::ai::ComplexPhysicObj* self) {
        if (!self) {
            return;
        }

        for (auto it = self->m_vehicleParts.begin(); it != self->m_vehicleParts.end(); ++it) {
            hta::ai::VehiclePart* part = (*it).second;
            if (!part || !part->GetClass()) {
                continue;
            }
            if (part->GetClass()->IsKindOf(&hta::ai::Appendix::m_classAppendix)) {
                static_cast<hta::ai::Appendix*>(part)->ReconstructCallback();
            }
        }
    }

    // --- Hook A: full vehicle visual rebuild (load / respawn) ----------------------
    // ai::ComplexPhysicObj::_InternalCreateVisualPart (RVA 0x2bfeb0) builds every part's
    // visual (incl. each Appendix's barrel node). Vehicle::_InternalCreateVisualPart calls
    // it directly at RVA 0x1eb38c — we retarget that call site (ChangeCall) below.
    constexpr uintptr_t kComplexCreateVisual = 0x006BFEB0;

    void __fastcall ComplexCreateVisual_Hook(hta::ai::ComplexPhysicObj* self, void* /*edx*/) {
        using Fn = void(__fastcall*)(hta::ai::ComplexPhysicObj*, void*);
        reinterpret_cast<Fn>(kComplexCreateVisual)(self, nullptr);
        RespreadVehicleAppendices(self);
    }

    // --- Hook B: in-game part add/remove (garage equip / unequip) -------------------
    // ai::Vehicle::SetPartByName (RVA 0x1e6020) is the per-change path used when a part is
    // mounted or moved back to the inventory; it does NOT rebuild the whole vehicle visual,
    // so Hook A never fires for it. We trampoline it: run the original, then respread, which
    // clears a removed thorn's lingering clones (and re-mounts on equip/cabin swap).
    static uint8_t s_setPartTrampoline[16];

    void __fastcall SetPartByName_Hook(hta::ai::ComplexPhysicObj* self, void* edx,
                                       hta::CStr* partName, hta::ai::VehiclePart* part, bool bUnsafe) {
        // Meridian's only DeattachCallback dispatch site is SetPartByName's detach
        // path (m113 complexphysicobj.cpp:904), fired while the part is still owned
        // and mounted. Mirror it here for an appendix being detached or replaced —
        // its clones hang under OTHER parts' nodes, so the post-call respread alone
        // would never reach a part that is no longer in m_vehicleParts.
        if (partName) {
            for (auto it = self->m_vehicleParts.begin(); it != self->m_vehicleParts.end(); ++it) {
                if (!((*it).first == *partName)) continue;
                hta::ai::VehiclePart* oldPart = (*it).second;
                if (oldPart && oldPart != part && oldPart->GetClass() &&
                    oldPart->GetClass()->IsKindOf(&hta::ai::Appendix::m_classAppendix)) {
                    static_cast<hta::ai::Appendix*>(oldPart)->DeattachCallback();
                }
                break;
            }
        }

        using Fn = void(__fastcall*)(hta::ai::ComplexPhysicObj*, void*, hta::CStr*, hta::ai::VehiclePart*, bool);
        reinterpret_cast<Fn>(static_cast<void*>(s_setPartTrampoline))(self, edx, partName, part, bUnsafe);
        RespreadVehicleAppendices(self);
    }

    // --- Hook C: ~ComplexPhysicObj (RVA 0x2c2ec0) ------------------------------------
    // HTA's destructor deletes VehicleParts without calling DeattachCallback. LP_THORN
    // ServerControlledNodes (children of part->m_Node) are NOT freed with the vehicle
    // hierarchy — they linger and render as floating spikes. We clean them up here,
    // before the destructor erases m_vehicleParts and frees the parts.
    // First 5 bytes: 51 56 57 8B F9 (push ecx; push esi; push edi; mov edi,ecx) — all
    // position-independent, safe to copy into the trampoline stub.
    static uint8_t s_complexDtorTrampoline[16];

    void __fastcall ComplexPhysicObjDtor_Hook(hta::ai::ComplexPhysicObj* self, void* /*edx*/) {
        for (auto it = self->m_vehicleParts.begin(); it != self->m_vehicleParts.end(); ++it) {
            hta::ai::VehiclePart* part = (*it).second;
            if (!part || !part->GetClass()) continue;
            if (part->GetClass()->IsKindOf(&hta::ai::Appendix::m_classAppendix)) {
                static_cast<hta::ai::Appendix*>(part)->ClearAppendices();
            }
        }
        // The thornForce side-map is keyed by Vehicle*; erase unconditionally so a
        // later allocation reusing this address can't inherit the dead vehicle's
        // thorn damage. (Erasing a key that was never inserted is a no-op.)
        ClearVehicleThornForce(reinterpret_cast<hta::ai::Vehicle*>(self));

        using Fn = void(__fastcall*)(hta::ai::ComplexPhysicObj*, void*);
        reinterpret_cast<Fn>(static_cast<void*>(s_complexDtorTrampoline))(self, nullptr);
    }

    // --- Hook D: VehiclePart::DefineSuppressedLPs trampoline (VA 0x006D3AF0) ---------
    // Meridian's second ReconstructCallback broadcast site (m113 RVA 0x4512b5):
    // DefineSuppressedLPs runs in every part's _InternalCreateVisualPart, in
    // _OnDurabilityValueAfterChange (damage model swap) and in BreakModel, and m113
    // re-broadcasts ReconstructCallback to all of the owner's parts right after it.
    // The damage paths destroy the part's old node; without an immediate respread the
    // thorn clones parented under it get orphaned by ~SgNode and freeze in mid-air.
    // First 5 bytes: 83 EC 28 53 57 (sub esp,0x28; push ebx; push edi) — all
    // position-independent, safe to copy into the trampoline stub.
    static uint8_t s_defineSuppressedLPsTrampoline[16];

    void __fastcall DefineSuppressedLPs_Hook(hta::ai::VehiclePart* self, void* /*edx*/) {
        using Fn = void(__fastcall*)(hta::ai::VehiclePart*, void*);
        reinterpret_cast<Fn>(static_cast<void*>(s_defineSuppressedLPsTrampoline))(self, nullptr);

        hta::ai::PhysicObj* owner = self->GetOwner();
        if (owner && owner->GetClass() && owner->GetClass()->IsKindOf("ComplexPhysicObj")) {
            RespreadVehicleAppendices(static_cast<hta::ai::ComplexPhysicObj*>(owner));
        }
    }

    // --- Fix: ComplexPhysicObj::_TearOffPart null-deref on Appendix parts -------------
    // When a vehicle explodes it tears off each part (Vehicle::_EvaluateToDead -> Flow ->
    // _TearOffPart, RVA 0x2c0600). For a part that IsKindOf(Gun) the engine re-parents the
    // barrel node off the part's m_Node:
    //     call Gun::GetBarrelNode          ; eax = barrel
    //     mov  ecx,[esi+0x124]             ; ecx = m_Node
    //     mov  edx,[ecx]                   ; <-- AV: m_Node is null
    // A mounted Appendix has m_Node == null (DestroyOwnVisual emulates m113's
    // m_createModel=false), so this crashes (0x006C068E, random — only on a hit strong
    // enough to tear the thorn part off). m113's _TearOffPart (RVA 0x439ba0) guards this:
    // right after GetBarrelNode it does `test edi,edi; je <skip>` and skips the whole
    // barrel branch when the barrel is null. HTA dropped that guard. We restore it with a
    // detour at the m_Node load (the guard doesn't fit inline): if the barrel (eax) is
    // null, jump to the branch's `pop edi` (0x006C06D8, balances the push edi at
    // 0x006C0680); otherwise run the displaced `mov ecx,[esi+0x124]` and continue. Normal
    // guns (non-null barrel) are unaffected.
    static uint8_t s_tearOffPartCave[32];

    void InstallTearOffPartFix() {
        void* const patchSite = reinterpret_cast<void*>(0x006C0688); // mov ecx,[esi+0x124] (6 bytes)
        const uintptr_t resumeAddr = 0x006C068E;                     // mov edx,[ecx]
        const uintptr_t skipAddr   = 0x006C06D8;                     // pop edi (gun branch tail)

        DWORD oldProt;
        VirtualProtect(s_tearOffPartCave, sizeof(s_tearOffPartCave), PAGE_EXECUTE_READWRITE, &oldProt);

        size_t i = 0;
        s_tearOffPartCave[i++] = 0x85; s_tearOffPartCave[i++] = 0xC0;   // test eax, eax
        s_tearOffPartCave[i++] = 0x0F; s_tearOffPartCave[i++] = 0x84;   // je rel32
        int32_t* jeRel = reinterpret_cast<int32_t*>(&s_tearOffPartCave[i]); i += 4;
        s_tearOffPartCave[i++] = 0x8B; s_tearOffPartCave[i++] = 0x8E;   // mov ecx,[esi+0x124]
        s_tearOffPartCave[i++] = 0x24; s_tearOffPartCave[i++] = 0x01;
        s_tearOffPartCave[i++] = 0x00; s_tearOffPartCave[i++] = 0x00;
        s_tearOffPartCave[i++] = 0xE9;                                  // jmp rel32
        int32_t* jmpRel = reinterpret_cast<int32_t*>(&s_tearOffPartCave[i]); i += 4;

        const uintptr_t caveBase = reinterpret_cast<uintptr_t>(s_tearOffPartCave);
        *jeRel  = static_cast<int32_t>(skipAddr   - (caveBase + 8));    // IP after je  = cave+8
        *jmpRel = static_cast<int32_t>(resumeAddr - (caveBase + i));    // IP after jmp = cave+19

        uint8_t patch[6];
        patch[0] = 0xE9;                                               // jmp cave
        *reinterpret_cast<int32_t*>(&patch[1]) =
            static_cast<int32_t>(caveBase - (reinterpret_cast<uintptr_t>(patchSite) + 5));
        patch[5] = 0x90;                                              // nop (6th displaced byte)
        kraken::routines::Patch(patchSite, patch, sizeof(patch));
    }
}

namespace kraken::ext::ai {
    void ApplyReconstructHook() {
        // Hook A: retarget the call inside Vehicle::_InternalCreateVisualPart (RVA 0x1eb38c)
        // that invokes ComplexPhysicObj::_InternalCreateVisualPart.
        kraken::routines::ChangeCall(reinterpret_cast<void*>(0x005EB38C),
                                     reinterpret_cast<void*>(&ComplexCreateVisual_Hook));

        // Hook B: trampoline ai::Vehicle::SetPartByName (RVA 0x1e6020). First 5 bytes
        // (83 EC 6C 53 55 = sub esp,0x6c; push ebx; push ebp) are position-independent.
        void* const origSetPart = reinterpret_cast<void*>(0x005E6020);
        DWORD oldProt;
        VirtualProtect(s_setPartTrampoline, sizeof(s_setPartTrampoline), PAGE_EXECUTE_READWRITE, &oldProt);
        std::memcpy(s_setPartTrampoline, origSetPart, 5);
        s_setPartTrampoline[5] = 0xE9;
        const uintptr_t jmpSrc = reinterpret_cast<uintptr_t>(s_setPartTrampoline) + 10;
        const uintptr_t jmpTarget = reinterpret_cast<uintptr_t>(origSetPart) + 5;
        *reinterpret_cast<int32_t*>(s_setPartTrampoline + 6) = static_cast<int32_t>(jmpTarget - jmpSrc);
        kraken::routines::Redirect(5, origSetPart, reinterpret_cast<void*>(&SetPartByName_Hook));

        // Hook C: trampoline ~ComplexPhysicObj (RVA 0x2c2ec0). First 5 bytes
        // (51 56 57 8B F9 = push ecx; push esi; push edi; mov edi,ecx) are position-independent.
        void* const origDtor = reinterpret_cast<void*>(0x006C2EC0);
        VirtualProtect(s_complexDtorTrampoline, sizeof(s_complexDtorTrampoline), PAGE_EXECUTE_READWRITE, &oldProt);
        std::memcpy(s_complexDtorTrampoline, origDtor, 5);
        s_complexDtorTrampoline[5] = 0xE9;
        const uintptr_t dtorJmpSrc = reinterpret_cast<uintptr_t>(s_complexDtorTrampoline) + 10;
        const uintptr_t dtorJmpTarget = reinterpret_cast<uintptr_t>(origDtor) + 5;
        *reinterpret_cast<int32_t*>(s_complexDtorTrampoline + 6) = static_cast<int32_t>(dtorJmpTarget - dtorJmpSrc);
        kraken::routines::Redirect(5, origDtor, reinterpret_cast<void*>(&ComplexPhysicObjDtor_Hook));

        // Hook D: trampoline ai::VehiclePart::DefineSuppressedLPs (RVA 0x2d3af0).
        // First 5 bytes (83 EC 28 53 57 = sub esp,0x28; push ebx; push edi) are
        // position-independent.
        void* const origDefineLPs = reinterpret_cast<void*>(0x006D3AF0);
        VirtualProtect(s_defineSuppressedLPsTrampoline, sizeof(s_defineSuppressedLPsTrampoline), PAGE_EXECUTE_READWRITE, &oldProt);
        std::memcpy(s_defineSuppressedLPsTrampoline, origDefineLPs, 5);
        s_defineSuppressedLPsTrampoline[5] = 0xE9;
        const uintptr_t dlpJmpSrc = reinterpret_cast<uintptr_t>(s_defineSuppressedLPsTrampoline) + 10;
        const uintptr_t dlpJmpTarget = reinterpret_cast<uintptr_t>(origDefineLPs) + 5;
        *reinterpret_cast<int32_t*>(s_defineSuppressedLPsTrampoline + 6) = static_cast<int32_t>(dlpJmpTarget - dlpJmpSrc);
        kraken::routines::Redirect(5, origDefineLPs, reinterpret_cast<void*>(&DefineSuppressedLPs_Hook));

        // Crash fix: guard _TearOffPart against the Appendix's null m_Node (see comment
        // on InstallTearOffPartFix). Mirrors m113's barrel-null check.
        InstallTearOffPartFix();
    }
}

