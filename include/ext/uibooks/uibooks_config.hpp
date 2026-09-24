#pragma once

#include <optional>

#include "ext/uibooks/uibooks.hpp"

namespace kraken::ext::uibooks::config {
    void LoadBookModes();
    void ClearBookModes();
    void RegisterBookMode(const char* nameId, BookMode mode);
    std::optional<BookMode> FindBookMode(const char* nameId);
}
