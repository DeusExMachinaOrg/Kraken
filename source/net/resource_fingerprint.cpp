#include "net/resource_fingerprint.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace kraken::net {
namespace {

namespace fs = std::filesystem;

struct IgnoreRule {
    std::string normalized;
    bool basename = false;
};

struct FileEntry {
    fs::path physical_path;
    std::string relative_path;
};

struct NormalizedPath {
    std::string value;
    bool valid = false;
};

[[nodiscard]] std::string path_utf8(const fs::path& path)
{
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()),
                       value.size());
}

void lowercase_ascii(std::string& value) noexcept
{
    for (char& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') &&
            byte <= static_cast<unsigned char>('Z'))
            character = static_cast<char>(byte + ('a' - 'A'));
    }
}

[[nodiscard]] NormalizedPath normalized_relative_path(const fs::path& path)
{
    if (path.empty() || path.is_absolute())
        return {};

    const fs::path normalized = path.lexically_normal();
    if (normalized.empty() || normalized == fs::path{"."})
        return {};

    std::string value = path_utf8(normalized);
    if (value.empty() || value == "." || value == ".." ||
        value.starts_with("../"))
        return {};
    lowercase_ascii(value);
    return {std::move(value), true};
}

[[nodiscard]] std::string basename_of(std::string_view path)
{
    const std::size_t separator = path.find_last_of('/');
    return std::string(path.substr(separator == std::string_view::npos
                                       ? 0
                                       : separator + 1));
}

[[nodiscard]] ResourceFingerprintResult failure(
    const ResourceFingerprintErrorCode error, fs::path path,
    std::string message, const ResourceFingerprintManifestStats& stats = {})
{
    ResourceFingerprintResult result{};
    result.error = error;
    result.error_path = std::move(path);
    result.error_message = std::move(message);
    result.stats = stats;
    return result;
}

[[nodiscard]] bool is_within_root(const fs::path& root,
                                  const fs::path& candidate)
{
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty() || relative == fs::path{"."})
        return true;
    return normalized_relative_path(relative).valid;
}

