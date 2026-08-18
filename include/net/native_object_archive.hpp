#ifndef KRAKEN_NET_NATIVE_OBJECT_ARCHIVE_HPP
#define KRAKEN_NET_NATIVE_OBJECT_ARCHIVE_HPP

#include "net/net_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hta::ai {
struct Obj;
}

namespace kraken::net {

// A wreck archive is a complete, verified value. It is never put directly
// in a world mutation packet; the transfer API below splits it into bounded
// reliable chunks. The limits are deliberately conservative because the
// native XML reader is not a streaming parser.
inline constexpr std::uint32_t kNativeObjectArchiveWireMagic =
    0x32414f4eu; // NOA2
inline constexpr std::uint16_t kNativeObjectArchiveVersion = 2;
inline constexpr std::size_t kNativeObjectArchiveMaxBytes = 256u * 1024u;
inline constexpr std::size_t kNativeObjectArchiveMaxXmlBytes = 192u * 1024u;
inline constexpr std::size_t kNativeObjectArchiveMaxPathBytes = 192;
inline constexpr std::size_t kNativeObjectArchiveMaxNamespaceBytes = 96;
inline constexpr std::size_t kNativeObjectArchiveMaxFingerprintBytes = 128;
inline constexpr std::size_t kNativeObjectArchiveMaxManifestEntries = 512;
inline constexpr std::size_t kNativeObjectArchiveMaxVisualRuntimeEntries = 128;
inline constexpr std::size_t kNativeObjectArchiveMaxVisualStringBytes = 128;
inline constexpr std::size_t kNativeObjectArchiveMaxManifestDepth = 32;

inline constexpr std::size_t kNativeObjectArchiveChunkPayloadBytes = 1024;
inline constexpr std::size_t kNativeObjectArchiveMaxChunks =
    (kNativeObjectArchiveMaxBytes + kNativeObjectArchiveChunkPayloadBytes - 1) /
    kNativeObjectArchiveChunkPayloadBytes;
inline constexpr std::size_t kNativeObjectArchiveMaxActiveTransfers = 8;
inline constexpr std::size_t kNativeObjectArchiveMaxCacheEntries = 64;
inline constexpr std::size_t kNativeObjectArchiveMaxCacheBytes =
    2u * 1024u * 1024u;
inline constexpr std::uint64_t kNativeObjectArchiveTransferTimeoutMs = 15'000;

using NativeObjectArchiveDigest = std::uint64_t;

enum class NativeObjectArchiveResourceKind : std::uint8_t {
    Prototype = 1,
    Mesh = 2,
    Material = 3,
    Texture = 4,
    Sound = 5,
    Effect = 6,
};

[[nodiscard]] constexpr bool is_valid_native_object_archive_resource_kind(
    const NativeObjectArchiveResourceKind value) noexcept
{
    switch (value) {
    case NativeObjectArchiveResourceKind::Prototype:
    case NativeObjectArchiveResourceKind::Mesh:
    case NativeObjectArchiveResourceKind::Material:
    case NativeObjectArchiveResourceKind::Texture:
    case NativeObjectArchiveResourceKind::Sound:
    case NativeObjectArchiveResourceKind::Effect:
        return true;
    }
    return false;
}

// These are the only runtime values allowed to cross the archive boundary.
// Gameplay state, physics state, object IDs, and arbitrary property bags are
// intentionally not representable here.
enum class NativeObjectArchiveVisualRuntimeKind : std::uint8_t {
    Visibility = 1,
    Mesh = 2,
    Material = 3,
    Transform = 4,
    BrokenPart = 5,
};

[[nodiscard]] constexpr bool is_valid_native_object_archive_visual_runtime_kind(
    const NativeObjectArchiveVisualRuntimeKind value) noexcept
{
    switch (value) {
    case NativeObjectArchiveVisualRuntimeKind::Visibility:
    case NativeObjectArchiveVisualRuntimeKind::Mesh:
    case NativeObjectArchiveVisualRuntimeKind::Material:
    case NativeObjectArchiveVisualRuntimeKind::Transform:
    case NativeObjectArchiveVisualRuntimeKind::BrokenPart:
        return true;
    }
    return false;
}

struct NativeObjectArchiveManifestEntry {
    // Stable, archive-local path. It is not a native pointer, ObjId, or
    // process-local name. parent_path is explicit so recursive validation
    // does not depend on an engine graph lookup.
    std::string archive_path{};
    std::string parent_path{};
    std::string resource_name{};
    // Native prototype and visual resource names are distinct namespaces. At
    // least one of resource_name/prototype_name must be present.
    std::string prototype_name{};
    NativeObjectArchiveResourceKind kind =
        NativeObjectArchiveResourceKind::Prototype;
    // Archive-local identity digest, not an asset-content hash. It is a
    // deterministic digest of this path/resource/prototype tuple bound to
    // NativeObjectArchiveV2::resource_fingerprint. Asset authenticity comes
    // from the verified session resource fingerprint and native prototype
    // resolution before LoadFromXML.
    std::uint64_t resource_fingerprint = 0;
};

struct NativeObjectArchiveVisualRuntime {
    std::string archive_path{};
    NativeObjectArchiveVisualRuntimeKind kind =
        NativeObjectArchiveVisualRuntimeKind::Visibility;
    std::string resource_name{};
    std::array<float, 4> components{};
    std::uint8_t component_count = 0;
    bool enabled = true;
};

struct NativeObjectArchiveV2 {
    std::string map_namespace{};
    std::string resource_fingerprint{};
    std::vector<NativeObjectArchiveManifestEntry> manifest{};
    std::vector<NativeObjectArchiveVisualRuntime> visual_runtime{};
    // Canonical structural XML from the native SaveToXML path. The
    // canonicalizer retains only the typed visual subset of Runtime, removes
    // explicit object/event/gameplay identity fields, and rejects unsafe XML
    // constructs before the native parser sees the data.
    std::vector<Byte> canonical_xml{};
    NativeObjectArchiveDigest digest = 0;
};

using NativeObjectArchive = NativeObjectArchiveV2;
using NativeObjectArchiveResource = NativeObjectArchiveManifestEntry;
using NativeObjectArchiveVisualState = NativeObjectArchiveVisualRuntime;

enum class NativeObjectArchiveErrorCode : std::uint8_t {
    None,
    EmptyInput,
    InputTooLarge,
    InvalidTempPath,
    PathTraversal,
    TempFileCreateFailed,
    RawFileOpenFailed,
    RawFileWriteFailed,
    RawFileReadFailed,
    XmlFileCreateFailed,
    XmlSerializeFailed,
    XmlParseFailed,
    XmlEnvelopeMissing,
    ContextMissing,
    ContextMismatch,
    ObjectNotSuspended,
    ObjectLoadFailed,
    AllocationFailed,
    NativeCaptureUnavailable,
    BadMagic,
    BadVersion,
    BadFlags,
    InvalidArchiveSize,
    InvalidXml,
    InvalidXmlIdentity,
    InvalidMapNamespace,
    InvalidResourceFingerprint,
    InvalidManifest,
    ManifestTooLarge,
    InvalidArchivePath,
    DuplicateArchivePath,
    MissingArchiveParent,
    InvalidVisualRuntime,
    InvalidVisualPath,
    InvalidDigest,
    InvalidChunk,
    ChunkTooLarge,
    ConflictingChunk,
    TransferMemoryLimit,
    TransferTimeout,
    ArchiveUnavailable,
    MaterializationRejected,
};

using NativeObjectArchiveError = NativeObjectArchiveErrorCode;

[[nodiscard]] constexpr bool native_object_archive_succeeded(
    const NativeObjectArchiveErrorCode error) noexcept
{
    return error == NativeObjectArchiveErrorCode::None;
}

struct NativeObjectArchiveResult {
    NativeObjectArchiveErrorCode error = NativeObjectArchiveErrorCode::None;
    std::size_t size = 0;
    std::string detail{};
    NativeObjectArchiveDigest digest = 0;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return native_object_archive_succeeded(error);
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return succeeded();
    }
};

