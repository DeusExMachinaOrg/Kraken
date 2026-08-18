#include "net/static_world_identity.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace kraken::net {
namespace {

constexpr StaticWorldId kFnvOffset = 14695981039346656037ull;
constexpr StaticWorldId kFnvPrime = 1099511628211ull;

void hash_byte(StaticWorldId& hash, const unsigned char value) noexcept
{
    hash ^= static_cast<StaticWorldId>(value);
    hash *= kFnvPrime;
}

void hash_bytes(StaticWorldId& hash, const std::string_view value) noexcept
{
    for (const unsigned char byte : value)
        hash_byte(hash, byte);
}

void hash_u64(StaticWorldId& hash, const StaticWorldId value) noexcept
{
    for (unsigned int shift = 0; shift < 64; shift += 8)
        hash_byte(hash, static_cast<unsigned char>(value >> shift));
}

bool is_control(const unsigned char value) noexcept
{
    return value < 0x20u || value == 0x7fu;
}

bool valid_hierarchical_path(const std::string_view path) noexcept
{
    if (path.empty() || path.front() == '/' || path.back() == '/')
        return false;

    std::size_t segment_start = 0;
    while (segment_start < path.size()) {
        const std::size_t separator = path.find('/', segment_start);
        const std::size_t segment_end =
            separator == std::string_view::npos ? path.size() : separator;
        const std::string_view segment =
            path.substr(segment_start, segment_end - segment_start);

        if (segment.empty() || segment == "." || segment == "..")
            return false;

        for (const unsigned char byte : segment) {
            // A relative slash-separated path is deliberate.  Rejecting
            // backslashes and colons also prevents drive/UNC install roots
            // from entering the identity key.
            if (byte == '\\' || byte == ':' || is_control(byte))
                return false;
        }

        if (separator == std::string_view::npos)
            break;
        segment_start = separator + 1;
    }
    return true;
}

bool valid_identity_text(const std::string_view value) noexcept
{
    if (value.empty())
        return false;
    for (const unsigned char byte : value) {
        if (is_control(byte))
            return false;
    }
    return true;
}

void append_field(std::string& output, const std::string_view label,
                  const std::string_view value)
{
    output.append(label);
    output.push_back('=');
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
    output.push_back(';');
}

std::string path_key(const StaticWorldSourceRecord& record)
{
    std::string output;
    output.reserve(24u + record.map_namespace.size() +
                   record.object_path.size());
    output.append("static-world-path/v1|");
    append_field(output, "map", record.map_namespace);
    append_field(output, "path", record.object_path);
    return output;
}

struct BuildItem {
    StaticWorldSourceRecord source;
    std::string path_key;
    std::string canonical_key;
    StaticWorldId id = kInvalidStaticWorldId;
    std::size_t input_index = kInvalidStaticWorldIndex;
};

StaticWorldIndexBuildResult make_error(const StaticWorldIndexError error,
                                       const std::size_t record_index,
                                       const std::size_t conflict =
                                           kInvalidStaticWorldIndex) noexcept
{
    return {error, record_index, conflict};
}

template <typename Item>
bool same_path(const Item& left, const Item& right) noexcept
{
    return left.path_key == right.path_key;
}

} // namespace

StaticWorldId static_world_id_hash(const std::string_view canonical_key) noexcept
{
    StaticWorldId hash = kFnvOffset;
    hash_bytes(hash, "kraken/static-world-id/v1\0");
    hash_bytes(hash, canonical_key);

    // Zero is reserved for an unmatched object and the high bit is the
    // host-issued dynamic namespace.  Collision validation compares the full
    // canonical key after this projection, so the namespace split is safe.
    hash &= kStaticWorldIdPayloadMask;
    return hash == kInvalidStaticWorldId ? 1u : hash;
}

