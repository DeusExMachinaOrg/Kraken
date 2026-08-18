#ifndef KRAKEN_NET_STATIC_WORLD_IDENTITY_HPP
#define KRAKEN_NET_STATIC_WORLD_IDENTITY_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kraken::net {

// A static-world identity is derived exclusively from map data.  It is not an
// ObjId, a pointer, an enumeration ordinal, or an installation-root path.
using StaticWorldId = std::uint64_t;

inline constexpr StaticWorldId kInvalidStaticWorldId = 0;
// The high bit is reserved globally for host-issued dynamic object IDs.
// Static hashes therefore use the lower 63 bits; full canonical keys are
// still retained and compared so truncation can never hide a collision.
inline constexpr StaticWorldId kStaticWorldIdPayloadMask =
    (std::numeric_limits<StaticWorldId>::max)() >> 1;
inline constexpr std::size_t kInvalidStaticWorldIndex =
    (std::numeric_limits<std::size_t>::max)();

// These values are intentionally data-oriented.  A sidecar/XML adapter can
// fill them while parsing source records, and an engine adapter can fill the
// same shape after the world has loaded.
struct StaticWorldSourceRecord {
    std::string map_namespace;
    std::string object_path;
    std::string prototype_identity;
};

using StaticWorldPostLoadRecord = StaticWorldSourceRecord;
using StaticWorldRecord = StaticWorldSourceRecord;

enum class StaticWorldIndexError : std::uint8_t {
    None = 0,
    EmptyMapNamespace,
    MalformedMapNamespace,
    EmptyPath,
    MalformedPath,
    EmptyPrototypeIdentity,
    MalformedPrototypeIdentity,
    DuplicateFullPath,
    DuplicateFullKey,
    HashCollision,
    InvalidHash,
    InvalidHashFunction,

    // Readable aliases for adapters that use shorter terminology.
    DuplicatePath = DuplicateFullPath,
    Collision = HashCollision,
};

struct StaticWorldIndexBuildResult {
    StaticWorldIndexError error = StaticWorldIndexError::None;
    std::size_t record_index = kInvalidStaticWorldIndex;
    std::size_t conflicting_record_index = kInvalidStaticWorldIndex;

    [[nodiscard]] bool ok() const noexcept
    {
        return error == StaticWorldIndexError::None;
    }

    explicit operator bool() const noexcept { return ok(); }
};

// The default is a fixed FNV-1a based function.  The injectable function is
// useful for an adapter's validation harness: a constant function can prove
// that the index rejects a 64-bit hash collision instead of silently
// assigning the same ID to two full keys.
using StaticWorldHashFunction = StaticWorldId (*)(std::string_view);

[[nodiscard]] StaticWorldId static_world_id_hash(
    std::string_view canonical_key) noexcept;

// Same operation as static_world_id_hash(), named explicitly for callers
// that treat the value as an identity rather than a generic hash.
[[nodiscard]] inline StaticWorldId static_world_id_for_key(
    std::string_view canonical_key) noexcept
{
    return static_world_id_hash(canonical_key);
}

[[nodiscard]] StaticWorldIndexError validate_static_world_record(
    const StaticWorldSourceRecord& record) noexcept;

// Canonical encoding is length-delimited, so field contents cannot create an
// ambiguous key.  An invalid record produces an empty string; callers that
// need the reason should use validate_static_world_record() first.
[[nodiscard]] std::string static_world_canonical_key(
    const StaticWorldSourceRecord& record);

[[nodiscard]] inline std::string canonical_static_world_key(
    const StaticWorldSourceRecord& record)
{
    return static_world_canonical_key(record);
}

struct StaticWorldIndexOptions {
    StaticWorldHashFunction hash_function = &static_world_id_hash;
};

struct StaticWorldIndexEntry {
    StaticWorldId id = kInvalidStaticWorldId;
    StaticWorldSourceRecord source;
    std::string canonical_key;

    // This is the canonical-key order in the source index, never the input
    // enumeration order.  It is therefore safe to use as a sidecar source
    // record reference across installations and loads.
    std::size_t source_index = kInvalidStaticWorldIndex;
};

class StaticWorldIdentityIndex final {
public:
    StaticWorldIdentityIndex() = default;

    [[nodiscard]] static StaticWorldIndexBuildResult build(
        std::span<const StaticWorldSourceRecord> records,
        StaticWorldIdentityIndex& output,
        StaticWorldIndexOptions options = {});