[[nodiscard]] const char* native_object_archive_error_name(
    NativeObjectArchiveErrorCode) noexcept;

// This inexpensive check remains the transport preflight used by callers
// that do not yet know whether a payload is an archive. Full v2 validation is
// provided separately and is always performed by the v2 decoder/reassembler.
[[nodiscard]] constexpr NativeObjectArchiveErrorCode
validate_native_object_archive(ByteView bytes) noexcept
{
    if (bytes.empty())
        return NativeObjectArchiveErrorCode::EmptyInput;
    if (bytes.size() > kNativeObjectArchiveMaxBytes)
        return NativeObjectArchiveErrorCode::InputTooLarge;
    return NativeObjectArchiveErrorCode::None;
}

[[nodiscard]] NativeObjectArchiveErrorCode
validate_native_object_archive_v2(const NativeObjectArchiveV2&) noexcept;

[[nodiscard]] NativeObjectArchiveErrorCode canonicalize_native_object_xml(
    ByteView input, std::vector<Byte>& output) noexcept;

[[nodiscard]] NativeObjectArchiveDigest native_object_archive_digest(
    ByteView encoded_archive) noexcept;

// Returns the honest per-entry identity digest described above. This does
// not claim to hash resource bytes; callers must separately verify the
// negotiated global resource fingerprint and resolve prototypes locally.
[[nodiscard]] std::uint64_t native_object_archive_resource_identity_digest(
    std::string_view global_resource_fingerprint,
    std::string_view archive_path, std::string_view resource_name,
    std::string_view prototype_name) noexcept;

