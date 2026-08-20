#include "net/native_object_archive.hpp"

#include "hta/ai/Obj.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/CStr.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/cmn/XmlFile.hpp"
#include "hta/m3d/cmn/XmlNode.hpp"
#include "hta/m3d/fs/RawFile.hpp"
#include "hta/native.hpp"
#include "hta/ref_ptr.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace {

using kraken::net::Byte;
using kraken::net::ByteView;
using kraken::net::NativeObjectArchiveChunk;
using kraken::net::NativeObjectArchiveDigest;
using kraken::net::NativeObjectArchiveErrorCode;
using kraken::net::NativeObjectArchiveResult;
using kraken::net::NativeObjectArchiveV2;

constexpr std::size_t kArchiveHeaderSize = 32;
constexpr std::size_t kArchiveDigestOffset = 24;
constexpr std::size_t kManifestEntryHeaderSize = 20;
constexpr std::size_t kVisualEntryHeaderSize = 24;
constexpr std::uint32_t kChunkWireMagic = 0x32434f4eu; // NOC2
constexpr std::size_t kChunkHeaderSize = 40;
constexpr float kMaxVisualComponent = 1'000'000.0f;

[[nodiscard]] NativeObjectArchiveResult result(
    const NativeObjectArchiveErrorCode error, std::string detail = {},
    const std::size_t size = 0, const NativeObjectArchiveDigest digest = 0)
{
    return {error, size, std::move(detail), digest};
}

void put_u16(Byte* const destination, const std::uint16_t value) noexcept
{
    destination[0] = static_cast<Byte>(value & 0xffu);
    destination[1] = static_cast<Byte>((value >> 8) & 0xffu);
}

void put_u32(Byte* const destination, const std::uint32_t value) noexcept
{
    for (std::size_t index = 0; index != 4; ++index)
        destination[index] = static_cast<Byte>(value >> (index * 8));
}

void put_u64(Byte* const destination, const std::uint64_t value) noexcept
{
    for (std::size_t index = 0; index != 8; ++index)
        destination[index] = static_cast<Byte>(value >> (index * 8));
}

[[nodiscard]] std::uint16_t get_u16(const Byte* const source) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(source[0])) |
           static_cast<std::uint16_t>(static_cast<std::uint8_t>(source[1]) << 8);
}

[[nodiscard]] std::uint32_t get_u32(const Byte* const source) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index != 4; ++index)
        value |= static_cast<std::uint32_t>(
                     static_cast<std::uint8_t>(source[index])) << (index * 8);
    return value;
}

[[nodiscard]] std::uint64_t get_u64(const Byte* const source) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index != 8; ++index)
        value |= static_cast<std::uint64_t>(
                     static_cast<std::uint8_t>(source[index])) << (index * 8);
    return value;
}

[[nodiscard]] bool checked_add(std::size_t& total, const std::size_t value) noexcept
{
    if (value > (std::numeric_limits<std::size_t>::max)() - total)
        return false;
    total += value;
    return true;
}

[[nodiscard]] bool ascii_control_free(const std::string_view value) noexcept
{
    for (const unsigned char byte : value) {
        if (byte == 0 || byte < 0x20u || byte == 0x7fu)
            return false;
    }
    return true;
}

[[nodiscard]] bool valid_stable_path(const std::string_view value,
                                     const bool allow_empty = false) noexcept
{
    if (value.empty())
        return allow_empty;
    if (value.size() > kraken::net::kNativeObjectArchiveMaxPathBytes ||
        value.front() == '/' || value.back() == '/' || value.find('\\') !=
        std::string_view::npos || !ascii_control_free(value))
        return false;
    std::size_t depth = 0;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t separator = value.find('/', begin);
        const std::size_t end = separator == std::string_view::npos
                                    ? value.size() : separator;
        const std::string_view component = value.substr(begin, end - begin);
        if (component.empty() || component == "." || component == ".." ||
            component.find(':') != std::string_view::npos)
            return false;
        ++depth;
        if (depth > kraken::net::kNativeObjectArchiveMaxManifestDepth)
            return false;
        if (separator == std::string_view::npos)
            break;
        begin = separator + 1;
    }
    return true;
}

[[nodiscard]] bool valid_text_field(
    const std::string_view value, const std::size_t maximum,
    const bool allow_empty = false) noexcept
{
    return (allow_empty || !value.empty()) && value.size() <= maximum &&
           ascii_control_free(value);
}

[[nodiscard]] bool identity_name(const std::string_view input) noexcept
{
    std::string lower;
    try {
        lower.assign(input);
    }
    catch (...) {
        return true;
    }
    for (char& character : lower)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    // This is an explicit policy. In particular, "Runtime" is not an
    // identity field: its visual subset is retained and validated below.
    constexpr std::array<std::string_view, 25> denied{{
        "objid", "objectid", "object_id", "parentid", "updatingobjid",
        "name", "localname", "belong",
        "event", "eventid", "eventrecipients", "script", "scripthandle",
        "ai", "aistate", "aiparam", "gameplay", "quest", "loot",
        "physics", "physicbody", "bodyid", "actionid", "prevactionid",
        "lastdamagesource"}};
    return std::find(denied.begin(), denied.end(), lower) != denied.end();
}

[[nodiscard]] std::string lower_ascii(std::string value);

[[nodiscard]] bool dropped_element_name(const std::string_view input) noexcept
{
    const std::string lower = lower_ascii(std::string(input));
    constexpr std::array<std::string_view, 7> denied{{
        "repository", "eventrecipients", "event", "physicbody", "ai",
        "path", "script"}};
    return std::find(denied.begin(), denied.end(), lower) != denied.end();
}

[[nodiscard]] bool dropped_runtime_attribute(
    const std::string_view input) noexcept
{
    const std::string lower = lower_ascii(std::string(input));
    constexpr std::array<std::string_view, 64> denied{{
        "linearvelocity", "angularvelocity", "targetid", "timeoutforreaimguns",
        "targetpos", "timeafterdeath", "numblownparts", "timeafterlastblow",
        "currentgear", "throttle", "realthrottle", "autobrake", "handbrake",
        "enginerpm", "currentdestination", "pathnum", "priority", "pathindex",
        "ismovingalongexternalpath", "canbedistractedfrommoving", "stoppagemode",
        "onoilmode", "insmokescreenmode", "turbothrottletime", "turbothrottlevalue",
        "immortalmode", "hidden", "movestatus", "cruisingspeed", "role",
        "recollectionid", "wasstuck", "prevpostocheckstuck", "timeouttocheckstuck",
        "pasttakingpos", "allowinventorymessage", "curangle", "animaction",
        "curanimtime", "health", "fuel", "targetpos", "curbarrelnum",
        "barrelnoderotation", "chargestate", "currentrechargingtime",
        "shellsincurrentcharge", "shellsinpool", "isfiring", "wasshot",
        "justshot", "questid"}};
    if (std::find(denied.begin(), denied.end(), lower) != denied.end())
        return true;
    return lower.rfind("mphealth", 0) == 0;
}

[[nodiscard]] bool is_runtime_container(const std::string_view input) noexcept
{
    std::string lower(input);
    for (char& character : lower)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    return lower == "runtime" || lower == "visualruntime" ||
           lower == "visualstate";
}

[[nodiscard]] bool is_visual_runtime_name(const std::string_view input) noexcept
{
    std::string lower(input);
    for (char& character : lower)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    constexpr std::array<std::string_view, 17> allowed{{
        "runtime", "visualruntime", "visualstate", "visual", "visibility",
        "visible", "enabled", "mesh", "material", "transform", "position",
        "rotation", "scale", "brokenpart", "color", "alpha", "visualpart"}};
    return std::find(allowed.begin(), allowed.end(), lower) != allowed.end();
}

[[nodiscard]] bool is_runtime_structural_name(
    const std::string_view input) noexcept
{
    const std::string lower = lower_ascii(std::string(input));
    constexpr std::array<std::string_view, 8> allowed{{
        "runtime", "visualruntime", "visualstate", "wheels", "wheelinfo",
        "wheel", "parts", "geom"}};
    return std::find(allowed.begin(), allowed.end(), lower) != allowed.end();
}

[[nodiscard]] bool is_visual_runtime_attribute(
    const std::string_view input) noexcept
{
    std::string lower(input);
    for (char& character : lower)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    constexpr std::array<std::string_view, 28> allowed{{
        "visible", "enabled", "name", "resource", "mesh", "material",
        "texture", "x", "y", "z", "w", "r", "g", "b", "a",
        "scale", "value", "index", "prototype", "prototypename", "pos",
        "rot", "present", "id", "modelname", "broken", "position",
        "rotation"}};
    return std::find(allowed.begin(), allowed.end(), lower) != allowed.end();
}

// This parser is intentionally a small, bounded XML structural canonicalizer,
// not a second native XML implementation. It accepts the SaveToXML subset,
// removes identity fields, rejects DTD/entity constructs, sorts attributes,
// and emits a deterministic representation for the native parser.
class XmlCanonicalizer final {
public:
    XmlCanonicalizer(const std::string_view input, std::string& output)
        : m_input(input), m_output(output)
    {
    }

    [[nodiscard]] bool parse()
    {
        skip_space();
        if (starts_with("\xef\xbb\xbf"))
            m_position += 3;
        skip_space();
        if (starts_with("<?")) {
            if (!skip_until("?>"))
                return false;
            skip_space();
        }
        if (!parse_element(true, false))
            return false;
        skip_space();
        while (starts_with("<!--")) {
            if (!skip_until("-->"))
                return false;
            skip_space();
        }
        return m_position == m_input.size() && !m_output.empty() &&
               m_output.size() <= kraken::net::kNativeObjectArchiveMaxXmlBytes;
    }

private:
    struct Attribute {
        std::string name;
        std::string value;
    };
    struct DepthGuard {
        explicit DepthGuard(std::size_t& depth) : value(depth) { ++value; }
        ~DepthGuard() { --value; }
        std::size_t& value;
    };

    [[nodiscard]] bool starts_with(const std::string_view prefix) const noexcept
    {
        return prefix.size() <= m_input.size() - m_position &&
               m_input.compare(m_position, prefix.size(), prefix) == 0;
    }

    void skip_space() noexcept
    {
        while (m_position < m_input.size()) {
            const char character = m_input[m_position];
            if (character != ' ' && character != '\t' && character != '\r' &&
                character != '\n')
                break;
            ++m_position;
        }
    }

    [[nodiscard]] bool skip_until(const std::string_view terminator) noexcept
    {
        const std::size_t end = m_input.find(terminator, m_position);
        if (end == std::string_view::npos)
            return false;
        m_position = end + terminator.size();
        return true;
    }