StaticWorldIndexError validate_static_world_record(
    const StaticWorldSourceRecord& record) noexcept
{
    if (record.map_namespace.empty())
        return StaticWorldIndexError::EmptyMapNamespace;
    if (!valid_hierarchical_path(record.map_namespace))
        return StaticWorldIndexError::MalformedMapNamespace;
    if (record.object_path.empty())
        return StaticWorldIndexError::EmptyPath;
    if (!valid_hierarchical_path(record.object_path))
        return StaticWorldIndexError::MalformedPath;
    if (record.prototype_identity.empty())
        return StaticWorldIndexError::EmptyPrototypeIdentity;
    if (!valid_identity_text(record.prototype_identity))
        return StaticWorldIndexError::MalformedPrototypeIdentity;
    return StaticWorldIndexError::None;
}

std::string static_world_canonical_key(
    const StaticWorldSourceRecord& record)
{
    if (validate_static_world_record(record) != StaticWorldIndexError::None)
        return {};

    std::string output;
    output.reserve(32u + record.map_namespace.size() +
                   record.object_path.size() + record.prototype_identity.size());
    output.append("static-world/v1|");
    append_field(output, "map", record.map_namespace);
    append_field(output, "path", record.object_path);
    append_field(output, "prototype", record.prototype_identity);
    return output;
}

StaticWorldIndexBuildResult StaticWorldIdentityIndex::build(
    const std::span<const StaticWorldSourceRecord> records,
    StaticWorldIdentityIndex& output, const StaticWorldIndexOptions options)
{
    return output.install(records, options);
}

StaticWorldIndexBuildResult StaticWorldIdentityIndex::install(
    const std::span<const StaticWorldSourceRecord> records,
    const StaticWorldIndexOptions options)
{
    if (options.hash_function == nullptr)
        return make_error(StaticWorldIndexError::InvalidHashFunction,
                          kInvalidStaticWorldIndex);

    std::vector<StaticWorldIndexEntry> built;
    const StaticWorldIndexBuildResult result =
        build_entries(records, options.hash_function, built);
    if (!result)
        return result;

    m_entries.swap(built);
    m_hash_function = options.hash_function;
    return result;
}

StaticWorldIndexBuildResult StaticWorldIdentityIndex::build_entries(
    const std::span<const StaticWorldSourceRecord> records,
    const StaticWorldHashFunction hash_function,
    std::vector<StaticWorldIndexEntry>& output) const
{
    std::vector<BuildItem> items;
    items.reserve(records.size());

    for (std::size_t index = 0; index < records.size(); ++index) {
        const StaticWorldSourceRecord& record = records[index];
        const StaticWorldIndexError validation =
            validate_static_world_record(record);
        if (validation != StaticWorldIndexError::None)
            return make_error(validation, index);

        BuildItem item;
        item.source = record;
        item.path_key = path_key(record);
        item.canonical_key = static_world_canonical_key(record);
        item.input_index = index;
        item.id = hash_function(item.canonical_key);
        if (item.id == kInvalidStaticWorldId)
            return make_error(StaticWorldIndexError::InvalidHash, index);
        items.push_back(std::move(item));
    }

    std::sort(items.begin(), items.end(),
              [](const BuildItem& left, const BuildItem& right) {
                  if (left.path_key != right.path_key)
                      return left.path_key < right.path_key;
                  return left.canonical_key < right.canonical_key;
              });

    for (std::size_t index = 1; index < items.size(); ++index) {
        if (!same_path(items[index - 1], items[index]))
            continue;
        return make_error(StaticWorldIndexError::DuplicateFullPath,
                          items[index].input_index,
                          items[index - 1].input_index);
    }

    // Collision checks deliberately compare the complete key, not just the
    // 64-bit value.  The source vector remains canonical-key sorted below;
    // this temporary order has no observable effect on the resulting index.
    std::vector<std::size_t> hash_order(items.size());
    for (std::size_t index = 0; index < hash_order.size(); ++index)
        hash_order[index] = index;
    std::sort(hash_order.begin(), hash_order.end(),
              [&items](const std::size_t left, const std::size_t right) {
                  if (items[left].id != items[right].id)
                      return items[left].id < items[right].id;
                  return items[left].canonical_key < items[right].canonical_key;
              });

    for (std::size_t index = 1; index < hash_order.size(); ++index) {
        const BuildItem& left = items[hash_order[index - 1]];
        const BuildItem& right = items[hash_order[index]];
        if (left.id != right.id)
            continue;
        if (left.canonical_key == right.canonical_key)
            return make_error(StaticWorldIndexError::DuplicateFullKey,
                              right.input_index, left.input_index);
        return make_error(StaticWorldIndexError::HashCollision,
                          right.input_index, left.input_index);
    }

    output.reserve(items.size());
    for (std::size_t source_index = 0; source_index < items.size();
         ++source_index) {
        BuildItem& item = items[source_index];
        output.push_back({item.id, std::move(item.source),
                          std::move(item.canonical_key), source_index});
    }
    return {};
}

