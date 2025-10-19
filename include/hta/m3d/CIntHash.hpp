#pragma once

namespace m3d {
    template<typename T>
    class CIntHash {
        /* Size=0x28 */
        char body[0x28];
        // WTF `stdext`????
        // 0x0000  private: stdext::hash_map<unsigned int,ui::Wnd *,stdext::hash_compare<unsigned int,std::less<unsigned int> >,std::allocator<std::pair<unsigned int const ,ui::Wnd *> > > m_hash;
    
        ~CIntHash<T>();
        void clear();
        void removeByKey(const uint32_t);
        bool getValueByKey(const uint32_t, T&) const;
        void addValueByKey(const uint32_t, T&);
        CIntHash(const CIntHash<T>&);
        CIntHash();
    };

};