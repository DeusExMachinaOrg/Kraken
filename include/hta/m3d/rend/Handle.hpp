#pragma once

namespace m3d::rend {
    template<typename T>
    struct Handle{
        /* Size=0x4 */
        /* 0x0000 */ int32_t m_handle;

        Handle(const Handle<T>&);
        Handle();
        void SetInvalid();
        bool IsValid() const;
        bool operator==(const Handle<T>&) const;
        bool operator!=(const Handle<T>&) const;
    };
};