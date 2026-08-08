#include "net/input_command.hpp"

#include <array>
#include <iostream>
#include <limits>

int main()
{
    using namespace kraken::net;
    InputCommand command{2, 9, 7, 0.5f, 0.2f, 0.1f, true, true};
    InputCommand decoded{};
    std::array<Byte, kInputCommandWireSize> wire{};
    if (encode_input_command(command, wire) != InputCommandCodecError::None ||
        decode_input_command(wire, decoded) != InputCommandCodecError::None ||
        decoded.entity_id != 2 || decoded.sequence != 9 ||
        !decoded.handbrake || !decoded.request_unstuck)
        return 1;

    std::array<Byte, kInputCommandWireSize - 1> short_wire{};
    if (encode_input_command(command, short_wire) !=
            InputCommandCodecError::OutputTooSmall ||
        decode_input_command(short_wire, decoded) !=
            InputCommandCodecError::InputSizeMismatch)
        return 2;

    command.throttle = -1.0f;
    if (encode_input_command(command, wire) != InputCommandCodecError::None ||
        decode_input_command(wire, decoded) != InputCommandCodecError::None ||
        decoded.throttle != -1.0f)
        return 3;

    command.throttle = std::numeric_limits<float>::quiet_NaN();
    if (encode_input_command(command, wire) !=
        InputCommandCodecError::NonFiniteValue)
        return 4;
    command.throttle = 2.0f;
    if (encode_input_command(command, wire) !=
        InputCommandCodecError::ValueOutOfRange)
        return 5;
    command.throttle = 0.5f;
    if (encode_input_command(command, wire) != InputCommandCodecError::None)
        return 6;
    wire[33] = static_cast<Byte>(2);
    if (decode_input_command(wire, decoded) !=
        InputCommandCodecError::InvalidUnstuckRequest)
        return 7;
    wire[0] = Byte{};
    if (decode_input_command(wire, decoded) != InputCommandCodecError::BadMagic)
        return 8;
    std::cout << "input command tests passed\n";
    return 0;
}
