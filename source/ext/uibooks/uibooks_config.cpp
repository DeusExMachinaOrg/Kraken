#define LOGGER "uibooks"

#include "ext/uibooks/uibooks_config.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "ext/logger.hpp"

#include "hta/CStr.hpp"
#include "hta/m3d/cmn/XmlFile.hpp"
#include "hta/m3d/cmn/XmlNode.hpp"
#include "hta/ref_ptr.hpp"

namespace kraken::ext::uibooks::config {
    namespace {
        constexpr const char* BOOKS_XML_PATH = "data\\if\\strings\\uibooks.xml";

        std::optional<std::unordered_map<std::string, BookMode>> g_xmlBookModes;
        std::unordered_map<std::string, BookMode> g_runtimeBookModes;

        std::optional<BookMode> ParseBookMode(const char* value) {
            if (value && _stricmp(value, "pages") == 0)
                return BookMode::Pages;
            if (value && _stricmp(value, "scroll") == 0)
                return BookMode::Scroll;
            return std::nullopt;
        }

        void ReadBookNode(const hta::m3d::cmn::XmlNode& node,
                          std::unordered_map<std::string, BookMode>& modes) {
            const char* nameId = node.GetAttribute("id");
            const char* modeValue = node.GetAttribute("mode");

            const std::optional<BookMode> mode = ParseBookMode(modeValue);
            if (!nameId || !*nameId || !mode.has_value()) {
                if (modeValue && *modeValue)
                    LOG_WARNING("Ignoring invalid book mode for '%s': '%s'",
                                nameId ? nameId : "", modeValue);
                return;
            }
            const bool inserted = modes.insert_or_assign(nameId, *mode).second;
            if (!inserted)
                LOG_WARNING("Duplicate book mode for '%s'; the last XML value wins", nameId);
        }

        void ReadBookNodes(hta::m3d::cmn::XmlFile& file,
                           hta::m3d::cmn::XmlNode& parent,
                           std::unordered_map<std::string, BookMode>& modes) {
            ref_ptr<hta::m3d::cmn::XmlNode> book =
                file.CreateNode(hta::m3d::cmn::XML_NODE_EMPTY, nullptr);
            if (!book || !parent.GetFirstChild(book, "book"))
                return;

            while (!book->IsEmpty()) {
                ReadBookNode(*book, modes);
                if (!book->GetNextSibling(book, "book"))
                    break;
            }
        }
    }

    void LoadBookModes() {
        if (g_xmlBookModes.has_value())
            return;

        // ReadXmlFile calls into the engine and is only safe after the game has
        // finished initializing its filesystem/UI services. Apply() runs earlier,
        // so the XML is loaded lazily on the first book selection instead.
        hta::CStr error;
        ref_ptr<hta::m3d::cmn::XmlFile> file =
            hta::m3d::cmn::ReadXmlFile(BOOKS_XML_PATH, &error);
        if (!file) {
            LOG_INFO("Book mode properties not loaded: '%s' (%s)", BOOKS_XML_PATH,
                     error.c_str());
            return;
        }

        ref_ptr<hta::m3d::cmn::XmlNode> root =
            file->CreateNode(hta::m3d::cmn::XML_NODE_EMPTY, nullptr);
        if (!root || !file->GetFirstChild(root, "resource")) {
            LOG_WARNING("Book mode properties not found in '%s'", BOOKS_XML_PATH);
            return;
        }
        std::unordered_map<std::string, BookMode> modes;
        ReadBookNodes(*file, *root, modes);
        g_xmlBookModes = std::move(modes);
        LOG_INFO("Loaded %u book mode properties from '%s'",
                 static_cast<unsigned>(g_xmlBookModes->size()), BOOKS_XML_PATH);
    }

    void ClearBookModes() {
        g_xmlBookModes.reset();
        g_runtimeBookModes.clear();
    }

    void RegisterBookMode(const char* nameId, BookMode mode) {
        if (!nameId || !*nameId)
            return;
        g_runtimeBookModes.insert_or_assign(nameId, mode);
    }

    std::optional<BookMode> FindBookMode(const char* nameId) {
        if (!nameId || !*nameId)
            return std::nullopt;
        const auto runtime = g_runtimeBookModes.find(nameId);
        if (runtime != g_runtimeBookModes.end())
            return runtime->second;
        if (!g_xmlBookModes.has_value())
            return std::nullopt;
        const auto xml = g_xmlBookModes->find(nameId);
        return xml == g_xmlBookModes->end() ? std::nullopt : std::optional<BookMode>(xml->second);
    }
}
