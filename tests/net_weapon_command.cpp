#include "net/weapon_command.hpp"

#include <array>
#include <iostream>

int main()
{
    using namespace kraken::net;
    WeaponCommand original{42, 7, 99, 3, true, 17};
    WeaponCommand decoded{};
    std::array<Byte, kWeaponCommandWireSize> wire{};
    if (encode_weapon_command(original, wire) != WeaponCommandCodecError::None ||
        decode_weapon_command(wire, decoded) != WeaponCommandCodecError::None ||
        decoded.entity_id != original.entity_id ||
        decoded.sequence != original.sequence || decoded.gun_id != original.gun_id ||
        !decoded.trigger_held || decoded.target_entity_id != original.target_entity_id)
        return 1;

    std::array<Byte, kWeaponCommandWireSize - 1> short_wire{};
    if (encode_weapon_command(original, short_wire) !=
            WeaponCommandCodecError::OutputTooSmall ||
        decode_weapon_command(short_wire, decoded) !=
            WeaponCommandCodecError::InputSizeMismatch)
        return 2;

    original.gun_id = -1;
    if (encode_weapon_command(original, wire) != WeaponCommandCodecError::InvalidGunId)
        return 3;
    original.gun_id = 3;
    if (encode_weapon_command(original, wire) != WeaponCommandCodecError::None)
        return 4;
    wire[6] = static_cast<Byte>(2);
    if (decode_weapon_command(wire, decoded) != WeaponCommandCodecError::BadFlags)
        return 5;
    wire[6] = static_cast<Byte>(1);
    wire[0] = Byte{};
    if (decode_weapon_command(wire, decoded) != WeaponCommandCodecError::BadMagic)
        return 6;

    std::cout << "weapon command tests passed\n";
    return 0;
}
