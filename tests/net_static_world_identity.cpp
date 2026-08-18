#include "net/static_world_identity.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

using kraken::net::StaticWorldIndexError;
using kraken::net::StaticWorldIdentityIndex;
using kraken::net::StaticWorldMembershipStabilityGate;
using kraken::net::StaticWorldMatchResult;
using kraken::net::StaticWorldPostLoadRecord;
using kraken::net::StaticWorldSourceRecord;
using kraken::net::StaticWorldId;
using kraken::net::kInvalidStaticWorldId;

StaticWorldSourceRecord record(const char* path, const char* prototype)
{
    return {"r1m1/winter", path, prototype};
}

void test_canonical_key_is_root_and_order_independent()
{
    const StaticWorldSourceRecord source =
        record("world/parent/leaf", "crate_prototype");
    const std::string key = kraken::net::static_world_canonical_key(source);
    CHECK(!key.empty());
    CHECK(key == kraken::net::canonical_static_world_key(source));

    std::vector<StaticWorldSourceRecord> first{
        record("world/parent/leaf", "crate_prototype"),
        record("world/other/leaf", "crate_prototype"),
    };
    std::vector<StaticWorldSourceRecord> second{
        first[1], first[0],
    };

    StaticWorldIdentityIndex first_index;
    StaticWorldIdentityIndex second_index;
    CHECK(StaticWorldIdentityIndex::build(first, first_index).ok());
    CHECK(StaticWorldIdentityIndex::build(second, second_index).ok());
    CHECK(first_index.entries().size() == 2);

    const auto first_id = first_index.id_for(first[0]);
    const auto second_id = second_index.id_for(first[0]);
    CHECK(first_id.has_value() && second_id.has_value());
    CHECK(first_id == second_id);
    CHECK(first_id.value_or(kInvalidStaticWorldId) != kInvalidStaticWorldId);
    CHECK((first_id.value_or(kInvalidStaticWorldId) &
           ~kraken::net::kStaticWorldIdPayloadMask) == 0);

    const auto* entry = first_index.find(source);
    CHECK(entry != nullptr);
    CHECK(entry != nullptr &&
          first_index.verify_id(entry->id, entry->canonical_key));
    CHECK(entry != nullptr &&
          !first_index.verify_id(entry->id, entry->canonical_key + "tampered"));

    // The install root is intentionally not a source-record field.  A
    // sidecar can therefore load the same record from any install root and
    // receive the same key and ID.
    const StaticWorldSourceRecord from_other_install = source;
    CHECK(kraken::net::static_world_canonical_key(from_other_install) == key);
    CHECK(kraken::net::static_world_id_hash(key) ==
          kraken::net::static_world_id_hash(
              kraken::net::static_world_canonical_key(from_other_install)));
}

void test_duplicate_paths_and_malformed_paths_are_rejected()
{
    StaticWorldIdentityIndex index;
    std::vector<StaticWorldSourceRecord> duplicate_path{
        record("world/parent/leaf", "crate_a"),
        record("world/parent/leaf", "crate_b"),
    };
    const auto duplicate = StaticWorldIdentityIndex::build(duplicate_path, index);
    CHECK(duplicate.error == StaticWorldIndexError::DuplicateFullPath);
    CHECK(index.empty());

    const std::vector<StaticWorldSourceRecord> malformed{
        record("/world/absolute", "crate"),
        record("world//empty_segment", "crate"),
        record("world/../escape", "crate"),
        record("world\\wrong_separator", "crate"),
    };
    for (const StaticWorldSourceRecord& candidate : malformed) {
        const auto result = StaticWorldIdentityIndex::build(
            std::vector<StaticWorldSourceRecord>{candidate}, index);
        CHECK(!result.ok());
        CHECK(result.error == StaticWorldIndexError::MalformedPath ||
              result.error == StaticWorldIndexError::MalformedMapNamespace);
    }

    const StaticWorldSourceRecord empty_prototype =
        record("world/valid/path", "");
    const auto empty_result = StaticWorldIdentityIndex::build(
        std::vector<StaticWorldSourceRecord>{empty_prototype}, index);
    CHECK(empty_result.error == StaticWorldIndexError::EmptyPrototypeIdentity);
}

StaticWorldId constant_hash(std::string_view)
{
    return 0x1122334455667788ull;
}

void test_full_key_hash_collisions_are_rejected()
{
    StaticWorldIdentityIndex index;
    const std::vector<StaticWorldSourceRecord> records{
        record("world/a/leaf", "prototype_a"),
        record("world/b/leaf", "prototype_b"),
    };
    const auto result = StaticWorldIdentityIndex::build(
        records, index, kraken::net::StaticWorldIndexOptions{&constant_hash});
    CHECK(result.error == StaticWorldIndexError::HashCollision);
    CHECK(index.empty());
}