[[nodiscard]] bool ignored_directory(std::string_view relative,
                                     const std::vector<IgnoreRule>& rules)
{
    for (const IgnoreRule& rule : rules) {
        if (rule.basename) {
            if (basename_of(relative) == rule.normalized)
                return true;
        } else if (relative == rule.normalized) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool add_file_entry(
    std::vector<FileEntry>& entries, std::unordered_set<std::string>& seen,
    const fs::path& root, const fs::path& physical_path,
    ResourceFingerprintResult& error_result)
{
    const fs::path relative = physical_path.lexically_relative(root);
    const NormalizedPath normalized = normalized_relative_path(relative);
    if (!normalized.valid) {
        error_result = failure(
            ResourceFingerprintErrorCode::InputOutsideInstallRoot,
            physical_path,
            "regular file does not have a valid relative install path");
        return false;
    }
    if (!seen.insert(normalized.value).second) {
        error_result = failure(
            ResourceFingerprintErrorCode::DuplicateRelativePath,
            physical_path,
            "more than one supplied input resolves to relative path '" +
                normalized.value + "'");
        return false;
    }
    entries.push_back({physical_path, normalized.value});
    return true;
}

[[nodiscard]] bool collect_directory(
    const fs::path& root, const fs::path& directory,
    const std::vector<IgnoreRule>& rules, std::vector<FileEntry>& entries,
    std::unordered_set<std::string>& seen,
    ResourceFingerprintManifestStats& stats,
    ResourceFingerprintResult& error_result)
{
    std::vector<fs::path> pending{directory};
    while (!pending.empty()) {
        const fs::path current = std::move(pending.back());
        pending.pop_back();

        const NormalizedPath relative = normalized_relative_path(
            current.lexically_relative(root));
        if (!relative.valid && current != root) {
            error_result = failure(
                ResourceFingerprintErrorCode::InputOutsideInstallRoot,
                current, "directory does not have a valid relative path");
            return false;
        }
        if (relative.valid && ignored_directory(relative.value, rules)) {
            ++stats.ignored_directory_count;
            continue;
        }

        std::error_code iterator_error;
        fs::directory_iterator iterator(current, {}, iterator_error);
        if (iterator_error) {
            error_result = failure(
                ResourceFingerprintErrorCode::UnreadableInput, current,
                "could not enumerate directory: " +
                    iterator_error.message());
            return false;
        }

        const fs::directory_iterator end;
        while (iterator != end) {
            const fs::path child = iterator->path();
            std::error_code status_error;
            const fs::file_status status = fs::symlink_status(child, status_error);
            if (status_error) {
                error_result = failure(
                    ResourceFingerprintErrorCode::UnreadableInput, child,
                    "could not inspect directory entry: " +
                        status_error.message());
                return false;
            }
            if (fs::is_symlink(status)) {
                error_result = failure(
                    ResourceFingerprintErrorCode::UnsupportedInput, child,
                    "symbolic links are not fingerprint inputs");
                return false;
            }
            if (fs::is_directory(status)) {
                const NormalizedPath child_relative = normalized_relative_path(
                    child.lexically_relative(root));
                if (!child_relative.valid) {
                    error_result = failure(
                        ResourceFingerprintErrorCode::InputOutsideInstallRoot,
                        child, "directory is outside install_root");
                    return false;
                }
                if (ignored_directory(child_relative.value, rules)) {
                    ++stats.ignored_directory_count;
                } else {
                    pending.push_back(child);
                }
            } else if (fs::is_regular_file(status)) {
                if (!add_file_entry(entries, seen, root, child, error_result))
                    return false;
            } else {
                error_result = failure(
                    ResourceFingerprintErrorCode::UnsupportedInput, child,
                    "directory contains a non-regular, non-directory entry");
                return false;
            }

            std::error_code increment_error;
            iterator.increment(increment_error);
            if (increment_error) {
                error_result = failure(
                    ResourceFingerprintErrorCode::UnreadableInput, current,
                    "could not continue directory enumeration: " +
                        increment_error.message());
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::string normalize_hex(const std::array<std::uint8_t, 32>& bytes)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        result.push_back(kHex[(byte >> 4) & 0x0f]);
        result.push_back(kHex[byte & 0x0f]);
    }
    return result;
}

// Self-contained SHA-256 keeps this helper independent of OpenSSL, BCrypt,
// or any other crypto library.  SHA-256's eight working lanes provide a
// deterministic 256-bit digest; the canonical path/length framing below
// prevents path and byte-stream concatenation ambiguities.
class Sha256 {
public:
    Sha256()
        : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}
    {
    }

    void update(const std::uint8_t* data, std::size_t size) noexcept
    {
        while (size != 0) {
            const std::size_t available = buffer_.size() - buffer_size_;
            const std::size_t count = std::min(available, size);
            for (std::size_t index = 0; index != count; ++index)
                buffer_[buffer_size_ + index] = data[index];
            buffer_size_ += count;
            data += count;
            size -= count;
            total_bytes_ += count;
            if (buffer_size_ == buffer_.size()) {
                compress(buffer_.data());
                buffer_size_ = 0;
            }
        }
    }

    void update(std::string_view value) noexcept
    {
        update(reinterpret_cast<const std::uint8_t*>(value.data()),
               value.size());
    }

    void update_u8(const std::uint8_t value) noexcept { update(&value, 1); }

    void update_u64(const std::uint64_t value) noexcept
    {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t index = 0; index != bytes.size(); ++index)
            bytes[bytes.size() - index - 1] =
                static_cast<std::uint8_t>(value >> (index * 8));
        update(bytes.data(), bytes.size());
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() const noexcept
    {
        Sha256 copy = *this;
        const std::uint64_t bit_count = copy.total_bytes_ * 8;
        copy.update_u8(0x80u);
        while (copy.buffer_size_ != 56)
            copy.update_u8(0);
        copy.update_u64(bit_count);

        std::array<std::uint8_t, 32> result{};
        for (std::size_t index = 0; index != copy.state_.size(); ++index) {
            result[index * 4 + 0] =
                static_cast<std::uint8_t>(copy.state_[index] >> 24);
            result[index * 4 + 1] =
                static_cast<std::uint8_t>(copy.state_[index] >> 16);
            result[index * 4 + 2] =
                static_cast<std::uint8_t>(copy.state_[index] >> 8);
            result[index * 4 + 3] = static_cast<std::uint8_t>(
                copy.state_[index]);
        }
        return result;
    }

private:
    static constexpr std::array<std::uint32_t, 64> kRoundConstants{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    static constexpr std::uint32_t rotate_right(const std::uint32_t value,
                                                 const unsigned count) noexcept
    {
        return (value >> count) | (value << (32u - count));
    }

    static constexpr std::uint32_t choose(const std::uint32_t x,
                                          const std::uint32_t y,
                                          const std::uint32_t z) noexcept
    {
        return (x & y) ^ (~x & z);
    }

    static constexpr std::uint32_t majority(const std::uint32_t x,
                                            const std::uint32_t y,
                                            const std::uint32_t z) noexcept
    {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    static constexpr std::uint32_t big_sigma0(const std::uint32_t value) noexcept
    {
        return rotate_right(value, 2) ^ rotate_right(value, 13) ^
               rotate_right(value, 22);
    }

    static constexpr std::uint32_t big_sigma1(const std::uint32_t value) noexcept
    {
        return rotate_right(value, 6) ^ rotate_right(value, 11) ^
               rotate_right(value, 25);
    }

    static constexpr std::uint32_t small_sigma0(
        const std::uint32_t value) noexcept
    {
        return rotate_right(value, 7) ^ rotate_right(value, 18) ^ (value >> 3);
    }

    static constexpr std::uint32_t small_sigma1(
        const std::uint32_t value) noexcept
    {
        return rotate_right(value, 17) ^ rotate_right(value, 19) ^ (value >> 10);
    }

    void compress(const std::uint8_t* block) noexcept
    {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index != 16; ++index) {
            schedule[index] =
                (static_cast<std::uint32_t>(block[index * 4 + 0]) << 24) |
                (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                static_cast<std::uint32_t>(block[index * 4 + 3]);
        }
        for (std::size_t index = 16; index != schedule.size(); ++index)
            schedule[index] = small_sigma1(schedule[index - 2]) +
                              schedule[index - 7] +
                              small_sigma0(schedule[index - 15]) +
                              schedule[index - 16];

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index != schedule.size(); ++index) {
            const std::uint32_t t1 = h + big_sigma1(e) + choose(e, f, g) +
                                     kRoundConstants[index] + schedule[index];
            const std::uint32_t t2 = big_sigma0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

constexpr std::array<std::uint32_t, 64> Sha256::kRoundConstants;

[[nodiscard]] bool update_file(Sha256& hash, const FileEntry& entry,
                               ResourceFingerprintManifestStats& stats,
                               ResourceFingerprintResult& error_result)
{
    std::error_code size_error;
    const std::uintmax_t file_size = fs::file_size(entry.physical_path, size_error);
    if (size_error || file_size > std::numeric_limits<std::uint64_t>::max()) {
        error_result = failure(
            ResourceFingerprintErrorCode::UnreadableInput, entry.physical_path,
            "could not read regular-file size");
        return false;
    }

    hash.update_u8(static_cast<std::uint8_t>('F'));
    hash.update_u64(static_cast<std::uint64_t>(entry.relative_path.size()));
    hash.update(entry.relative_path);
    hash.update_u64(static_cast<std::uint64_t>(file_size));

    std::ifstream stream(entry.physical_path, std::ios::binary);
    if (!stream) {
        error_result = failure(
            ResourceFingerprintErrorCode::UnreadableInput, entry.physical_path,
            "could not open regular file for reading");
        return false;
    }

    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t bytes_read = 0;
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if (count > 0) {
            const auto unsigned_count = static_cast<std::uint64_t>(count);
            if (bytes_read > std::numeric_limits<std::uint64_t>::max() -
                                  unsigned_count) {
                error_result = failure(
                    ResourceFingerprintErrorCode::ChangedDuringRead,
                    entry.physical_path, "file size exceeded the hash limit");
                return false;
            }
            bytes_read += unsigned_count;
            hash.update(buffer.data(), static_cast<std::size_t>(count));
        }
        if (stream.eof())
            break;
        if (stream.bad() || !stream) {
            error_result = failure(
                ResourceFingerprintErrorCode::UnreadableInput,
                entry.physical_path, "read failed before end of file");
            return false;
        }
    }

    if (bytes_read != file_size) {
        error_result = failure(
            ResourceFingerprintErrorCode::ChangedDuringRead, entry.physical_path,
            "file size changed while it was being fingerprinted");
        return false;
    }

    std::error_code final_size_error;
    const std::uintmax_t final_size =
        fs::file_size(entry.physical_path, final_size_error);
    if (final_size_error || final_size != file_size) {
        error_result = failure(
            ResourceFingerprintErrorCode::ChangedDuringRead, entry.physical_path,
            "file size changed while it was being fingerprinted");
        return false;
    }

    if (stats.total_bytes > std::numeric_limits<std::uint64_t>::max() -
                                static_cast<std::uint64_t>(file_size)) {
        error_result = failure(
            ResourceFingerprintErrorCode::ChangedDuringRead, entry.physical_path,
            "manifest byte count overflow");
        return false;
    }
    stats.total_bytes += static_cast<std::uint64_t>(file_size);
    ++stats.file_count;
    return true;
}

[[nodiscard]] ResourceFingerprintResult fingerprint_impl(
    const ResourceFingerprintRequest& request)
{
    if (request.install_root.empty())
        return failure(ResourceFingerprintErrorCode::InvalidInstallRoot, {},
                       "install_root is empty");

    std::error_code root_status_error;
    const fs::file_status root_status =
        fs::status(request.install_root, root_status_error);
    if (root_status_error || !fs::exists(root_status)) {
        return failure(ResourceFingerprintErrorCode::InvalidInstallRoot,
                       request.install_root,
                       "install_root does not exist or is unreadable");
    }
    if (!fs::is_directory(root_status))
        return failure(ResourceFingerprintErrorCode::InvalidInstallRoot,
                       request.install_root, "install_root is not a directory");

    std::error_code root_error;
    const fs::path root = fs::weakly_canonical(request.install_root, root_error);
    if (root_error || root.empty())
        return failure(ResourceFingerprintErrorCode::InvalidInstallRoot,
                       request.install_root,
                       "install_root could not be normalized: " +
                           root_error.message());

    std::vector<IgnoreRule> rules;
    rules.reserve(request.policy.ignored_directories.size());
    for (const fs::path& ignored : request.policy.ignored_directories) {
        const NormalizedPath normalized = normalized_relative_path(ignored);
        if (!normalized.valid) {
            return failure(ResourceFingerprintErrorCode::InvalidIgnorePolicy,
                           ignored,
                           "ignored directory must be a non-empty relative path");
        }
        rules.push_back(
            {normalized.value, normalized.value.find('/') == std::string::npos});
    }

    ResourceFingerprintManifestStats stats{};
    stats.input_count = request.inputs.size();
    std::vector<FileEntry> entries;
    std::unordered_set<std::string> seen;
    ResourceFingerprintResult error_result{};

    for (const fs::path& input : request.inputs) {
        std::error_code input_error;
        const fs::path unresolved = input.is_absolute() ? input : root / input;
        const fs::path resolved = fs::weakly_canonical(unresolved, input_error);
        if (input_error) {
            const auto status = fs::status(unresolved, input_error);
            if (input_error || !fs::exists(status)) {
                return failure(ResourceFingerprintErrorCode::MissingInput, input,
                               "supplied input does not exist");
            }
            return failure(ResourceFingerprintErrorCode::UnreadableInput, input,
                           "supplied input could not be normalized: " +
                               input_error.message());
        }
        if (!is_within_root(root, resolved))
            return failure(ResourceFingerprintErrorCode::InputOutsideInstallRoot,
                           input, "supplied input is outside install_root");

        std::error_code status_error;
        const fs::file_status status = fs::symlink_status(resolved, status_error);
        if (status_error || !fs::exists(status))
            return failure(ResourceFingerprintErrorCode::MissingInput, input,
                           "supplied input does not exist");
        if (fs::is_symlink(status))
            return failure(ResourceFingerprintErrorCode::UnsupportedInput, input,
                           "symbolic links are not fingerprint inputs");
        if (fs::is_directory(status)) {
            const NormalizedPath relative = normalized_relative_path(
                resolved.lexically_relative(root));
            if (relative.valid && ignored_directory(relative.value, rules)) {
                ++stats.ignored_directory_count;
                continue;
            }
            if (!collect_directory(root, resolved, rules, entries, seen, stats,
                                   error_result))
                return error_result;
        } else if (fs::is_regular_file(status)) {
            if (!add_file_entry(entries, seen, root, resolved, error_result))
                return error_result;
        } else {
            return failure(ResourceFingerprintErrorCode::UnsupportedInput, input,
                           "supplied input is not a regular file or directory");
        }
    }

    std::sort(entries.begin(), entries.end(),
              [](const FileEntry& left, const FileEntry& right) {
                  return left.relative_path < right.relative_path;
              });

    Sha256 hash;
    hash.update("Kraken resource fingerprint\0rfp2\0");
    for (const FileEntry& entry : entries) {
        if (!update_file(hash, entry, stats, error_result))
            return error_result;
    }
    hash.update_u8(static_cast<std::uint8_t>('M'));
    hash.update_u64(stats.file_count);
    hash.update_u64(stats.total_bytes);

    const auto bytes = hash.finish();
    ResourceFingerprintResult result{};
    result.digest = std::string(kResourceFingerprintPrefix) + normalize_hex(bytes);
    result.stats = stats;
    return result;
}

} // namespace

const char* resource_fingerprint_error_name(
    const ResourceFingerprintErrorCode error) noexcept
{
    switch (error) {
    case ResourceFingerprintErrorCode::None:
        return "none";
    case ResourceFingerprintErrorCode::InvalidInstallRoot:
        return "invalid-install-root";
    case ResourceFingerprintErrorCode::InvalidIgnorePolicy:
        return "invalid-ignore-policy";
    case ResourceFingerprintErrorCode::MissingInput:
        return "missing-input";
    case ResourceFingerprintErrorCode::InputOutsideInstallRoot:
        return "input-outside-install-root";
    case ResourceFingerprintErrorCode::UnsupportedInput:
        return "unsupported-input";
    case ResourceFingerprintErrorCode::UnreadableInput:
        return "unreadable-input";
    case ResourceFingerprintErrorCode::DuplicateRelativePath:
        return "duplicate-relative-path";
    case ResourceFingerprintErrorCode::ChangedDuringRead:
        return "changed-during-read";
    }
    return "unknown";
}

ResourceFingerprintResult fingerprint_resources(
    const ResourceFingerprintRequest& request)
{
    return fingerprint_impl(request);
}

ResourceFingerprintResult fingerprint_resources(
    const std::filesystem::path& install_root,
    const std::vector<std::filesystem::path>& inputs,
    const ResourceFingerprintPolicy& policy)
{
    return fingerprint_resources({install_root, inputs, policy});
}

} // namespace kraken::net
