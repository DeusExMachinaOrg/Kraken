#ifndef KRAKEN_NET_RESOURCE_FINGERPRINT_HPP
#define KRAKEN_NET_RESOURCE_FINGERPRINT_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kraken::net {

// The prefix is part of the SessionIdentity contract.  The digest is
// printable ASCII and is deliberately shorter than the identity field's
// 128-byte wire limit.
inline constexpr std::uint16_t kResourceFingerprintVersion = 2;
inline constexpr std::string_view kResourceFingerprintPrefix =
    "rfp2-sha256:";

// Ignore rules are intentionally supplied by the caller.  A one-component
// rule (for example "build" or "cache") matches that directory name at any
// depth.  A multi-component rule (for example "content/cache") matches that
// exact normalized path below install_root.  Matching is case-insensitive and
// uses '/' as the path separator.
struct ResourceFingerprintPolicy {
    std::vector<std::filesystem::path> ignored_directories;
};

// Each input is resolved below install_root.  It may name one regular file or
// one directory, in which case regular files below it are enumerated
// recursively.  Relative paths are relative to install_root; absolute paths
// are accepted only when they resolve below install_root.  The absolute
// install root and all filesystem timestamps are never hashed.
struct ResourceFingerprintRequest {
    std::filesystem::path install_root;
    std::vector<std::filesystem::path> inputs;
    ResourceFingerprintPolicy policy{};
};

enum class ResourceFingerprintErrorCode : std::uint8_t {
    None,
    InvalidInstallRoot,
    InvalidIgnorePolicy,
    MissingInput,
    InputOutsideInstallRoot,
    UnsupportedInput,
    UnreadableInput,
    DuplicateRelativePath,
    ChangedDuringRead,
};

struct ResourceFingerprintManifestStats {
    std::uint64_t input_count = 0;
    std::uint64_t file_count = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t ignored_directory_count = 0;
};

struct ResourceFingerprintResult {
    ResourceFingerprintErrorCode error = ResourceFingerprintErrorCode::None;
    std::filesystem::path error_path{};
    std::string error_message{};
    std::string digest{};
    ResourceFingerprintManifestStats stats{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == ResourceFingerprintErrorCode::None;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return succeeded();
    }
};

[[nodiscard]] const char* resource_fingerprint_error_name(
    ResourceFingerprintErrorCode) noexcept;

[[nodiscard]] ResourceFingerprintResult fingerprint_resources(
    const ResourceFingerprintRequest&);

[[nodiscard]] ResourceFingerprintResult fingerprint_resources(
    const std::filesystem::path& install_root,
    const std::vector<std::filesystem::path>& inputs,
    const ResourceFingerprintPolicy& policy = {});

} // namespace kraken::net

#endif // KRAKEN_NET_RESOURCE_FINGERPRINT_HPP