    [[nodiscard]] static bool name_start(const char character) noexcept
    {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') || character == '_';
    }

    [[nodiscard]] static bool name_character(const char character) noexcept
    {
        return name_start(character) || (character >= '0' && character <= '9') ||
               character == '-' || character == '.' || character == ':';
    }

    [[nodiscard]] bool read_name(std::string& output)
    {
        if (m_position >= m_input.size() || !name_start(m_input[m_position]))
            return false;
        const std::size_t begin = m_position++;
        while (m_position < m_input.size() && name_character(m_input[m_position]))
            ++m_position;
        try {
            output.assign(m_input.substr(begin, m_position - begin));
        }
        catch (...) {
            return false;
        }
        return output.size() <= kraken::net::kNativeObjectArchiveMaxPathBytes;
    }

    [[nodiscard]] bool read_attribute_value(std::string& output)
    {
        if (m_position >= m_input.size() ||
            (m_input[m_position] != '\'' && m_input[m_position] != '"'))
            return false;
        const char quote = m_input[m_position++];
        const std::size_t begin = m_position;
        while (m_position < m_input.size() && m_input[m_position] != quote) {
            const unsigned char byte =
                static_cast<unsigned char>(m_input[m_position]);
            if (byte == 0 || byte < 0x20u && byte != '\t' && byte != '\r' &&
                byte != '\n' || m_input[m_position] == '<')
                return false;
            ++m_position;
        }
        if (m_position == m_input.size())
            return false;
        try {
            output.assign(m_input.substr(begin, m_position - begin));
        }
        catch (...) {
            return false;
        }
        ++m_position;
        return output.size() <= kraken::net::kNativeObjectArchiveMaxXmlBytes;
    }

    void append_escaped_attribute(const std::string_view value)
    {
        for (const char character : value) {
            if (character == '"')
                m_output += "&quot;";
            else
                m_output.push_back(character);
        }
    }

    [[nodiscard]] bool append_text(const std::string_view value)
    {
        std::size_t begin = 0;
        while (begin < value.size() && (value[begin] == ' ' ||
               value[begin] == '\t' || value[begin] == '\r' ||
               value[begin] == '\n'))
            ++begin;
        std::size_t end = value.size();
        while (end > begin && (value[end - 1] == ' ' || value[end - 1] ==
               '\t' || value[end - 1] == '\r' || value[end - 1] == '\n'))
            --end;
        if (begin == end)
            return true;
        bool pending_space = false;
        for (std::size_t index = begin; index != end; ++index) {
            const unsigned char byte = static_cast<unsigned char>(value[index]);
            if (byte == 0 || byte < 0x20u && byte != '\t' && byte != '\r' &&
                byte != '\n')
                return false;
            if (value[index] == ' ' || value[index] == '\t' ||
                value[index] == '\r' || value[index] == '\n') {
                pending_space = true;
                continue;
            }
            if (pending_space && !m_output.empty() && m_output.back() != '>')
                m_output.push_back(' ');
            pending_space = false;
            m_output.push_back(value[index]);
        }
        return true;
    }

    [[nodiscard]] bool parse_element(const bool parent_emit,
                                     const bool runtime_context)
    {
        if (m_position >= m_input.size() || m_input[m_position++] != '<' ||
            m_position >= m_input.size() || m_input[m_position] == '/' ||
            m_input[m_position] == '!' || m_input[m_position] == '?')
            return false;
        if (m_depth >= kraken::net::kNativeObjectArchiveMaxManifestDepth)
            return false;
        const DepthGuard depth_guard(m_depth);
        std::string name;
        if (!read_name(name))
            return false;
        const bool denied = identity_name(name);
        const bool dropped = dropped_element_name(name);
        if (runtime_context && !denied && !dropped &&
            !is_visual_runtime_name(name) &&
            !is_runtime_structural_name(name))
            return false;
        const bool this_runtime_context = runtime_context ||
            is_runtime_container(name);
        std::vector<Attribute> attributes;
        skip_space();
        bool self_closing = false;
        while (m_position < m_input.size()) {
            if (starts_with("/>")) {
                m_position += 2;
                self_closing = true;
                break;
            }
            if (m_input[m_position] == '>') {
                ++m_position;
                break;
            }
            Attribute attribute;
            if (!read_name(attribute.name))
                return false;
            skip_space();
            if (m_position >= m_input.size() || m_input[m_position++] != '=')
                return false;
            skip_space();
            if (!read_attribute_value(attribute.value))
                return false;
            if (!identity_name(attribute.name) && !denied && !dropped) {
                if (this_runtime_context &&
                    dropped_runtime_attribute(attribute.name)) {
                    skip_space();
                    continue;
                }
                if (this_runtime_context &&
                    !is_visual_runtime_attribute(attribute.name))
                    return false;
                attributes.push_back(std::move(attribute));
            }
            skip_space();
        }
        if (m_position > m_input.size())
            return false;
        std::sort(attributes.begin(), attributes.end(),
                  [](const Attribute& left, const Attribute& right) {
                      return left.name < right.name;
                  });
        const bool emit = parent_emit && !denied && !dropped;
        if (emit) {
            m_output.push_back('<');
            m_output += name;
            for (const Attribute& attribute : attributes) {
                m_output.push_back(' ');
                m_output += attribute.name;
                m_output += "=\"";
                append_escaped_attribute(attribute.value);
                m_output.push_back('"');
            }
            m_output += self_closing ? "/>" : ">";
        }
        if (self_closing)
            return true;

        while (m_position < m_input.size()) {
            if (starts_with("</")) {
                m_position += 2;
                std::string closing_name;
                if (!read_name(closing_name) || closing_name != name)
                    return false;
                skip_space();
                if (m_position >= m_input.size() || m_input[m_position++] != '>')
                    return false;
                if (emit) {
                    m_output += "</";
                    m_output += name;
                    m_output.push_back('>');
                }
                return true;
            }
            if (starts_with("<!--")) {
                m_position += 4;
                if (!skip_until("-->"))
                    return false;
                continue;
            }
            if (starts_with("<![CDATA[")) {
                m_position += 9;
                const std::size_t end = m_input.find("]]>", m_position);
                if (end == std::string_view::npos)
                    return false;
                if (emit && !append_text(m_input.substr(m_position, end - m_position)))
                    return false;
                m_position = end + 3;
                continue;
            }
            if (m_input[m_position] == '<') {
                if (!parse_element(emit,
                                   denied || dropped ? false :
                                       this_runtime_context))
                    return false;
                continue;
            }
            const std::size_t begin = m_position;
            while (m_position < m_input.size() && m_input[m_position] != '<')
                ++m_position;
            if (emit && !append_text(m_input.substr(begin, m_position - begin)))
                return false;
        }
        return false;
    }

    std::string_view m_input;
    std::string& m_output;
    std::size_t m_position = 0;
    std::size_t m_depth = 0;
};

[[nodiscard]] bool archive_entry_less(
    const kraken::net::NativeObjectArchiveManifestEntry& left,
    const kraken::net::NativeObjectArchiveManifestEntry& right) noexcept
{
    return left.archive_path < right.archive_path;
}

[[nodiscard]] bool visual_entry_less(
    const kraken::net::NativeObjectArchiveVisualRuntime& left,
    const kraken::net::NativeObjectArchiveVisualRuntime& right) noexcept
{
    if (left.archive_path != right.archive_path)
        return left.archive_path < right.archive_path;
    return static_cast<std::uint8_t>(left.kind) <
           static_cast<std::uint8_t>(right.kind);
}

[[nodiscard]] bool valid_chunk(const NativeObjectArchiveChunk& value) noexcept
{
    if (value.archive_id == 0 || value.archive_revision == 0 ||
        value.digest == 0 || value.total_size == 0 ||
        value.total_size > kraken::net::kNativeObjectArchiveMaxBytes ||
        value.chunk_count == 0 || value.chunk_count >
            kraken::net::kNativeObjectArchiveMaxChunks ||
        value.chunk_index >= value.chunk_count || value.payload.empty() ||
        value.payload.size() > kraken::net::kNativeObjectArchiveChunkPayloadBytes)
        return false;
    const std::size_t expected_count =
        (value.total_size + kraken::net::kNativeObjectArchiveChunkPayloadBytes - 1) /
        kraken::net::kNativeObjectArchiveChunkPayloadBytes;
    if (value.chunk_count != expected_count)
        return false;
    const std::size_t offset = static_cast<std::size_t>(value.chunk_index) *
        kraken::net::kNativeObjectArchiveChunkPayloadBytes;
    const std::size_t expected_size = (std::min)(
        kraken::net::kNativeObjectArchiveChunkPayloadBytes,
        static_cast<std::size_t>(value.total_size) - offset);
    return value.payload.size() == expected_size;
}

// Constructing RawFile through the recovered native constructor is retained
// only for the verified XML load seam. The v2 capture path never guesses or
// reads native object layout.
class NativeRawFile final {
public:
    NativeRawFile(const char* path, const hta::m3d::fs::IStream::OpenFlags flags,
                  const bool mapping)
    {
        using Constructor = void(__thiscall*)(hta::m3d::fs::RawFile*,
                                               const char*,
                                               hta::m3d::fs::IStream::OpenFlags,
                                               bool);
        reinterpret_cast<Constructor>(0x008B06D0)(raw_file(), path, flags,
                                                   mapping);
        m_constructed = true;
    }

    ~NativeRawFile() noexcept
    {
        if (m_constructed) {
            using Destructor = void(__thiscall*)(hta::m3d::fs::RawFile*);
            reinterpret_cast<Destructor>(0x008B0750)(raw_file());
        }
    }

    NativeRawFile(const NativeRawFile&) = delete;
    NativeRawFile& operator=(const NativeRawFile&) = delete;

    [[nodiscard]] hta::m3d::fs::RawFile* operator->() noexcept { return raw_file(); }
    [[nodiscard]] hta::m3d::fs::RawFile& stream() noexcept { return *raw_file(); }

private:
    [[nodiscard]] hta::m3d::fs::RawFile* raw_file() noexcept
    {
        return reinterpret_cast<hta::m3d::fs::RawFile*>(m_storage);
    }
    alignas(hta::m3d::fs::RawFile) std::byte m_storage[
        sizeof(hta::m3d::fs::RawFile)]{};
    bool m_constructed = false;
};

[[nodiscard]] bool contains_parent_component(
    const std::filesystem::path& path) noexcept
{
    for (const auto& component : path)
        if (component == std::filesystem::path(".."))
            return true;
    return false;
}

