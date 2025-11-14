#pragma once

#include "hta/m3d/ui/Wnd.hpp"
#include "hta/ai/GeomRepositoryItem.hpp"

class DragSlot;

namespace hta::m3d::ui {
    struct DragDropItemsWnd : public m3d::ui::Wnd { /* Size=0x224 */
        enum DragStyle : int32_t {
            DRAGSTYLE_VISIBLE_SRC = 0x0000,
            DRAGSTYLE_HIDDEN_SRC = 0x0001,
        };
        /* 0x0000: fields for m3d::ui::Wnd */
        /* 0x0220 */ DragDropItemsWnd::DragStyle m_dragStyle;
        inline static DragSlot*& m_dragSlot = *(DragSlot**)0x00A42120;
        static m3d::Class m_classDragDropItemsWnd;
        
        virtual BoundsBase<float> GeomToWndBounds(const BoundsBase<int>&);
        virtual void Enable(bool);
        virtual int32_t CanAddDragItem(bool);
        virtual int32_t OnMouseButton0(uint32_t, const PointBase<float>&);
        virtual int32_t OnMouseMove(const PointBase<float>&, const PointBase<float>&);
        virtual int32_t OnMouseOut();
        virtual int32_t OnMouseIn();
        virtual int32_t GameDataUpdate(void*, int32_t);
        virtual int32_t OnBeforeRemoveFromWndStation();
        virtual int32_t OnWndNotify(m3d::ui::Wnd*, uint32_t, uint32_t, const m3d::AIParam&);
        virtual int32_t CreateDragSlotFromWndPt(const PointBase<float>&);
        virtual int32_t StartDrag();
        virtual void Drag(const PointBase<float>&);
        virtual int32_t Drop(const PointBase<float>&);
        virtual void UpdateDragSlotSize();
        virtual void UpdateDragSlotPosition(const PointBase<float>&);
        virtual void PlayStartDragSound();
        virtual void PlayDropSound();
        virtual ai::GeomRepositoryItem GetItemFromOrigin(const PointBase<float>&);
        virtual int32_t AddItem(const ai::GeomRepositoryItem&);
        virtual void OnUpdateWhileDrag(const PointBase<float>&);
        virtual void OnUpdateWhileNoDrag(const PointBase<float>&);
        virtual void OnDragOut();
        virtual int32_t OnDragRemove();
        virtual int32_t GiveUpItem(const ai::GeomRepositoryItem&, m3d::ui::Wnd*);
        virtual void HideDragSrc();
        virtual void ShowDragSrc();
        DragDropItemsWnd(const DragDropItemsWnd&);
        DragDropItemsWnd();
        virtual ~DragDropItemsWnd();
        virtual m3d::Class* GetRtClass() const;

        static bool IsDragging();
        static ai::GeomRepositoryItem GetDragItem();
        static void RemoveDragSlot();
        static m3d::Class* GetBaseClass();
    };
    static_assert(sizeof(DragDropItemsWnd) == 0x224);
}