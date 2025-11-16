#define LOGGER "fileserver"

#include "ext/logger.hpp"
#include "fix/fileserver.hpp"
#include "routines.hpp"

#include <filesystem>

#include "hta/m3d/fs/FileServer.hpp"

namespace kraken::fix::fileserver {
    auto FileServer_AddFile = (void (__fastcall*)(hta::m3d::fs::FileServer* self, void* _, const char* path))0x00663690;

    int __fastcall Fixed_AddFile(hta::m3d::fs::FileServer* self, void* _, const char* folder, const char* mask, bool recursive) {
        auto source = std::filesystem::path(folder);

        for (const auto& path : std::filesystem::recursive_directory_iterator(source)) {
            if (path.is_regular_file()) {
                auto result = path.path();
                FileServer_AddFile(self, NULL, result.string().c_str());
            }
        }
        
        return 0;
    };

    void Apply(void) {
        LOG_INFO("Feature enabled");
        routines::Redirect(0x01E4, (void*) 0x00664440, (void*) &Fixed_AddFile);
    };
};