void StaticWorldIdentityIndex::clear() noexcept
{
    m_entries.clear();
}

const StaticWorldIndexEntry* StaticWorldIdentityIndex::entry_at(
    const std::size_t source_index) const noexcept
{
    if (source_index >= m_entries.size())
        return nullptr;
    return &m_entries[source_index];
}

std::optional<StaticWorldId> StaticWorldIdentityIndex::id_for(
    const StaticWorldSourceRecord& record) const
{
    const std::string key = static_world_canonical_key(record);
    if (key.empty())
        return std::nullopt;
    return id_for_key(key);
}

std::optional<StaticWorldId> StaticWorldIdentityIndex::id_for_key(
    const std::string_view canonical_key) const noexcept
{
    const auto it = std::lower_bound(
        m_entries.begin(), m_entries.end(), canonical_key,
        [](const StaticWorldIndexEntry& entry, const std::string_view key) {
            return entry.canonical_key < key;
        });
    if (it == m_entries.end() || it->canonical_key != canonical_key)
        return std::nullopt;
    return it->id;
}

const StaticWorldIndexEntry* StaticWorldIdentityIndex::find_by_id(
    const StaticWorldId id, const std::string_view canonical_key) const noexcept
{
    if (id == kInvalidStaticWorldId)
        return nullptr;
    const auto it = std::lower_bound(
        m_entries.begin(), m_entries.end(), canonical_key,
        [](const StaticWorldIndexEntry& entry, const std::string_view key) {
            return entry.canonical_key < key;
        });
    if (it == m_entries.end() || it->canonical_key != canonical_key ||
        it->id != id)
        return nullptr;
    return &*it;
}

bool StaticWorldIdentityIndex::verify_id(
    const StaticWorldId id, const std::string_view canonical_key) const noexcept
{
    return find_by_id(id, canonical_key) != nullptr;
}

const StaticWorldIndexEntry* StaticWorldIdentityIndex::find(
    const StaticWorldSourceRecord& record) const
{
    const std::string key = static_world_canonical_key(record);
    if (key.empty())
        return nullptr;
    const auto it = std::lower_bound(
        m_entries.begin(), m_entries.end(), key,
        [](const StaticWorldIndexEntry& entry, const std::string& value) {
            return entry.canonical_key < value;
        });
    if (it == m_entries.end() || it->canonical_key != key)
        return nullptr;
    return &*it;
}