void test_matching_is_exact_and_dynamic_objects_stay_unassigned()
{
    const std::vector<StaticWorldSourceRecord> source{
        record("world/north/leaf", "crate"),
        record("world/south/leaf", "crate"),
        record("world/door", "door_prototype"),
    };
    StaticWorldIdentityIndex index;
    CHECK(StaticWorldIdentityIndex::build(source, index).ok());

    // Enumeration order is intentionally different from the source order.
    const std::vector<StaticWorldPostLoadRecord> loaded{
        record("world/runtime/new_object", "dynamic_prototype"),
        record("world/south/leaf", "crate"),
        record("world/door", "door_prototype"),
        record("world/north/leaf", "crate"),
        // A renamed static object has no exact source key and remains dynamic.
        record("world/north/renamed_leaf", "crate"),
    };
    const auto matches = kraken::net::match_static_world_records(index, loaded);
    CHECK(matches.ok());
    CHECK(matches.matches.size() == 3);
    CHECK(matches.ids_by_post_load_index.size() == loaded.size());
    CHECK(matches.ids_by_post_load_index[0] == kInvalidStaticWorldId);
    CHECK(matches.ids_by_post_load_index[4] == kInvalidStaticWorldId);
    CHECK(matches.assigned(1));
    CHECK(matches.assigned(2));
    CHECK(matches.assigned(3));
    CHECK(matches.unmatched_post_load_indices.size() == 2);

    const auto north_id = index.id_for(source[0]);
    const auto south_id = index.id_for(source[1]);
    CHECK(north_id.has_value() && south_id.has_value());
    CHECK(matches.ids_by_post_load_index[3] == north_id.value());
    CHECK(matches.ids_by_post_load_index[1] == south_id.value());

    // Duplicate leaf names are distinct because their complete parent paths
    // are part of the canonical key.
    CHECK(north_id != south_id);

    const std::vector<StaticWorldPostLoadRecord> loaded_reordered{
        loaded[3], loaded[1], loaded[2], loaded[0], loaded[4],
    };
    const auto reordered =
        kraken::net::match_static_world_records(index, loaded_reordered);
    CHECK(reordered.ok());
    CHECK(reordered.membership_digest == matches.membership_digest);
}

void test_post_load_duplicate_and_rename_behavior()
{
    StaticWorldIdentityIndex index;
    const StaticWorldSourceRecord original = record("world/chest", "chest");
    CHECK(StaticWorldIdentityIndex::build(
              std::vector<StaticWorldSourceRecord>{original}, index)
              .ok());

    const StaticWorldPostLoadRecord renamed =
        record("world/chest_renamed", "chest");
    const auto rename_result = kraken::net::match_static_world_records(
        index, std::vector<StaticWorldPostLoadRecord>{renamed});
    CHECK(rename_result.ok());
    CHECK(rename_result.matches.empty());
    CHECK(rename_result.ids_by_post_load_index[0] == kInvalidStaticWorldId);

    const auto duplicate_result = kraken::net::match_static_world_records(
        index, std::vector<StaticWorldPostLoadRecord>{original, original});
    CHECK(duplicate_result.error == StaticWorldIndexError::DuplicateFullPath);
}

void test_two_consecutive_membership_digest_gate()
{
    const std::vector<StaticWorldId> first{9, 3, 9, kInvalidStaticWorldId};
    const std::vector<StaticWorldId> same_membership{3, 9};
    const std::vector<StaticWorldId> changed{3, 10};

    const StaticWorldId first_digest =
        kraken::net::static_world_membership_digest(first);
    CHECK(first_digest ==
          kraken::net::static_world_membership_digest(same_membership));

    StaticWorldMembershipStabilityGate gate;
    CHECK(!gate.observe(first));
    CHECK(!gate.stable());
    CHECK(gate.consecutive_digest_count() == 1);
    CHECK(gate.observe(same_membership));
    CHECK(gate.stable());
    CHECK(gate.consecutive_digest_count() == 2);
    CHECK(gate.current_digest() == first_digest);

    CHECK(!gate.observe(changed));
    CHECK(!gate.stable());
    CHECK(gate.consecutive_digest_count() == 1);
    CHECK(gate.observe(changed));
    CHECK(gate.stable());

    gate.reset();
    CHECK(!gate.stable());
    CHECK(gate.consecutive_digest_count() == 0);
}

void test_ambiguous_sample_fails_closed_without_dynamic_reinterpretation()
{
    const StaticWorldSourceRecord original =
        record("world/chest", "chest");
    StaticWorldIdentityIndex index;
    CHECK(StaticWorldIdentityIndex::build(
              std::vector<StaticWorldSourceRecord>{original}, index)
              .ok());

    const std::vector<StaticWorldPostLoadRecord> valid{original};
    const StaticWorldMatchResult first =
        kraken::net::match_static_world_records(index, valid);
    CHECK(first.ok());
    CHECK(first.assigned(0));

    StaticWorldMembershipStabilityGate gate;
    CHECK(!gate.observe(first));
    CHECK(gate.observe(first));
    CHECK(gate.stable());
    const std::vector<StaticWorldId> committed_ids =
        first.ids_by_post_load_index;

    // A duplicate source path is ambiguous.  The invalid result intentionally
    // carries no aligned IDs, so the runtime fail-closed path must retain the
    // committed static mapping instead of converting it to a dynamic ID.
    const std::vector<StaticWorldPostLoadRecord> ambiguous{
        original, original};
    const StaticWorldMatchResult rejected =
        kraken::net::match_static_world_records(index, ambiguous);
    CHECK(!rejected.ok());
    CHECK(rejected.error == StaticWorldIndexError::DuplicateFullPath);
    CHECK(rejected.matches.empty());
    CHECK(rejected.unmatched_post_load_indices.empty());
    CHECK(rejected.ids_by_post_load_index.empty());
    CHECK(!gate.observe(rejected));
    CHECK(!gate.stable());
    CHECK(gate.consecutive_digest_count() == 0);
    CHECK(committed_ids == first.ids_by_post_load_index);
}

} // namespace

int main()
{
    test_canonical_key_is_root_and_order_independent();
    test_duplicate_paths_and_malformed_paths_are_rejected();
    test_full_key_hash_collisions_are_rejected();
    test_matching_is_exact_and_dynamic_objects_stay_unassigned();
    test_post_load_duplicate_and_rename_behavior();
    test_two_consecutive_membership_digest_gate();
    test_ambiguous_sample_fails_closed_without_dynamic_reinterpretation();

    if (failures != 0) {
        std::cerr << failures << " static-world identity test(s) failed\n";
        return 1;
    }

    std::cout << "static-world identity tests passed\n";
    return 0;
}
