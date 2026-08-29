#pragma once

#include <cstdint>
#include <vector>

#include "ext/uibooks/uibooks.hpp"

namespace kraken::ext::uibooks::pagination {
    struct PagePlan {
        std::vector<int32_t> start;
        std::vector<int32_t> end;
    };

    PagePlan Build(const std::vector<int32_t>& lineRows,
                   const std::vector<bool>& lineBreak,
                   BookMode mode, int32_t rowsPerPage);

    int32_t VisualRows(const std::vector<int32_t>& pageStart,
                       const std::vector<int32_t>& pageEnd,
                       const std::vector<int32_t>& lineRows,
                       int32_t page);
}
