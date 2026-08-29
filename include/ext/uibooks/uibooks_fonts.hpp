#pragma once

#include <cstdint>

#include "ext/uibooks/uibooks_state.hpp"

namespace kraken::ext::uibooks::fonts {
    int32_t EnsureStyleFont(BookState& state, uint32_t style);
    bool UsesSyntheticBold(const BookState& state, uint32_t style);
}