[[nodiscard]] NativeObjectArchiveErrorCode encode_native_object_archive_v2(
    const NativeObjectArchiveV2&, std::vector<Byte>& output);

[[nodiscard]] NativeObjectArchiveErrorCode decode_native_object_archive_v2(
    ByteView input, NativeObjectArchiveV2& output) noexcept;

[[nodiscard]] inline NativeObjectArchiveErrorCode
validate_native_object_archive_v2(const ByteView input) noexcept
{
    NativeObjectArchiveV2 value{};
    return decode_native_object_archive_v2(input, value);
}

// Short aliases keep the v2 protocol easy to discover without reintroducing
// the old unversioned wire contract.
[[nodiscard]] inline NativeObjectArchiveErrorCode encode_native_object_archive(
    const NativeObjectArchiveV2& value, std::vector<Byte>& output)
{
    return encode_native_object_archive_v2(value, output);
}

[[nodiscard]] inline NativeObjectArchiveErrorCode decode_native_object_archive(
    ByteView input, NativeObjectArchiveV2& output) noexcept
{
    return decode_native_object_archive_v2(input, output);
}

struct NativeObjectArchiveChunk {
    std::uint64_t archive_id = 0;
    std::uint32_t archive_revision = 0;
    NativeObjectArchiveDigest digest = 0;
    std::uint32_t total_size = 0;
    std::uint16_t chunk_index = 0;
    std::uint16_t chunk_count = 0;
    std::vector<Byte> payload{};
};

enum class NativeObjectArchiveTransferResult : std::uint8_t {
    Accepted,
    Complete,
    Duplicate,
    InvalidChunk,
    ConflictingChunk,
    DigestMismatch,
    ArchiveRejected,
    TransferMemoryLimit,
    TransferTimeout,
};

[[nodiscard]] NativeObjectArchiveTransferResult make_native_object_archive_chunks(
    ByteView archive, std::uint64_t archive_id, std::uint32_t archive_revision,
    NativeObjectArchiveDigest digest,
    std::vector<NativeObjectArchiveChunk>& output);

[[nodiscard]] NativeObjectArchiveErrorCode encode_native_object_archive_chunk(
    const NativeObjectArchiveChunk&, std::vector<Byte>& output);

[[nodiscard]] NativeObjectArchiveErrorCode decode_native_object_archive_chunk(
    ByteView input, NativeObjectArchiveChunk& output) noexcept;

[[nodiscard]] NativeObjectArchiveErrorCode
validate_native_object_archive_temp_path(
    const std::filesystem::path& candidate,
    const std::filesystem::path& temp_root);

// The reassembler owns all incomplete and cached data. It has one bounded
// assembly per archive identity and never exposes partial bytes to callers.
class NativeObjectArchiveReassembler final {
public:
    explicit NativeObjectArchiveReassembler(
        std::size_t max_cache_entries = kNativeObjectArchiveMaxCacheEntries,
        std::size_t max_cache_bytes = kNativeObjectArchiveMaxCacheBytes);