    // Build transactionally: a rejected input leaves the existing index
    // unchanged.
    [[nodiscard]] StaticWorldIndexBuildResult install(
        std::span<const StaticWorldSourceRecord> records,
        StaticWorldIndexOptions options = {});

    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }

    [[nodiscard]] const std::vector<StaticWorldIndexEntry>& entries() const
        noexcept
    {
        return m_entries;
    }

    [[nodiscard]] const StaticWorldIndexEntry* entry_at(
        std::size_t source_index) const noexcept;

    [[nodiscard]] std::optional<StaticWorldId> id_for(
        const StaticWorldSourceRecord& record) const;

    [[nodiscard]] std::optional<StaticWorldId> id_for_key(
        std::string_view canonical_key) const noexcept;

    // Lookup by both ID and full key is the collision-verification seam for
    // receivers.  A matching hash alone is never sufficient.
    [[nodiscard]] const StaticWorldIndexEntry* find_by_id(
        StaticWorldId id, std::string_view canonical_key) const noexcept;

    [[nodiscard]] bool verify_id(StaticWorldId id,
                                 std::string_view canonical_key) const noexcept;

    [[nodiscard]] const StaticWorldIndexEntry* find(
        const StaticWorldSourceRecord& record) const;

private:
    [[nodiscard]] StaticWorldIndexBuildResult build_entries(
        std::span<const StaticWorldSourceRecord> records,
        StaticWorldHashFunction hash_function,
        std::vector<StaticWorldIndexEntry>& output) const;

    std::vector<StaticWorldIndexEntry> m_entries;
    StaticWorldHashFunction m_hash_function = &static_world_id_hash;
};

using StaticWorldSourceIndex = StaticWorldIdentityIndex;
using StaticWorldIndex = StaticWorldIdentityIndex;

struct StaticWorldMatch {
    StaticWorldId id = kInvalidStaticWorldId;
    std::size_t source_index = kInvalidStaticWorldIndex;
    std::size_t post_load_record_index = kInvalidStaticWorldIndex;
};

struct StaticWorldMatchResult {
    StaticWorldIndexError error = StaticWorldIndexError::None;
    std::size_t record_index = kInvalidStaticWorldIndex;
    std::size_t conflicting_record_index = kInvalidStaticWorldIndex;

    // Matches are returned in source canonical-key order.  The aligned ID
    // vector preserves the caller's post-load indexes and uses zero for an
    // unmatched/dynamic object.
    std::vector<StaticWorldMatch> matches;
    std::vector<std::size_t> unmatched_post_load_indices;
    std::vector<StaticWorldId> ids_by_post_load_index;
    StaticWorldId membership_digest = 0;

    [[nodiscard]] bool ok() const noexcept
    {
        return error == StaticWorldIndexError::None;
    }

    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] bool assigned(std::size_t post_load_record_index) const
        noexcept
    {
        return post_load_record_index < ids_by_post_load_index.size() &&
               ids_by_post_load_index[post_load_record_index] !=
                   kInvalidStaticWorldId;
    }
};

// Match is exact on map namespace, full hierarchical path, and prototype
// identity.  A valid record absent from the source index is intentionally
// left with kInvalidStaticWorldId; this is how dynamic/unmatched objects stay
// outside the static identity space.
[[nodiscard]] StaticWorldMatchResult match_static_world_records(
    const StaticWorldIdentityIndex& source_index,
    std::span<const StaticWorldPostLoadRecord> post_load_records);

[[nodiscard]] inline StaticWorldMatchResult match_post_load_records(
    const StaticWorldIdentityIndex& source_index,
    std::span<const StaticWorldPostLoadRecord> post_load_records)
{
    return match_static_world_records(source_index, post_load_records);
}

// Membership is a set of static IDs, not an enumeration sequence.  The
// digest sorts and de-duplicates IDs and ignores the invalid/unmatched value.
[[nodiscard]] StaticWorldId static_world_membership_digest(
    std::span<const StaticWorldId> ids);

[[nodiscard]] inline StaticWorldId static_world_membership_digest(
    const StaticWorldMatchResult& result)
{
    return result.membership_digest;
}

class StaticWorldMembershipStabilityGate final {
public:
    // Returns true only after the same digest has been observed twice in a
    // row.  A changed digest starts a new one-observation run.
    [[nodiscard]] bool observe_digest(StaticWorldId digest) noexcept;

    [[nodiscard]] bool observe(
        std::span<const StaticWorldId> membership_ids);

    [[nodiscard]] bool observe(const StaticWorldMatchResult& result) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool stable() const noexcept { return m_stable; }
    [[nodiscard]] StaticWorldId current_digest() const noexcept
    {
        return m_have_digest ? m_last_digest : 0;
    }
    [[nodiscard]] std::size_t consecutive_digest_count() const noexcept
    {
        return m_consecutive;
    }

private:
    StaticWorldId m_last_digest = 0;
    std::size_t m_consecutive = 0;
    bool m_have_digest = false;
    bool m_stable = false;
};

using StaticWorldStabilityGate = StaticWorldMembershipStabilityGate;

} // namespace kraken::net

#endif // KRAKEN_NET_STATIC_WORLD_IDENTITY_HPP