[[nodiscard]] std::filesystem::path without_trailing_separator(
    const std::filesystem::path& path)
{
    const std::filesystem::path normalized = path.lexically_normal();
    std::string value = normalized.generic_string();
    std::string root_prefix = normalized.root_name().generic_string();
    if (normalized.has_root_directory())
        root_prefix.push_back('/');
    while (value.size() > root_prefix.size() && !value.empty() &&
           value.back() == '/')
        value.pop_back();
    return std::filesystem::path(value);
}

[[nodiscard]] bool path_is_below(const std::filesystem::path& candidate,
                                 const std::filesystem::path& root)
{
    const std::filesystem::path relative =
        without_trailing_separator(candidate).lexically_relative(
            without_trailing_separator(root));
    if (relative.empty() || relative.is_absolute())
        return false;
    for (const auto& component : relative)
        if (component == std::filesystem::path(".."))
            return false;
    return true;
}

[[nodiscard]] NativeObjectArchiveErrorCode write_bytes_to_temp(
    const std::string& native_path, const ByteView bytes)
{
    NativeRawFile raw_file(native_path.c_str(),
                           hta::m3d::fs::IStream::OPEN_WRITE, false);
    if (!raw_file->IsOpen())
        return NativeObjectArchiveErrorCode::RawFileOpenFailed;
    const std::uint32_t size = static_cast<std::uint32_t>(bytes.size());
    if (raw_file->WriteBytes(bytes.data(), size) != size ||
        raw_file->Flush() < 0 || raw_file->Close() < 0)
        return NativeObjectArchiveErrorCode::RawFileWriteFailed;
    return NativeObjectArchiveErrorCode::None;
}

[[nodiscard]] NativeObjectArchiveErrorCode read_bytes_from_temp(
    const std::string& native_path, std::vector<Byte>& output)
{
    output.clear();
    NativeRawFile raw_file(native_path.c_str(),
                           hta::m3d::fs::IStream::OPEN_READ, false);
    if (!raw_file->IsOpen())
        return NativeObjectArchiveErrorCode::RawFileOpenFailed;
    const std::uint32_t size = raw_file->GetSize();
    if (size == 0 || size > kraken::net::kNativeObjectArchiveMaxXmlBytes)
        return NativeObjectArchiveErrorCode::InputTooLarge;
    try {
        output.resize(size);
    }
    catch (const std::bad_alloc&) {
        return NativeObjectArchiveErrorCode::AllocationFailed;
    }
    if (raw_file->ReadBytes(output.data(), size) != size) {
        output.clear();
        return NativeObjectArchiveErrorCode::RawFileReadFailed;
    }
    return NativeObjectArchiveErrorCode::None;
}

[[nodiscard]] std::string root_name_from_xml(const std::vector<Byte>& xml)
{
    if (xml.empty() || static_cast<char>(xml[0]) != '<')
        return {};
    std::size_t begin = 1;
    if (begin < xml.size() && static_cast<char>(xml[begin]) == '?')
        return {};
    std::size_t end = begin;
    while (end < xml.size() && static_cast<char>(xml[end]) != '>' &&
           static_cast<char>(xml[end]) != '/' && static_cast<char>(xml[end]) !=
               ' ' && static_cast<char>(xml[end]) != '\t')
        ++end;
    if (end == begin)
        return {};
    return std::string(reinterpret_cast<const char*>(xml.data()) + begin,
                       end - begin);
}

struct XmlGraphNode {
    std::string path;
    std::string name;
    std::string resource;
    std::string prototype;
    kraken::net::NativeObjectArchiveResourceKind kind =
        kraken::net::NativeObjectArchiveResourceKind::Prototype;
    bool visual_runtime = false;
    kraken::net::NativeObjectArchiveVisualRuntimeKind visual_kind =
        kraken::net::NativeObjectArchiveVisualRuntimeKind::Visibility;
    std::array<float, 4> visual_components{};
    std::uint8_t visual_component_count = 0;
    bool visual_enabled = true;
};

struct XmlGraphFrame {
    std::string path;
    std::string name;
    std::vector<std::pair<std::string, std::size_t>> child_counts;
};

[[nodiscard]] std::string lower_ascii(std::string value)
{
    for (char& character : value)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    return value;
}

