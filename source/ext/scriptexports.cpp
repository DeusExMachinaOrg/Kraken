#define LOGGER "scriptexports"

#include "ext/scriptexports.hpp"

#include <cstring>
#include <memory>
#include <vector>

#include "ext/logger.hpp"

#include "hta/m3d/Class.hpp"
#include "hta/m3d/ExportInfo.hpp"

namespace kraken::scriptexports {
    namespace {
        struct ExportTable {
            std::unique_ptr<hta::m3d::ExportInfo[]> entries;
        };

        std::vector<ExportTable> g_exportTables;
    }

    bool AddMethod(hta::m3d::Class& targetClass, const MethodInfo& method) {
        if (!method.name || !method.method) {
            LOG_ERROR("Cannot register an unnamed Lua method for %s", targetClass.m_className);
            return false;
        }

        if (targetClass.m_scriptHandle) {
            LOG_ERROR("Cannot register %s:%s after its Lua class table was built", targetClass.m_className, method.name);
            return false;
        }

        auto* source = targetClass.m_lExports;
        if (!source) {
            LOG_ERROR("Class %s has no Lua export table", targetClass.m_className);
            return false;
        }

        size_t count = 0;
        while (source[count].name) {
            if (std::strcmp(source[count].name, method.name) == 0) {
                LOG_INFO("Lua method %s:%s is already exported", targetClass.m_className, method.name);
                return true;
            }
            ++count;
        }

        ExportTable table;
        table.entries = std::make_unique<hta::m3d::ExportInfo[]>(count + 2);
        std::memcpy(table.entries.get(), source, count * sizeof(*source));
        table.entries[count] = {
            method.name,
            hta::m3d::METHOD,
            reinterpret_cast<void*>(method.method),
            nullptr,
            method.returns,
            method.params,
            method.description,
        };
        table.entries[count + 1] = {};

        targetClass.m_lExports = table.entries.get();
        g_exportTables.push_back(std::move(table));
        LOG_INFO("Exported Lua method %s:%s", targetClass.m_className, method.name);
        return true;
    }
}