    [[nodiscard]] NativeObjectArchiveTransferResult accept(
        const NativeObjectArchiveChunk&, std::uint64_t now_ms,
        std::vector<Byte>& complete_archive);
    [[nodiscard]] std::size_t expire(std::uint64_t now_ms) noexcept;
    [[nodiscard]] bool lookup(NativeObjectArchiveDigest digest,
                              std::vector<Byte>& output) const;
    [[nodiscard]] std::size_t active_transfer_count() const noexcept;
    [[nodiscard]] std::size_t cached_archive_count() const noexcept;
    [[nodiscard]] std::size_t cached_bytes() const noexcept;
    void clear() noexcept;

private:
    struct Assembly {
        std::uint64_t archive_id = 0;
        std::uint32_t archive_revision = 0;
        NativeObjectArchiveDigest digest = 0;
        std::uint32_t total_size = 0;
        std::uint16_t chunk_count = 0;
        std::vector<std::vector<Byte>> chunks{};
        std::vector<bool> present{};
        std::size_t received = 0;
        std::uint64_t last_activity_ms = 0;
    };
    struct CachedArchive {
        std::uint64_t archive_id = 0;
        std::uint32_t archive_revision = 0;
        NativeObjectArchiveDigest digest = 0;
        std::uint32_t total_size = 0;
        std::uint16_t chunk_count = 0;
        std::vector<Byte> bytes{};
        std::uint64_t last_activity_ms = 0;
    };
    std::vector<Assembly> m_active;
    std::vector<CachedArchive> m_cache;
    std::size_t m_max_cache_entries = 0;
    std::size_t m_max_cache_bytes = 0;
    std::size_t m_cached_bytes = 0;
};

using NativeObjectArchiveTransfer = NativeObjectArchiveReassembler;

// GetTempFileNameA creates the file, then RawFile owns all subsequent I/O.
// The destructor removes the path even when native XML parsing or loading
// reports an error.
class NativeObjectArchiveTempFile final {
public:
    NativeObjectArchiveTempFile() = default;
    ~NativeObjectArchiveTempFile() noexcept;

    NativeObjectArchiveTempFile(const NativeObjectArchiveTempFile&) = delete;
    NativeObjectArchiveTempFile& operator=(
        const NativeObjectArchiveTempFile&) = delete;
    NativeObjectArchiveTempFile(NativeObjectArchiveTempFile&&) = delete;
    NativeObjectArchiveTempFile& operator=(NativeObjectArchiveTempFile&&) = delete;

    [[nodiscard]] NativeObjectArchiveErrorCode create();
    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept { return !m_native_path.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    [[nodiscard]] const std::string& native_path() const noexcept { return m_native_path; }

private:
    std::filesystem::path m_path{};
    std::string m_native_path{};
};

// Capture uses the exact virtual SaveToXML seam and the native temp-file XML
// path. It fails closed if the resulting XML does not expose a single,
// recursively validated graph with explicit resource/prototype names; no
// native offsets or pointer identity are inferred. A verified archive can
// still be loaded through the proven virtual LoadFromXML path below.
[[nodiscard]] NativeObjectArchiveResult capture_native_object_archive(
    hta::ai::Obj& object, std::vector<Byte>& bytes);

// Production capture supplies the already negotiated map/resource identity.
// The XML and manifest are still derived exclusively from the native
// SaveToXML result; these values never substitute for native resource data.
[[nodiscard]] NativeObjectArchiveResult capture_native_object_archive(
    hta::ai::Obj& object, std::string_view map_namespace,
    std::string_view resource_fingerprint, std::vector<Byte>& bytes);

[[nodiscard]] NativeObjectArchiveResult restore_native_object_archive_v2(
    const NativeObjectArchiveV2&, hta::ai::Obj& suspended_object);

[[nodiscard]] NativeObjectArchiveResult restore_native_object_archive(
    ByteView bytes, hta::ai::Obj& suspended_object);

} // namespace kraken::net

#endif // KRAKEN_NET_NATIVE_OBJECT_ARCHIVE_HPP
