#pragma once

namespace m3d {
    class Object;
}

class dContact;

namespace kraken::fix::watereffect {
    int __fastcall CollidePOAndWater(m3d::Object* obj1, m3d::Object* obj2, dContact* contacts, unsigned int* numContacts, bool reverse);
    void Apply();
}