[[nodiscard]] bool parse_visual_bool(const std::string_view value,
                                     bool& output)
{
    const std::string lower = lower_ascii(std::string(value));
    if (lower == "1" || lower == "true" || lower == "yes" ||
        lower == "on") {
        output = true;
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" ||
        lower == "off") {
        output = false;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_visual_float(const std::string_view value,
                                      float& output) noexcept
{
    std::string copy;
    try {
        copy.assign(value);
    }
    catch (...) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(copy.c_str(), &end);
    if (end == copy.c_str() || end == nullptr || *end != '\0' ||
        !std::isfinite(parsed) || std::fabs(parsed) > kMaxVisualComponent)
        return false;
    output = parsed;
    return true;
}

[[nodiscard]] bool collect_xml_graph(const std::vector<Byte>& xml,
                                     std::vector<XmlGraphNode>& output)
{
    output.clear();
    std::vector<XmlGraphFrame> stack;
    std::size_t position = 0;
    std::size_t root_count = 0;
    const std::string_view input(reinterpret_cast<const char*>(xml.data()),
                                 xml.size());
    const auto skip_space = [&input](std::size_t& cursor) {
        while (cursor < input.size() &&
               (input[cursor] == ' ' || input[cursor] == '\t' ||
                input[cursor] == '\r' || input[cursor] == '\n'))
            ++cursor;
    };
    const auto read_name = [&input](std::size_t& cursor, std::string& name) {
        const std::size_t begin = cursor;
        if (cursor >= input.size() ||
            !((input[cursor] >= 'A' && input[cursor] <= 'Z') ||
              (input[cursor] >= 'a' && input[cursor] <= 'z') ||
              input[cursor] == '_'))
            return false;
        ++cursor;
        while (cursor < input.size() &&
               ((input[cursor] >= 'A' && input[cursor] <= 'Z') ||
                (input[cursor] >= 'a' && input[cursor] <= 'z') ||
                (input[cursor] >= '0' && input[cursor] <= '9') ||
                input[cursor] == '_' || input[cursor] == '-' ||
                input[cursor] == '.' || input[cursor] == ':'))
            ++cursor;
        name.assign(input.substr(begin, cursor - begin));
        return !name.empty();
    };
    while (position < input.size()) {
        if (input[position] != '<') {
            ++position;
            continue;
        }
        if (input.compare(position, 4, "<!--") == 0) {
            const std::size_t end = input.find("-->", position + 4);
            if (end == std::string_view::npos)
                return false;
            position = end + 3;
            continue;
        }
        if (input.compare(position, 2, "<?") == 0) {
            const std::size_t end = input.find("?>", position + 2);
            if (end == std::string_view::npos)
                return false;
            position = end + 2;
            continue;
        }
        ++position;
        if (position < input.size() && input[position] == '/') {
            ++position;
            std::string closing;
            if (!read_name(position, closing))
                return false;
            skip_space(position);
            if (position >= input.size() || input[position++] != '>' ||
                stack.empty() || stack.back().name != closing)
                return false;
            stack.pop_back();
            continue;
        }
        std::string name;
        if (!read_name(position, name))
            return false;
        XmlGraphNode node{};
        node.name = name;
        if (stack.empty() && ++root_count != 1)
            return false;
        node.path = stack.empty() ? name : stack.back().path + "/" + name;
        if (!stack.empty()) {
            auto& counts = stack.back().child_counts;
            auto found = std::find_if(counts.begin(), counts.end(),
                [&name](const auto& value) { return value.first == name; });
            if (found == counts.end())
                counts.emplace_back(name, 1);
            else {
                ++found->second;
                node.path += "[" + std::to_string(found->second) + "]";
            }
        }
        bool self_closing = false;
        skip_space(position);
        while (position < input.size()) {
            if (input.compare(position, 2, "/>" ) == 0) {
                position += 2;
                self_closing = true;
                break;
            }
            if (input[position] == '>') {
                ++position;
                break;
            }
            std::string attr_name;
            if (!read_name(position, attr_name))
                return false;
            skip_space(position);
            if (position >= input.size() || input[position++] != '=')
                return false;
            skip_space(position);
            if (position >= input.size() ||
                (input[position] != '\'' && input[position] != '"'))
                return false;
            const char quote = input[position++];
            const std::size_t begin = position;
            while (position < input.size() && input[position] != quote)
                ++position;
            if (position >= input.size())
                return false;
            const std::string value(input.substr(begin, position - begin));
            ++position;
            const std::string lower = lower_ascii(attr_name);
            if (lower == "prototype" || lower == "prototypename")
                node.prototype = value;
            else if (lower == "resource" || lower == "resourcename")
                node.resource = value;
            else if (lower == "mesh") {
                node.resource = value;
                node.kind = kraken::net::NativeObjectArchiveResourceKind::Mesh;
            }
            else if (lower == "material") {
                node.resource = value;
                node.kind = kraken::net::NativeObjectArchiveResourceKind::Material;
            }
            else if (lower == "texture") {
                node.resource = value;
                node.kind = kraken::net::NativeObjectArchiveResourceKind::Texture;
            }
            else if (lower == "enabled" || lower == "visible") {
                if (!parse_visual_bool(value, node.visual_enabled))
                    return false;
            }
            else if (lower == "x" || lower == "y" || lower == "z" ||
                     lower == "w") {
                const std::size_t component = lower == "x" ? 0 :
                    lower == "y" ? 1 : lower == "z" ? 2 : 3;
                if (!parse_visual_float(value,
                                        node.visual_components[component]))
                    return false;
                node.visual_component_count = static_cast<std::uint8_t>(
                    (std::max)(static_cast<std::size_t>(
                                   node.visual_component_count), component + 1));
            }
            skip_space(position);
        }
        bool under_runtime = false;
        for (const XmlGraphFrame& frame : stack)
            under_runtime = under_runtime || is_runtime_container(frame.name);
        if (under_runtime) {
            const std::string lower = lower_ascii(name);
            if (lower == "visibility" || lower == "visible")
                node.visual_runtime = true;
            else if (lower == "mesh" || lower == "visualpart") {
                node.visual_runtime = true;
                node.visual_kind =
                    kraken::net::NativeObjectArchiveVisualRuntimeKind::Mesh;
            }
            else if (lower == "material") {
                node.visual_runtime = true;
                node.visual_kind =
                    kraken::net::NativeObjectArchiveVisualRuntimeKind::Material;
            }
            else if (lower == "transform" || lower == "position" ||
                     lower == "rotation" || lower == "scale") {
                node.visual_runtime = true;
                node.visual_kind =
                    kraken::net::NativeObjectArchiveVisualRuntimeKind::Transform;
            }
            else if (lower == "brokenpart") {
                node.visual_runtime = true;
                node.visual_kind =
                    kraken::net::NativeObjectArchiveVisualRuntimeKind::BrokenPart;
            }
        }
        output.push_back(node);
        if (!self_closing)
            stack.push_back(XmlGraphFrame{node.path, name, {}});
    }
    return stack.empty() && root_count == 1 && !output.empty();
}

} // namespace

namespace kraken::net {

const char* native_object_archive_error_name(
    const NativeObjectArchiveErrorCode error) noexcept
{
    switch (error) {
    case NativeObjectArchiveErrorCode::None: return "none";
    case NativeObjectArchiveErrorCode::EmptyInput: return "empty_input";
    case NativeObjectArchiveErrorCode::InputTooLarge: return "input_too_large";
    case NativeObjectArchiveErrorCode::InvalidTempPath: return "invalid_temp_path";
    case NativeObjectArchiveErrorCode::PathTraversal: return "path_traversal";
    case NativeObjectArchiveErrorCode::TempFileCreateFailed: return "temp_file_create_failed";
    case NativeObjectArchiveErrorCode::RawFileOpenFailed: return "raw_file_open_failed";
    case NativeObjectArchiveErrorCode::RawFileWriteFailed: return "raw_file_write_failed";
    case NativeObjectArchiveErrorCode::RawFileReadFailed: return "raw_file_read_failed";
    case NativeObjectArchiveErrorCode::XmlFileCreateFailed: return "xml_file_create_failed";
    case NativeObjectArchiveErrorCode::XmlSerializeFailed: return "xml_serialize_failed";
    case NativeObjectArchiveErrorCode::XmlParseFailed: return "xml_parse_failed";
    case NativeObjectArchiveErrorCode::XmlEnvelopeMissing: return "xml_envelope_missing";
    case NativeObjectArchiveErrorCode::ContextMissing: return "context_missing";
    case NativeObjectArchiveErrorCode::ContextMismatch: return "context_mismatch";
    case NativeObjectArchiveErrorCode::ObjectNotSuspended: return "object_not_suspended";
    case NativeObjectArchiveErrorCode::ObjectLoadFailed: return "object_load_failed";
    case NativeObjectArchiveErrorCode::AllocationFailed: return "allocation_failed";
    case NativeObjectArchiveErrorCode::NativeCaptureUnavailable: return "native_capture_unavailable";
    case NativeObjectArchiveErrorCode::BadMagic: return "bad_magic";
    case NativeObjectArchiveErrorCode::BadVersion: return "bad_version";
    case NativeObjectArchiveErrorCode::BadFlags: return "bad_flags";
    case NativeObjectArchiveErrorCode::InvalidArchiveSize: return "invalid_archive_size";
    case NativeObjectArchiveErrorCode::InvalidXml: return "invalid_xml";
    case NativeObjectArchiveErrorCode::InvalidXmlIdentity: return "invalid_xml_identity";
    case NativeObjectArchiveErrorCode::InvalidMapNamespace: return "invalid_map_namespace";
    case NativeObjectArchiveErrorCode::InvalidResourceFingerprint: return "invalid_resource_fingerprint";
    case NativeObjectArchiveErrorCode::InvalidManifest: return "invalid_manifest";
    case NativeObjectArchiveErrorCode::ManifestTooLarge: return "manifest_too_large";
    case NativeObjectArchiveErrorCode::InvalidArchivePath: return "invalid_archive_path";
    case NativeObjectArchiveErrorCode::DuplicateArchivePath: return "duplicate_archive_path";
    case NativeObjectArchiveErrorCode::MissingArchiveParent: return "missing_archive_parent";
    case NativeObjectArchiveErrorCode::InvalidVisualRuntime: return "invalid_visual_runtime";
    case NativeObjectArchiveErrorCode::InvalidVisualPath: return "invalid_visual_path";
    case NativeObjectArchiveErrorCode::InvalidDigest: return "invalid_digest";
    case NativeObjectArchiveErrorCode::InvalidChunk: return "invalid_chunk";
    case NativeObjectArchiveErrorCode::ChunkTooLarge: return "chunk_too_large";
    case NativeObjectArchiveErrorCode::ConflictingChunk: return "conflicting_chunk";
    case NativeObjectArchiveErrorCode::TransferMemoryLimit: return "transfer_memory_limit";
    case NativeObjectArchiveErrorCode::TransferTimeout: return "transfer_timeout";
    case NativeObjectArchiveErrorCode::ArchiveUnavailable: return "archive_unavailable";
    case NativeObjectArchiveErrorCode::MaterializationRejected: return "materialization_rejected";
    }
    return "unknown";
}

NativeObjectArchiveErrorCode canonicalize_native_object_xml(
    const ByteView input, std::vector<Byte>& output) noexcept
{
    output.clear();
    if (input.empty())
        return NativeObjectArchiveErrorCode::EmptyInput;
    if (input.size() > kNativeObjectArchiveMaxXmlBytes)
        return NativeObjectArchiveErrorCode::InputTooLarge;
    try {
        std::string canonical;
        canonical.reserve(input.size());
        XmlCanonicalizer parser(
            std::string_view(reinterpret_cast<const char*>(input.data()), input.size()),
            canonical);
        if (!parser.parse())
            return NativeObjectArchiveErrorCode::InvalidXml;
        output.assign(reinterpret_cast<const Byte*>(canonical.data()),
                      reinterpret_cast<const Byte*>(canonical.data() + canonical.size()));
    }
    catch (const std::bad_alloc&) {
        output.clear();
        return NativeObjectArchiveErrorCode::AllocationFailed;
    }
    return output.empty() ? NativeObjectArchiveErrorCode::InvalidXml
                          : NativeObjectArchiveErrorCode::None;
}

NativeObjectArchiveErrorCode validate_native_object_archive_v2(
    const NativeObjectArchiveV2& value) noexcept
{
    try {
        if (!valid_stable_path(value.map_namespace) ||
            value.map_namespace.size() > kNativeObjectArchiveMaxNamespaceBytes)
            return NativeObjectArchiveErrorCode::InvalidMapNamespace;
        if (!valid_text_field(value.resource_fingerprint,
                              kNativeObjectArchiveMaxFingerprintBytes))
            return NativeObjectArchiveErrorCode::InvalidResourceFingerprint;
        if (value.manifest.size() > kNativeObjectArchiveMaxManifestEntries ||
            value.visual_runtime.size() > kNativeObjectArchiveMaxVisualRuntimeEntries)
            return NativeObjectArchiveErrorCode::ManifestTooLarge;

        std::vector<Byte> canonical_xml;
        const NativeObjectArchiveErrorCode xml_error =
            canonicalize_native_object_xml(value.canonical_xml, canonical_xml);
        if (xml_error != NativeObjectArchiveErrorCode::None ||
            canonical_xml != value.canonical_xml)
            return NativeObjectArchiveErrorCode::InvalidXml;

        std::vector<XmlGraphNode> xml_graph;
        if (!collect_xml_graph(value.canonical_xml, xml_graph) ||
            xml_graph.empty() || value.manifest.empty() ||
            value.manifest.size() != xml_graph.size())
            return NativeObjectArchiveErrorCode::InvalidManifest;
        const std::string xml_root = xml_graph.front().path;
        std::size_t manifest_roots = 0;
        std::string manifest_root;

        for (std::size_t index = 0; index != value.manifest.size(); ++index) {
            const auto& entry = value.manifest[index];
            if (!valid_stable_path(entry.archive_path))
                return NativeObjectArchiveErrorCode::InvalidArchivePath;
            if (!valid_stable_path(entry.parent_path, true))
                return NativeObjectArchiveErrorCode::InvalidArchivePath;
            if (!is_valid_native_object_archive_resource_kind(entry.kind))
                return NativeObjectArchiveErrorCode::InvalidManifest;
            if ((!entry.resource_name.empty() &&
                 !valid_stable_path(entry.resource_name)) ||
                (!entry.prototype_name.empty() &&
                 !valid_stable_path(entry.prototype_name)) ||
                (entry.archive_path == xml_root &&
                 entry.resource_name.empty() && entry.prototype_name.empty()))
                return NativeObjectArchiveErrorCode::InvalidManifest;
            if (entry.resource_fingerprint == 0)
                return NativeObjectArchiveErrorCode::InvalidResourceFingerprint;
            const std::size_t separator = entry.archive_path.rfind('/');
            const std::string expected_parent = separator == std::string::npos
                ? std::string{} : entry.archive_path.substr(0, separator);
            if (entry.parent_path != expected_parent)
                return NativeObjectArchiveErrorCode::InvalidManifest;
            for (std::size_t prior = 0; prior != index; ++prior) {
                if (value.manifest[prior].archive_path == entry.archive_path)
                    return NativeObjectArchiveErrorCode::DuplicateArchivePath;
            }
            if (!entry.parent_path.empty()) {
                bool found_parent = false;
                for (const auto& candidate : value.manifest)
                    if (candidate.archive_path == entry.parent_path) {
                        found_parent = true;
                        break;
                    }
                if (!found_parent)
                    return NativeObjectArchiveErrorCode::MissingArchiveParent;
            }
            else {
                ++manifest_roots;
                manifest_root = entry.archive_path;
            }
            const auto graph = std::find_if(xml_graph.begin(), xml_graph.end(),
                [&entry](const XmlGraphNode& node) {
                    return node.path == entry.archive_path;
                });
            if (graph == xml_graph.end())
                return NativeObjectArchiveErrorCode::InvalidManifest;
            if (graph->resource != entry.resource_name ||
                graph->prototype != entry.prototype_name)
                return NativeObjectArchiveErrorCode::InvalidManifest;
            if (entry.resource_fingerprint !=
                native_object_archive_resource_identity_digest(
                    value.resource_fingerprint, entry.archive_path,
                    entry.resource_name, entry.prototype_name))
                return NativeObjectArchiveErrorCode::InvalidResourceFingerprint;
        }
        if (manifest_roots != 1 || manifest_root != xml_root)
            return NativeObjectArchiveErrorCode::InvalidManifest;
        for (const auto& entry : value.manifest) {
            std::string path = entry.archive_path;
            std::size_t hops = 0;
            while (!path.empty()) {
                const auto current = std::find_if(value.manifest.begin(),
                    value.manifest.end(), [&path](const auto& candidate) {
                        return candidate.archive_path == path;
                    });
                if (current == value.manifest.end())
                    return NativeObjectArchiveErrorCode::MissingArchiveParent;
                if (++hops > value.manifest.size())
                    return NativeObjectArchiveErrorCode::InvalidManifest;
                path = current->parent_path;
            }
        }

        // Visual runtime state is part of the canonical graph contract, not
        // an optional side table. Every allowlisted visual node must have one
        // typed runtime record, and every record must point back to the same
        // node/kind/resource in the XML graph.
        for (const XmlGraphNode& node : xml_graph) {
            if (!node.visual_runtime)
                continue;
            std::size_t matches = 0;
            for (const auto& runtime : value.visual_runtime)
                if (runtime.archive_path == node.path &&
                    runtime.kind == node.visual_kind &&
                    runtime.resource_name == node.resource)
                    ++matches;
            if (matches != 1)
                return NativeObjectArchiveErrorCode::InvalidVisualRuntime;
        }

        for (std::size_t index = 0; index != value.visual_runtime.size(); ++index) {
            const auto& runtime = value.visual_runtime[index];
            if (!valid_stable_path(runtime.archive_path))
                return NativeObjectArchiveErrorCode::InvalidVisualPath;
            const auto graph = std::find_if(xml_graph.begin(), xml_graph.end(),
                [&runtime](const XmlGraphNode& node) {
                    return node.path == runtime.archive_path;
                });
            if (graph == xml_graph.end() || !graph->visual_runtime ||
                graph->visual_kind != runtime.kind ||
                graph->resource != runtime.resource_name ||
                graph->visual_enabled != runtime.enabled ||
                graph->visual_component_count != runtime.component_count)
                return NativeObjectArchiveErrorCode::InvalidVisualPath;
            for (std::size_t component = 0;
                 component != runtime.component_count; ++component)
                if (graph->visual_components[component] !=
                    runtime.components[component])
                    return NativeObjectArchiveErrorCode::InvalidVisualRuntime;
            if (!is_valid_native_object_archive_visual_runtime_kind(runtime.kind) ||
                runtime.component_count > runtime.components.size() ||
                runtime.resource_name.size() >
                    kNativeObjectArchiveMaxVisualStringBytes ||
                (!runtime.resource_name.empty() &&
                 !valid_stable_path(runtime.resource_name)))
                return NativeObjectArchiveErrorCode::InvalidVisualRuntime;
            if ((runtime.kind == NativeObjectArchiveVisualRuntimeKind::Mesh ||
                 runtime.kind == NativeObjectArchiveVisualRuntimeKind::Material) &&
                runtime.resource_name.empty())
                return NativeObjectArchiveErrorCode::InvalidVisualRuntime;
            if (runtime.kind == NativeObjectArchiveVisualRuntimeKind::Transform &&
                runtime.component_count != 3 && runtime.component_count != 4)
                return NativeObjectArchiveErrorCode::InvalidVisualRuntime;
            for (std::size_t component = 0;
                 component != runtime.component_count; ++component) {
                if (!std::isfinite(runtime.components[component]) ||
                    std::fabs(runtime.components[component]) > kMaxVisualComponent)
                    return NativeObjectArchiveErrorCode::InvalidVisualRuntime;
            }
            for (std::size_t prior = 0; prior != index; ++prior)
                if (value.visual_runtime[prior].archive_path == runtime.archive_path &&
                    value.visual_runtime[prior].kind == runtime.kind)
                    return NativeObjectArchiveErrorCode::InvalidVisualRuntime;
        }
    }
    catch (...) {
        return NativeObjectArchiveErrorCode::AllocationFailed;
    }
    return NativeObjectArchiveErrorCode::None;
}

NativeObjectArchiveDigest native_object_archive_digest(
    const ByteView encoded_archive) noexcept
{
    const bool has_archive_digest = encoded_archive.size() >= kArchiveHeaderSize &&
        get_u32(encoded_archive.data()) == kNativeObjectArchiveWireMagic &&
        get_u16(encoded_archive.data() + 4) == kNativeObjectArchiveVersion;
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t index = 0; index != encoded_archive.size(); ++index) {
        const std::uint8_t value = has_archive_digest &&
            index >= kArchiveDigestOffset &&
            index < kArchiveDigestOffset + sizeof(NativeObjectArchiveDigest)
                ? 0 : static_cast<std::uint8_t>(encoded_archive[index]);
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

std::uint64_t native_object_archive_resource_identity_digest(
    const std::string_view global_resource_fingerprint,
    const std::string_view archive_path, const std::string_view resource_name,
    const std::string_view prototype_name) noexcept
{
    // This is deliberately an identity binding, not a fabricated resource
    // content fingerprint. The global session fingerprint is verified by the
    // production resolver; this tuple only prevents a manifest entry from
    // being silently retargeted to another path/name within that session.
    constexpr std::string_view prefix = "native-archive-resource-id-v2|";
    std::uint64_t hash = 14695981039346656037ull;
    const auto update = [&hash](const std::string_view value) noexcept {
        for (const unsigned char byte : value) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    };
    update(prefix);
    update(archive_path);
    update("|");
    update(resource_name);
    update("|");
    update(prototype_name);
    update("|");
    update(global_resource_fingerprint);
    return hash == 0 ? 1 : hash;
}

NativeObjectArchiveErrorCode encode_native_object_archive_v2(
    const NativeObjectArchiveV2& value, std::vector<Byte>& output)
{
    output.clear();
    try {
        NativeObjectArchiveV2 canonical = value;
        const NativeObjectArchiveErrorCode xml_error = canonicalize_native_object_xml(
            value.canonical_xml, canonical.canonical_xml);
        if (xml_error != NativeObjectArchiveErrorCode::None)
            return xml_error;
        std::sort(canonical.manifest.begin(), canonical.manifest.end(), archive_entry_less);
        std::sort(canonical.visual_runtime.begin(), canonical.visual_runtime.end(),
                  visual_entry_less);
        if (const auto error = validate_native_object_archive_v2(canonical);
            error != NativeObjectArchiveErrorCode::None)
            return error;
        std::size_t size = kArchiveHeaderSize;
        if (!checked_add(size, canonical.map_namespace.size()) ||
            !checked_add(size, canonical.resource_fingerprint.size()))
            return NativeObjectArchiveErrorCode::InvalidArchiveSize;
        for (const auto& entry : canonical.manifest) {
            if (!checked_add(size, kManifestEntryHeaderSize) ||
                !checked_add(size, entry.archive_path.size()) ||
                !checked_add(size, entry.parent_path.size()) ||
                !checked_add(size, entry.resource_name.size()) ||
                !checked_add(size, entry.prototype_name.size()))
                return NativeObjectArchiveErrorCode::InvalidArchiveSize;
        }
        for (const auto& runtime : canonical.visual_runtime) {
            if (!checked_add(size, kVisualEntryHeaderSize) ||
                !checked_add(size, runtime.archive_path.size()) ||
                !checked_add(size, runtime.resource_name.size()))
                return NativeObjectArchiveErrorCode::InvalidArchiveSize;
        }
        if (!checked_add(size, canonical.canonical_xml.size()) ||
            size > kNativeObjectArchiveMaxBytes)
            return NativeObjectArchiveErrorCode::InputTooLarge;
        if (size > (std::numeric_limits<std::uint32_t>::max)() ||
            canonical.map_namespace.size() > (std::numeric_limits<std::uint16_t>::max)() ||
            canonical.resource_fingerprint.size() > (std::numeric_limits<std::uint16_t>::max)())
            return NativeObjectArchiveErrorCode::InvalidArchiveSize;
        output.assign(size, Byte{});
        Byte* const data = output.data();
        put_u32(data, kNativeObjectArchiveWireMagic);
        put_u16(data + 4, kNativeObjectArchiveVersion);
        put_u16(data + 6, 0);
        put_u32(data + 8, static_cast<std::uint32_t>(size));
        put_u32(data + 12, static_cast<std::uint32_t>(canonical.canonical_xml.size()));
        put_u16(data + 16, static_cast<std::uint16_t>(canonical.manifest.size()));
        put_u16(data + 18, static_cast<std::uint16_t>(canonical.visual_runtime.size()));
        put_u16(data + 20, static_cast<std::uint16_t>(canonical.map_namespace.size()));
        put_u16(data + 22, static_cast<std::uint16_t>(canonical.resource_fingerprint.size()));
        put_u64(data + kArchiveDigestOffset, 0);
        std::size_t offset = kArchiveHeaderSize;
        auto write_string = [&data, &offset](const std::string& text) {
            if (!text.empty())
                std::memcpy(data + offset, text.data(), text.size());
            offset += text.size();
        };
        write_string(canonical.map_namespace);
        write_string(canonical.resource_fingerprint);
        for (const auto& entry : canonical.manifest) {
            put_u16(data + offset, static_cast<std::uint16_t>(entry.archive_path.size()));
            put_u16(data + offset + 2, static_cast<std::uint16_t>(entry.parent_path.size()));
            put_u16(data + offset + 4, static_cast<std::uint16_t>(entry.resource_name.size()));
            put_u16(data + offset + 6, static_cast<std::uint16_t>(entry.prototype_name.size()));
            data[offset + 8] = static_cast<Byte>(entry.kind);
            data[offset + 9] = Byte{};
            put_u16(data + offset + 10, 0);
            put_u64(data + offset + 12, entry.resource_fingerprint);
            offset += kManifestEntryHeaderSize;
            write_string(entry.archive_path);
            write_string(entry.parent_path);
            write_string(entry.resource_name);
            write_string(entry.prototype_name);
        }
        for (const auto& runtime : canonical.visual_runtime) {
            put_u16(data + offset, static_cast<std::uint16_t>(runtime.archive_path.size()));
            put_u16(data + offset + 2, static_cast<std::uint16_t>(runtime.resource_name.size()));
            data[offset + 4] = static_cast<Byte>(runtime.kind);
            data[offset + 5] = static_cast<Byte>(runtime.enabled ? 1 : 0);
            data[offset + 6] = static_cast<Byte>(runtime.component_count);
            data[offset + 7] = Byte{};
            for (std::size_t component = 0; component != 4; ++component) {
                static_assert(sizeof(float) == sizeof(std::uint32_t));
                std::uint32_t bits = 0;
                std::memcpy(&bits, &runtime.components[component], sizeof(bits));
                put_u32(data + offset + 8 + component * 4, bits);
            }
            offset += kVisualEntryHeaderSize;
            write_string(runtime.archive_path);
            write_string(runtime.resource_name);
        }
        if (!canonical.canonical_xml.empty())
            std::memcpy(data + offset, canonical.canonical_xml.data(),
                        canonical.canonical_xml.size());
        const NativeObjectArchiveDigest digest = native_object_archive_digest(output);
        put_u64(data + kArchiveDigestOffset, digest);
    }
    catch (const std::bad_alloc&) {
        output.clear();
        return NativeObjectArchiveErrorCode::AllocationFailed;
    }
    return NativeObjectArchiveErrorCode::None;
}

NativeObjectArchiveErrorCode decode_native_object_archive_v2(
    const ByteView input, NativeObjectArchiveV2& output) noexcept
{
    if (input.empty())
        return NativeObjectArchiveErrorCode::EmptyInput;
    if (input.size() > kNativeObjectArchiveMaxBytes)
        return NativeObjectArchiveErrorCode::InputTooLarge;
    if (input.size() < kArchiveHeaderSize)
        return NativeObjectArchiveErrorCode::InvalidArchiveSize;
    const Byte* const data = input.data();
    if (get_u32(data) != kNativeObjectArchiveWireMagic)
        return NativeObjectArchiveErrorCode::BadMagic;
    if (get_u16(data + 4) != kNativeObjectArchiveVersion)
        return NativeObjectArchiveErrorCode::BadVersion;
    if (get_u16(data + 6) != 0)
        return NativeObjectArchiveErrorCode::BadFlags;
    if (get_u32(data + 8) != input.size() || get_u32(data + 8) >
            kNativeObjectArchiveMaxBytes)
        return NativeObjectArchiveErrorCode::InvalidArchiveSize;
    const std::uint32_t xml_size = get_u32(data + 12);
    const std::uint16_t manifest_count = get_u16(data + 16);
    const std::uint16_t visual_count = get_u16(data + 18);
    if (manifest_count > kNativeObjectArchiveMaxManifestEntries ||
        visual_count > kNativeObjectArchiveMaxVisualRuntimeEntries ||
        xml_size > kNativeObjectArchiveMaxXmlBytes)
        return NativeObjectArchiveErrorCode::ManifestTooLarge;
    NativeObjectArchiveV2 value{};
    value.digest = get_u64(data + kArchiveDigestOffset);
    if (value.digest == 0 || value.digest != native_object_archive_digest(input))
        return NativeObjectArchiveErrorCode::InvalidDigest;
    std::size_t offset = kArchiveHeaderSize;
    const auto read_string = [&input, &offset](const std::size_t size,
                                               std::string& value) noexcept {
        if (offset > input.size() || size > input.size() - offset)
            return false;
        try {
            value.assign(reinterpret_cast<const char*>(input.data() + offset), size);
        }
        catch (...) {
            return false;
        }
        offset += size;
        return true;
    };
    try {
        if (!read_string(get_u16(data + 20), value.map_namespace) ||
            !read_string(get_u16(data + 22), value.resource_fingerprint))
            return NativeObjectArchiveErrorCode::InvalidArchiveSize;
        value.manifest.reserve(manifest_count);
        for (std::size_t index = 0; index != manifest_count; ++index) {
            if (input.size() - offset < kManifestEntryHeaderSize)
                return NativeObjectArchiveErrorCode::InvalidArchiveSize;
            const Byte* const record = input.data() + offset;
            const std::uint16_t path_size = get_u16(record);
            const std::uint16_t parent_size = get_u16(record + 2);
            const std::uint16_t resource_size = get_u16(record + 4);
            const std::uint16_t prototype_size = get_u16(record + 6);
            if (record[9] != Byte{} || get_u16(record + 10) != 0)
                return NativeObjectArchiveErrorCode::BadFlags;
            NativeObjectArchiveManifestEntry entry{};
            entry.kind = static_cast<NativeObjectArchiveResourceKind>(record[8]);
            entry.resource_fingerprint = get_u64(record + 12);
            offset += kManifestEntryHeaderSize;
            if (!read_string(path_size, entry.archive_path) ||
                !read_string(parent_size, entry.parent_path) ||
                !read_string(resource_size, entry.resource_name) ||
                !read_string(prototype_size, entry.prototype_name))
                return NativeObjectArchiveErrorCode::InvalidArchiveSize;
            value.manifest.push_back(std::move(entry));
        }
        value.visual_runtime.reserve(visual_count);
        for (std::size_t index = 0; index != visual_count; ++index) {
            if (input.size() - offset < kVisualEntryHeaderSize)
                return NativeObjectArchiveErrorCode::InvalidArchiveSize;
            const Byte* const record = input.data() + offset;
            const std::uint16_t path_size = get_u16(record);
            const std::uint16_t resource_size = get_u16(record + 2);
            if (record[7] != Byte{} || (static_cast<std::uint8_t>(record[5]) & 0xfeu) != 0)
                return NativeObjectArchiveErrorCode::BadFlags;
            NativeObjectArchiveVisualRuntime runtime{};
            runtime.kind = static_cast<NativeObjectArchiveVisualRuntimeKind>(record[4]);
            runtime.enabled = static_cast<std::uint8_t>(record[5]) != 0;
            runtime.component_count = static_cast<std::uint8_t>(record[6]);
            for (std::size_t component = 0; component != 4; ++component) {
                const std::uint32_t bits = get_u32(record + 8 + component * 4);
                std::memcpy(&runtime.components[component], &bits, sizeof(bits));
            }
            offset += kVisualEntryHeaderSize;
            if (!read_string(path_size, runtime.archive_path) ||
                !read_string(resource_size, runtime.resource_name))
                return NativeObjectArchiveErrorCode::InvalidArchiveSize;
            value.visual_runtime.push_back(std::move(runtime));
        }
        if (input.size() - offset != xml_size)
            return NativeObjectArchiveErrorCode::InvalidArchiveSize;
        value.canonical_xml.assign(input.begin() + static_cast<std::ptrdiff_t>(offset),
                                   input.end());
    }
    catch (const std::bad_alloc&) {
        return NativeObjectArchiveErrorCode::AllocationFailed;
    }
    if (const auto error = validate_native_object_archive_v2(value);
        error != NativeObjectArchiveErrorCode::None)
        return error;
    output = std::move(value);
    return NativeObjectArchiveErrorCode::None;
}

NativeObjectArchiveTransferResult make_native_object_archive_chunks(
    const ByteView archive, const std::uint64_t archive_id,
    const std::uint32_t archive_revision, NativeObjectArchiveDigest digest,
    std::vector<NativeObjectArchiveChunk>& output)
{
    output.clear();
    if (archive_id == 0 || archive_revision == 0 ||
        validate_native_object_archive(archive) != NativeObjectArchiveErrorCode::None)
        return NativeObjectArchiveTransferResult::InvalidChunk;
    NativeObjectArchiveV2 decoded{};
    if (decode_native_object_archive_v2(archive, decoded) !=
        NativeObjectArchiveErrorCode::None)
        return NativeObjectArchiveTransferResult::ArchiveRejected;
    const NativeObjectArchiveDigest actual_digest = native_object_archive_digest(archive);
    if (digest == 0)
        digest = actual_digest;
    if (digest != actual_digest)
        return NativeObjectArchiveTransferResult::DigestMismatch;
    const std::size_t count = (archive.size() + kNativeObjectArchiveChunkPayloadBytes - 1) /
                              kNativeObjectArchiveChunkPayloadBytes;
    try {
        output.reserve(count);
        for (std::size_t index = 0; index != count; ++index) {
            NativeObjectArchiveChunk chunk{};
            chunk.archive_id = archive_id;
            chunk.archive_revision = archive_revision;
            chunk.digest = digest;
            chunk.total_size = static_cast<std::uint32_t>(archive.size());
            chunk.chunk_index = static_cast<std::uint16_t>(index);
            chunk.chunk_count = static_cast<std::uint16_t>(count);
            const std::size_t begin = index * kNativeObjectArchiveChunkPayloadBytes;
            const std::size_t end = (std::min)(begin + kNativeObjectArchiveChunkPayloadBytes,
                                               archive.size());
            chunk.payload.assign(archive.begin() + static_cast<std::ptrdiff_t>(begin),
                                 archive.begin() + static_cast<std::ptrdiff_t>(end));
            output.push_back(std::move(chunk));
        }
    }
    catch (const std::bad_alloc&) {
        output.clear();
        return NativeObjectArchiveTransferResult::TransferMemoryLimit;
    }
    return NativeObjectArchiveTransferResult::Accepted;
}

NativeObjectArchiveErrorCode encode_native_object_archive_chunk(
    const NativeObjectArchiveChunk& value, std::vector<Byte>& output)
{
    output.clear();
    if (!valid_chunk(value))
        return NativeObjectArchiveErrorCode::InvalidChunk;
    try {
        output.assign(kChunkHeaderSize + value.payload.size(), Byte{});
    }
    catch (const std::bad_alloc&) {
        return NativeObjectArchiveErrorCode::AllocationFailed;
    }
    Byte* const data = output.data();
    put_u32(data, kChunkWireMagic);
    put_u16(data + 4, kNativeObjectArchiveVersion);
    put_u16(data + 6, 0);
    put_u64(data + 8, value.archive_id);
    put_u32(data + 16, value.archive_revision);
    put_u64(data + 20, value.digest);
    put_u32(data + 28, value.total_size);
    put_u16(data + 32, value.chunk_index);
    put_u16(data + 34, value.chunk_count);
    put_u16(data + 36, static_cast<std::uint16_t>(value.payload.size()));
    put_u16(data + 38, 0);
    std::memcpy(data + kChunkHeaderSize, value.payload.data(), value.payload.size());
    return NativeObjectArchiveErrorCode::None;
}

NativeObjectArchiveErrorCode decode_native_object_archive_chunk(
    const ByteView input, NativeObjectArchiveChunk& output) noexcept
{
    if (input.size() < kChunkHeaderSize)
        return NativeObjectArchiveErrorCode::InvalidChunk;
    const Byte* const data = input.data();
    if (get_u32(data) != kChunkWireMagic)
        return NativeObjectArchiveErrorCode::BadMagic;
    if (get_u16(data + 4) != kNativeObjectArchiveVersion)
        return NativeObjectArchiveErrorCode::BadVersion;
    if (get_u16(data + 6) != 0 || get_u16(data + 38) != 0)
        return NativeObjectArchiveErrorCode::BadFlags;
    const std::uint16_t payload_size = get_u16(data + 36);
    if (input.size() != kChunkHeaderSize + payload_size)
        return NativeObjectArchiveErrorCode::InvalidChunk;
    NativeObjectArchiveChunk value{};
    value.archive_id = get_u64(data + 8);
    value.archive_revision = get_u32(data + 16);
    value.digest = get_u64(data + 20);
    value.total_size = get_u32(data + 28);
    value.chunk_index = get_u16(data + 32);
    value.chunk_count = get_u16(data + 34);
    try {
        value.payload.assign(input.begin() + kChunkHeaderSize, input.end());
    }
    catch (const std::bad_alloc&) {
        return NativeObjectArchiveErrorCode::AllocationFailed;
    }
    if (!valid_chunk(value))
        return value.payload.size() > kNativeObjectArchiveChunkPayloadBytes
                   ? NativeObjectArchiveErrorCode::ChunkTooLarge
                   : NativeObjectArchiveErrorCode::InvalidChunk;
    output = std::move(value);
    return NativeObjectArchiveErrorCode::None;
}

NativeObjectArchiveReassembler::NativeObjectArchiveReassembler(
    const std::size_t max_cache_entries, const std::size_t max_cache_bytes)
    : m_max_cache_entries((std::min)(max_cache_entries,
                                      kNativeObjectArchiveMaxCacheEntries)),
      m_max_cache_bytes((std::min)(max_cache_bytes,
                                   kNativeObjectArchiveMaxCacheBytes))
{
}

std::size_t NativeObjectArchiveReassembler::expire(const std::uint64_t now_ms) noexcept
{
    std::size_t removed = 0;
    const auto expired = [now_ms](const std::uint64_t last) noexcept {
        return now_ms >= last && now_ms - last > kNativeObjectArchiveTransferTimeoutMs;
    };
    const auto active_end = std::remove_if(m_active.begin(), m_active.end(),
        [&expired, &removed](const Assembly& assembly) {
            if (!expired(assembly.last_activity_ms))
                return false;
            ++removed;
            return true;
        });
    m_active.erase(active_end, m_active.end());
    const auto cache_end = std::remove_if(m_cache.begin(), m_cache.end(),
        [&expired, &removed, this](const CachedArchive& archive) {
            if (!expired(archive.last_activity_ms))
                return false;
            m_cached_bytes -= archive.bytes.size();
            ++removed;
            return true;
        });
    m_cache.erase(cache_end, m_cache.end());
    return removed;
}

NativeObjectArchiveTransferResult NativeObjectArchiveReassembler::accept(
    const NativeObjectArchiveChunk& chunk, const std::uint64_t now_ms,
    std::vector<Byte>& complete_archive)
{
    complete_archive.clear();
    (void)expire(now_ms);
    if (!valid_chunk(chunk))
        return NativeObjectArchiveTransferResult::InvalidChunk;

    for (CachedArchive& cached : m_cache) {
        if (cached.digest != chunk.digest || cached.archive_id != chunk.archive_id ||
            cached.archive_revision != chunk.archive_revision)
            continue;
        if (cached.total_size != chunk.total_size ||
            cached.chunk_count != chunk.chunk_count)
            return NativeObjectArchiveTransferResult::ConflictingChunk;
        const std::size_t begin = static_cast<std::size_t>(chunk.chunk_index) *
            kNativeObjectArchiveChunkPayloadBytes;
        if (begin + chunk.payload.size() > cached.bytes.size() ||
            std::memcmp(cached.bytes.data() + begin, chunk.payload.data(),
                        chunk.payload.size()) != 0)
            return NativeObjectArchiveTransferResult::ConflictingChunk;
        cached.last_activity_ms = now_ms;
        return NativeObjectArchiveTransferResult::Duplicate;
    }

    Assembly* assembly = nullptr;
    for (Assembly& candidate : m_active) {
        if (candidate.archive_id == chunk.archive_id &&
            candidate.archive_revision == chunk.archive_revision &&
            candidate.digest == chunk.digest) {
            if (candidate.total_size != chunk.total_size ||
                candidate.chunk_count != chunk.chunk_count)
                return NativeObjectArchiveTransferResult::ConflictingChunk;
            assembly = &candidate;
            break;
        }
    }
    if (assembly == nullptr) {
        std::size_t active_bytes = 0;
        for (const Assembly& candidate : m_active)
            active_bytes += candidate.total_size;
        if (m_active.size() >= kNativeObjectArchiveMaxActiveTransfers ||
            chunk.total_size > kNativeObjectArchiveMaxBytes ||
            active_bytes > kNativeObjectArchiveMaxBytes *
                kNativeObjectArchiveMaxActiveTransfers - chunk.total_size)
            return NativeObjectArchiveTransferResult::TransferMemoryLimit;
        try {
            Assembly candidate{};
            candidate.archive_id = chunk.archive_id;
            candidate.archive_revision = chunk.archive_revision;
            candidate.digest = chunk.digest;
            candidate.total_size = chunk.total_size;
            candidate.chunk_count = chunk.chunk_count;
            candidate.chunks.resize(chunk.chunk_count);
            candidate.present.assign(chunk.chunk_count, false);
            candidate.last_activity_ms = now_ms;
            m_active.push_back(std::move(candidate));
            assembly = &m_active.back();
        }
        catch (const std::bad_alloc&) {
            return NativeObjectArchiveTransferResult::TransferMemoryLimit;
        }
    }
    assembly->last_activity_ms = now_ms;
    const std::size_t index = chunk.chunk_index;
    if (assembly->present[index]) {
        if (assembly->chunks[index] == chunk.payload)
            return NativeObjectArchiveTransferResult::Duplicate;
        return NativeObjectArchiveTransferResult::ConflictingChunk;
    }
    try {
        assembly->chunks[index] = chunk.payload;
    }
    catch (const std::bad_alloc&) {
        return NativeObjectArchiveTransferResult::TransferMemoryLimit;
    }
    assembly->present[index] = true;
    ++assembly->received;
    if (assembly->received != assembly->chunk_count)
        return NativeObjectArchiveTransferResult::Accepted;

    std::vector<Byte> assembled;
    try {
        assembled.reserve(assembly->total_size);
        for (const auto& part : assembly->chunks)
            assembled.insert(assembled.end(), part.begin(), part.end());
        if (assembled.size() != assembly->total_size ||
            native_object_archive_digest(assembled) != assembly->digest) {
            m_active.erase(std::remove_if(m_active.begin(), m_active.end(),
                [assembly](const Assembly& candidate) {
                    return &candidate == assembly;
                }), m_active.end());
            return NativeObjectArchiveTransferResult::DigestMismatch;
        }
        NativeObjectArchiveV2 decoded{};
        if (decode_native_object_archive_v2(assembled, decoded) !=
            NativeObjectArchiveErrorCode::None) {
            m_active.erase(std::remove_if(m_active.begin(), m_active.end(),
                [assembly](const Assembly& candidate) {
                    return &candidate == assembly;
                }), m_active.end());
            return NativeObjectArchiveTransferResult::ArchiveRejected;
        }
    }
    catch (const std::bad_alloc&) {
        return NativeObjectArchiveTransferResult::TransferMemoryLimit;
    }
    complete_archive = assembled;
    const std::uint64_t archive_id = assembly->archive_id;
    const std::uint32_t archive_revision = assembly->archive_revision;
    const NativeObjectArchiveDigest digest = assembly->digest;
    m_active.erase(std::remove_if(m_active.begin(), m_active.end(),
        [archive_id, archive_revision, digest](const Assembly& candidate) {
            return candidate.archive_id == archive_id &&
                   candidate.archive_revision == archive_revision &&
                   candidate.digest == digest;
        }), m_active.end());
    if (m_max_cache_entries != 0 && assembled.size() <= m_max_cache_bytes) {
        try {
            while (m_cache.size() >= m_max_cache_entries ||
                   m_cached_bytes + assembled.size() > m_max_cache_bytes) {
                if (m_cache.empty())
                    break;
                const auto oldest = std::min_element(m_cache.begin(), m_cache.end(),
                    [](const CachedArchive& left, const CachedArchive& right) {
                        return left.last_activity_ms < right.last_activity_ms;
                    });
                m_cached_bytes -= oldest->bytes.size();
                m_cache.erase(oldest);
            }
            if (m_cache.size() < m_max_cache_entries &&
                m_cached_bytes + assembled.size() <= m_max_cache_bytes) {
                CachedArchive cached{};
                cached.archive_id = archive_id;
                cached.archive_revision = archive_revision;
                cached.digest = digest;
                cached.total_size = static_cast<std::uint32_t>(assembled.size());
                cached.chunk_count = static_cast<std::uint16_t>(
                    (assembled.size() + kNativeObjectArchiveChunkPayloadBytes - 1) /
                    kNativeObjectArchiveChunkPayloadBytes);
                cached.bytes = assembled;
                cached.last_activity_ms = now_ms;
                m_cached_bytes += cached.bytes.size();
                m_cache.push_back(std::move(cached));
            }
        }
        catch (const std::bad_alloc&) {
            // Completion is already verified; cache pressure must not make a
            // valid transfer fail. The cache simply remains unchanged.
        }
    }
    return NativeObjectArchiveTransferResult::Complete;
}

bool NativeObjectArchiveReassembler::lookup(
    const NativeObjectArchiveDigest digest, std::vector<Byte>& output) const
{
    output.clear();
    if (digest == 0)
        return false;
    for (const CachedArchive& cached : m_cache) {
        if (cached.digest == digest) {
            try {
                output = cached.bytes;
            }
            catch (...) {
                output.clear();
                return false;
            }
            return true;
        }
    }
    return false;
}

std::size_t NativeObjectArchiveReassembler::active_transfer_count() const noexcept
{
    return m_active.size();
}

std::size_t NativeObjectArchiveReassembler::cached_archive_count() const noexcept
{
    return m_cache.size();
}

std::size_t NativeObjectArchiveReassembler::cached_bytes() const noexcept
{
    return m_cached_bytes;
}

void NativeObjectArchiveReassembler::clear() noexcept
{
    m_active.clear();
    m_cache.clear();
    m_cached_bytes = 0;
}

NativeObjectArchiveErrorCode validate_native_object_archive_temp_path(
    const std::filesystem::path& candidate,
    const std::filesystem::path& temp_root)
{
    if (candidate.empty() || temp_root.empty())
        return NativeObjectArchiveErrorCode::InvalidTempPath;
    if (contains_parent_component(candidate) || contains_parent_component(temp_root))
        return NativeObjectArchiveErrorCode::PathTraversal;
    if (!path_is_below(candidate, temp_root))
        return NativeObjectArchiveErrorCode::PathTraversal;
    if (without_trailing_separator(candidate) ==
        without_trailing_separator(temp_root))
        return NativeObjectArchiveErrorCode::InvalidTempPath;
    return NativeObjectArchiveErrorCode::None;
}

NativeObjectArchiveTempFile::~NativeObjectArchiveTempFile() noexcept
{
    reset();
}

NativeObjectArchiveErrorCode NativeObjectArchiveTempFile::create()
{
    reset();
    std::error_code filesystem_error;
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path(filesystem_error);
    if (filesystem_error || temp_root.empty())
        return NativeObjectArchiveErrorCode::InvalidTempPath;
    std::string root = temp_root.string();
    if (root.empty() || root.size() >= MAX_PATH - 1)
        return NativeObjectArchiveErrorCode::InvalidTempPath;
    if (root.back() != '\\' && root.back() != '/')
        root.push_back('\\');
    char path_buffer[MAX_PATH]{};
    if (::GetTempFileNameA(root.c_str(), "KNA", 0, path_buffer) == 0)
        return NativeObjectArchiveErrorCode::TempFileCreateFailed;
    m_native_path = path_buffer;
    m_path = std::filesystem::path(m_native_path);
    const auto valid = validate_native_object_archive_temp_path(m_path, temp_root);
    if (valid != NativeObjectArchiveErrorCode::None) {
        reset();
        return valid;
    }
    return NativeObjectArchiveErrorCode::None;
}

void NativeObjectArchiveTempFile::reset() noexcept
{
    if (!m_native_path.empty() &&
        (::DeleteFileA(m_native_path.c_str()) != 0 ||
         ::GetLastError() == ERROR_FILE_NOT_FOUND)) {
        m_native_path.clear();
        m_path.clear();
    }
}

NativeObjectArchiveResult capture_native_object_archive(
    hta::ai::Obj& object, std::vector<Byte>& bytes)
{
    return capture_native_object_archive(object, "native/capture-v2",
                                         "rfp2-native-capture",
                                         bytes);
}

NativeObjectArchiveResult capture_native_object_archive(
    hta::ai::Obj& object, const std::string_view map_namespace,
    const std::string_view resource_fingerprint, std::vector<Byte>& bytes)
{
    bytes.clear();
    if (!valid_stable_path(map_namespace) ||
        !valid_text_field(resource_fingerprint,
                          kNativeObjectArchiveMaxFingerprintBytes))
        return result(NativeObjectArchiveErrorCode::ContextMismatch);

    NativeObjectArchiveTempFile temp_file;
    if (const auto error = temp_file.create();
        error != NativeObjectArchiveErrorCode::None)
        return result(error);
    try {
        hta::m3d::Kernel* const kernel = hta::m3d::Kernel::Instance();
        if (kernel == nullptr)
            return result(NativeObjectArchiveErrorCode::ContextMissing);
        ref_ptr<hta::m3d::cmn::XmlFile> xml_file = kernel->CreateXmlFile();
        if (!xml_file)
            return result(NativeObjectArchiveErrorCode::XmlFileCreateFailed);
        ref_ptr<hta::m3d::cmn::XmlNode> object_node =
            xml_file->CreateNode(hta::m3d::cmn::XML_NODE_ELEMENT, "Object");
        if (!object_node)
            return result(NativeObjectArchiveErrorCode::XmlSerializeFailed,
                          "native XML object envelope allocation failed");
        // XmlFile is itself the document container. Native save paths such as
        // DynamicScene::SaveSceneToFile attach their top-level element
        // directly; XML_NODE_DOCUMENT is not created through CreateNode.
        if (!xml_file->AddChild(object_node))
            return result(NativeObjectArchiveErrorCode::XmlSerializeFailed,
                          "native XML object envelope attach failed");
        // This is the recovered virtual SaveToXML seam. It includes native
        // Runtime automatically; canonicalization below applies the explicit
        // visual allowlist and identity denylist.  Obj::SaveToXML receives an
        // existing <Object> element in ObjContainer::SaveToXml; passing the
        // document node violates the native serializer contract.
        object.SaveToXML(xml_file, object_node);
        NativeRawFile raw_file(temp_file.native_path().c_str(),
                               hta::m3d::fs::IStream::OPEN_WRITE, false);
        if (!raw_file->IsOpen() || xml_file->Write(raw_file.stream()) < 0 ||
            raw_file->Flush() < 0 || raw_file->Close() < 0)
            return result(NativeObjectArchiveErrorCode::RawFileWriteFailed);
    }
    catch (const std::bad_alloc&) {
        return result(NativeObjectArchiveErrorCode::AllocationFailed);
    }
    catch (...) {
        return result(NativeObjectArchiveErrorCode::XmlSerializeFailed);
    }

    std::vector<Byte> native_xml;
    if (const auto error = read_bytes_from_temp(temp_file.native_path(), native_xml);
        error != NativeObjectArchiveErrorCode::None)
        return result(error);
    NativeObjectArchiveV2 archive{};
    archive.map_namespace.assign(map_namespace);
    archive.resource_fingerprint.assign(resource_fingerprint);
    if (const auto error = canonicalize_native_object_xml(native_xml,
                                                          archive.canonical_xml);
        error != NativeObjectArchiveErrorCode::None)
        return result(error);
    std::vector<XmlGraphNode> graph;
    try {
        if (!collect_xml_graph(archive.canonical_xml, graph))
            return result(NativeObjectArchiveErrorCode::InvalidManifest,
                          "native SaveToXML produced no single object graph");
    }
    catch (const std::bad_alloc&) {
        return result(NativeObjectArchiveErrorCode::AllocationFailed);
    }
    catch (...) {
        return result(NativeObjectArchiveErrorCode::InvalidManifest,
                      "native SaveToXML graph inspection failed");
    }
    try {
        archive.manifest.reserve(graph.size());
        for (const XmlGraphNode& node : graph) {
            // Structural descendants such as Runtime/Visibility are part of
            // the graph but do not declare an independent resource. The root
            // object must still expose a verified prototype/resource so the
            // suspended native object cannot be created from an inferred
            // class or local identity.
            if (node.resource.empty() && node.prototype.empty() &&
                node.path == graph.front().path)
                return result(NativeObjectArchiveErrorCode::NativeCaptureUnavailable,
                              "SaveToXML root has no verified resource/prototype name");
            const std::size_t separator = node.path.rfind('/');
            NativeObjectArchiveManifestEntry entry{};
            entry.archive_path = node.path;
            entry.parent_path = separator == std::string::npos
                ? std::string{} : node.path.substr(0, separator);
            entry.resource_name = node.resource;
            entry.prototype_name = node.prototype;
            entry.kind = node.kind;
            entry.resource_fingerprint =
                native_object_archive_resource_identity_digest(
                    resource_fingerprint, node.path, node.resource,
                    node.prototype);
            archive.manifest.push_back(std::move(entry));
            if (node.visual_runtime) {
                NativeObjectArchiveVisualRuntime visual{};
                visual.archive_path = node.path;
                visual.kind = node.visual_kind;
                visual.resource_name = node.resource;
                visual.components = node.visual_components;
                visual.component_count = node.visual_component_count;
                visual.enabled = node.visual_enabled;
                if (visual.kind == NativeObjectArchiveVisualRuntimeKind::Mesh ||
                    visual.kind == NativeObjectArchiveVisualRuntimeKind::Material) {
                    if (visual.resource_name.empty())
                        return result(NativeObjectArchiveErrorCode::NativeCaptureUnavailable,
                                      "visual resource is not declared by SaveToXML");
                }
                archive.visual_runtime.push_back(std::move(visual));
            }
        }
    }
    catch (const std::bad_alloc&) {
        return result(NativeObjectArchiveErrorCode::AllocationFailed);
    }
    std::vector<Byte> encoded;
    const auto error = encode_native_object_archive_v2(archive, encoded);
    if (error != NativeObjectArchiveErrorCode::None)
        return result(error);
    bytes = std::move(encoded);
    NativeObjectArchiveV2 verified{};
    if (decode_native_object_archive_v2(ByteView{bytes}, verified) !=
        NativeObjectArchiveErrorCode::None)
        return result(NativeObjectArchiveErrorCode::InvalidDigest);
    return result(NativeObjectArchiveErrorCode::None, {}, bytes.size(),
                  verified.digest);
}

NativeObjectArchiveResult restore_native_object_archive_v2(
    const NativeObjectArchiveV2& archive, hta::ai::Obj& suspended_object)
{
    if (const auto error = validate_native_object_archive_v2(archive);
        error != NativeObjectArchiveErrorCode::None)
        return result(error);
    // CreateNewObjectWithSuspendedPostLoad leaves m_bNeedPostLoad set until
    // the transaction seals the complete restored graph.  ODE ownership is
    // not a suspension marker: prototype construction may initialize that
    // flag before Obj::PostLoad, and presentation replicas retire it later.
    if (!suspended_object.m_bNeedPostLoad)
        return result(NativeObjectArchiveErrorCode::ObjectNotSuspended,
                      "native object already crossed the PostLoad barrier");
    NativeObjectArchiveTempFile temp_file;
    if (const auto error = temp_file.create();
        error != NativeObjectArchiveErrorCode::None)
        return result(error);
    if (const auto error = write_bytes_to_temp(temp_file.native_path(), archive.canonical_xml);
        error != NativeObjectArchiveErrorCode::None)
        return result(error);
    hta::CStr parse_error;
    ref_ptr<hta::m3d::cmn::XmlFile> xml_file =
        hta::m3d::cmn::ReadXmlFile(temp_file.native_path().c_str(), &parse_error);
    if (!xml_file)
        return result(NativeObjectArchiveErrorCode::XmlParseFailed,
                      parse_error.c_str() != nullptr ? parse_error.c_str()
                                                      : "native XML parse failed");
    const std::string root = root_name_from_xml(archive.canonical_xml);
    // Vehicle::SaveToXML writes the concrete object envelope used by the
    // native save fixture (Object), while the loader is dispatched through
    // the suspended Vehicle vtable.  Do not invent a Vehicle wrapper here.
    const char* expected = suspended_object.IsKindOf("Vehicle") ? "Object" : "Obj";
    if (root.empty() || root != expected)
        return result(NativeObjectArchiveErrorCode::XmlEnvelopeMissing);
    ref_ptr<hta::m3d::cmn::XmlNode> document =
        xml_file->CreateNode(hta::m3d::cmn::XML_NODE_EMPTY, nullptr);
    if (!document || !xml_file->GetFirstChild(document, expected))
        return result(NativeObjectArchiveErrorCode::XmlEnvelopeMissing);
    // LoadFromXML is the structure-only native seam. PostLoad, visual
    // creation, physics, gameplay, and death callbacks belong to the
    // transactional materializer and are never invoked here.
    suspended_object.LoadFromXML(xml_file, document);
    return result(NativeObjectArchiveErrorCode::None,
                  {}, archive.canonical_xml.size(), archive.digest);
}

NativeObjectArchiveResult restore_native_object_archive(
    const ByteView bytes, hta::ai::Obj& suspended_object)
{
    NativeObjectArchiveV2 archive{};
    const auto error = decode_native_object_archive_v2(bytes, archive);
    if (error != NativeObjectArchiveErrorCode::None)
        return result(error);
    return restore_native_object_archive_v2(archive, suspended_object);
}

} // namespace kraken::net