StaticWorldMatchResult match_static_world_records(
    const StaticWorldIdentityIndex& source_index,
    const std::span<const StaticWorldPostLoadRecord> post_load_records)
{
    StaticWorldMatchResult result;
    result.ids_by_post_load_index.assign(post_load_records.size(),
                                         kInvalidStaticWorldId);

    struct PostItem {
        std::string path_key;
        std::string canonical_key;
        std::size_t input_index = kInvalidStaticWorldIndex;
        std::size_t source_index = kInvalidStaticWorldIndex;
    };

    std::vector<PostItem> items;
    items.reserve(post_load_records.size());
    for (std::size_t index = 0; index < post_load_records.size(); ++index) {
        const StaticWorldPostLoadRecord& record = post_load_records[index];
        const StaticWorldIndexError validation =
            validate_static_world_record(record);
        if (validation != StaticWorldIndexError::None) {
            result.error = validation;
            result.record_index = index;
            result.matches.clear();
            result.unmatched_post_load_indices.clear();
            result.ids_by_post_load_index.clear();
            return result;
        }

        const std::string key = static_world_canonical_key(record);
        const auto source_id = source_index.id_for_key(key);
        const StaticWorldIndexEntry* source_entry =
            source_id ? source_index.find_by_id(*source_id, key) : nullptr;

        PostItem item;
        item.path_key = path_key(record);
        item.canonical_key = key;
        item.input_index = index;
        if (source_entry != nullptr) {
            item.source_index = source_entry->source_index;
        }
        items.push_back(std::move(item));
    }

    std::sort(items.begin(), items.end(),
              [](const PostItem& left, const PostItem& right) {
                  if (left.path_key != right.path_key)
                      return left.path_key < right.path_key;
                  return left.canonical_key < right.canonical_key;
              });
    for (std::size_t index = 1; index < items.size(); ++index) {
        if (items[index - 1].path_key != items[index].path_key)
            continue;
        result.error = StaticWorldIndexError::DuplicateFullPath;
        result.record_index = items[index].input_index;
        result.conflicting_record_index = items[index - 1].input_index;
        result.matches.clear();
        result.unmatched_post_load_indices.clear();
        result.ids_by_post_load_index.clear();
        return result;
    }

    result.matches.reserve(items.size());
    result.unmatched_post_load_indices.reserve(items.size());
    for (const PostItem& item : items) {
        if (item.source_index == kInvalidStaticWorldIndex) {
            result.unmatched_post_load_indices.push_back(item.input_index);
            continue;
        }

        const StaticWorldIndexEntry* source_entry =
            source_index.entry_at(item.source_index);
        if (source_entry == nullptr ||
            source_entry->canonical_key != item.canonical_key) {
            // This cannot happen for a successfully built index, but retaining
            // the check makes the full-key verification seam explicit.
            result.error = StaticWorldIndexError::HashCollision;
            result.record_index = item.input_index;
            result.matches.clear();
            result.unmatched_post_load_indices.clear();
            result.ids_by_post_load_index.clear();
            return result;
        }

        result.ids_by_post_load_index[item.input_index] = source_entry->id;
        result.matches.push_back(
            {source_entry->id, source_entry->source_index, item.input_index});
    }

    std::vector<StaticWorldId> membership;
    membership.reserve(result.matches.size());
    for (const StaticWorldMatch& match : result.matches)
        membership.push_back(match.id);
    result.membership_digest = static_world_membership_digest(membership);
    return result;
}

StaticWorldId static_world_membership_digest(
    const std::span<const StaticWorldId> ids)
{
    std::vector<StaticWorldId> canonical_ids;
    canonical_ids.reserve(ids.size());
    for (const StaticWorldId id : ids) {
        if (id != kInvalidStaticWorldId)
            canonical_ids.push_back(id);
    }
    std::sort(canonical_ids.begin(), canonical_ids.end());
    canonical_ids.erase(
        std::unique(canonical_ids.begin(), canonical_ids.end()),
        canonical_ids.end());

    StaticWorldId digest = kFnvOffset;
    hash_bytes(digest, "kraken/static-world-membership/v1\0");
    hash_u64(digest, static_cast<StaticWorldId>(canonical_ids.size()));
    for (const StaticWorldId id : canonical_ids)
        hash_u64(digest, id);
    return digest == kInvalidStaticWorldId ? 1u : digest;
}

bool StaticWorldMembershipStabilityGate::observe_digest(
    const StaticWorldId digest) noexcept
{
    if (!m_have_digest || digest != m_last_digest) {
        m_have_digest = true;
        m_last_digest = digest;
        m_consecutive = 1;
        m_stable = false;
        return false;
    }

    if (m_consecutive != (std::numeric_limits<std::size_t>::max)())
        ++m_consecutive;
    m_stable = m_consecutive >= 2;
    return m_stable;
}

bool StaticWorldMembershipStabilityGate::observe(
    const std::span<const StaticWorldId> membership_ids)
{
    return observe_digest(static_world_membership_digest(membership_ids));
}

bool StaticWorldMembershipStabilityGate::observe(
    const StaticWorldMatchResult& result) noexcept
{
    if (!result.ok()) {
        reset();
        return false;
    }
    return observe_digest(result.membership_digest);
}

void StaticWorldMembershipStabilityGate::reset() noexcept
{
    m_last_digest = 0;
    m_consecutive = 0;
    m_have_digest = false;
    m_stable = false;
}

} // namespace kraken::net
