#include "ext/uibooks/uibooks_pagination.hpp"

#include <algorithm>

namespace kraken::ext::uibooks::pagination {
    namespace {
        bool HasBreakAfter(const std::vector<bool>& lineBreak, int32_t line, int32_t lineCount) {
            return line >= 0 && line < lineCount - 1
                && line < static_cast<int32_t>(lineBreak.size()) && lineBreak[(size_t) line];
        }

        void AppendBlock(PagePlan& plan, const std::vector<int32_t>& lineRows,
                         int32_t first, int32_t last, int32_t rowsPerPage) {
            for (int32_t start = first; start < last;) {
                int32_t end = start;
                int32_t used = 0;
                while (end < last) {
                    const int32_t needed = (std::max)(1, lineRows[(size_t) end]);
                    if (end > start && used + needed > rowsPerPage)
                        break;
                    used += needed;
                    ++end;
                }
                plan.start.push_back(start);
                plan.end.push_back(end);
                start = end;
            }
        }
    }

    PagePlan Build(const std::vector<int32_t>& lineRows,
                   const std::vector<bool>& lineBreak,
                   BookMode mode, int32_t rowsPerPage) {
        PagePlan plan;
        const int32_t lineCount = static_cast<int32_t>(lineRows.size());
        if (lineCount <= 0)
            return plan;

        if (mode == BookMode::Scroll) {
            int32_t start = 0;
            for (int32_t line = 0; line < lineCount; ++line) {
                if (HasBreakAfter(lineBreak, line, lineCount)) {
                    plan.start.push_back(start);
                    plan.end.push_back(line + 1);
                    start = line + 1;
                }
            }
            plan.start.push_back(start);
            plan.end.push_back(lineCount);
            return plan;
        }

        const int32_t capacity = (std::max)(1, rowsPerPage);
        int32_t blockStart = 0;
        for (int32_t line = 0; line < lineCount; ++line) {
            if (!HasBreakAfter(lineBreak, line, lineCount))
                continue;
            const int32_t blockEnd = line + 1;
            AppendBlock(plan, lineRows, blockStart, blockEnd, capacity);
            blockStart = blockEnd;
        }
        AppendBlock(plan, lineRows, blockStart, lineCount, capacity);
        return plan;
    }

    int32_t VisualRows(const std::vector<int32_t>& pageStart,
                       const std::vector<int32_t>& pageEnd,
                       const std::vector<int32_t>& lineRows,
                       int32_t page) {
        if (page < 0 || page >= static_cast<int32_t>(pageStart.size())
            || page >= static_cast<int32_t>(pageEnd.size()))
            return 0;

        const int32_t first = (std::max)(0, pageStart[(size_t) page]);
        const int32_t last = (std::min)(static_cast<int32_t>(lineRows.size()), pageEnd[(size_t) page]);
        int32_t rows = 0;
        for (int32_t line = first; line < last; ++line)
            rows += (std::max)(1, lineRows[(size_t) line]);
        return rows;
    }
}
