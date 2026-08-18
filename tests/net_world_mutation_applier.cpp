#include "net/host_world_registry.hpp"
#include "net/world_mutation_applier.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace kraken::net;

int failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

WorldMutationSource network_source()
{
    return {TransportRole::Server, ReplicationSourceContext::NetworkReplay,
            kInvalidPeer};
}

void test_host_world_registry()
{
    HostWorldRegistry registry;

    CHECK(registry.install_static(10, 100) ==
          HostWorldRegistryResult::Inserted);
    CHECK(registry.install_static(11, 101, 3, 100) ==
          HostWorldRegistryResult::Inserted);
    CHECK(registry.install_static(10, 100) ==
          HostWorldRegistryResult::AlreadyBound);
    CHECK(registry.install_static(10, 102) ==
          HostWorldRegistryResult::Collision);
    CHECK(registry.install_static(12, kHostWorldDynamicFirstId) ==
          HostWorldRegistryResult::InvalidNamespace);
    CHECK(registry.id_for(10).value_or(0) == 100);
    CHECK(registry.id_for(11, 3).value_or(0) == 101);
    CHECK(registry.parent_for(101).value_or(0) == 100);
    CHECK(registry.parent_for_handle(11, 3).value_or(0) == 100);
    CHECK(registry.id_for(11, 2) == std::nullopt);

    const HostWorldDynamicAllocation first = registry.allocate_dynamic(20);
    const HostWorldDynamicAllocation second = registry.allocate_dynamic(21);
    CHECK(first.succeeded() && second.succeeded());
    CHECK(is_host_world_dynamic_id(first.id));
    CHECK(is_host_world_dynamic_id(second.id));
    CHECK(first.id < second.id);
    CHECK(first.id != static_cast<HostObjectId>(20));
    CHECK(registry.bind_dynamic(22, first.id) ==
          HostWorldRegistryResult::NonMonotonicId);

    CHECK(registry.remove(20, 2) ==
          HostWorldRegistryResult::StaleGeneration);
    CHECK(registry.remove(20, first.id, 1) ==
          HostWorldRegistryResult::Removed);
    CHECK(registry.was_retired(first.id));
    CHECK(registry.allocate_dynamic(20, 1).result ==
          HostWorldRegistryResult::StaleGeneration);

    const HostWorldDynamicAllocation reused_handle =
        registry.allocate_dynamic(20, 2);
    CHECK(reused_handle.succeeded());
    CHECK(reused_handle.id > second.id);
    CHECK(registry.remove_id(reused_handle.id, 1) ==
          HostWorldRegistryResult::StaleGeneration);
    CHECK(registry.remove_id(reused_handle.id, 2) ==
          HostWorldRegistryResult::Removed);
    CHECK(registry.bind_dynamic(23, reused_handle.id, 3) ==
          HostWorldRegistryResult::NonMonotonicId);

    CHECK(registry.set_parent(11, 3, 100) ==
          HostWorldRegistryResult::AlreadyBound);
    CHECK(registry.set_parent(11, 3, 999) ==
          HostWorldRegistryResult::InvalidParent);
    CHECK(registry.set_parent(10, 1, 101) ==
          HostWorldRegistryResult::ParentCycle);
    CHECK(registry.remove(11, 3) ==
          HostWorldRegistryResult::InvalidParent);
    CHECK(registry.set_parent(11, 3, kInvalidHostObjectId) ==
          HostWorldRegistryResult::Inserted);
    CHECK(registry.remove(11, 3) ==
          HostWorldRegistryResult::Removed);
    CHECK(registry.was_retired(101));

    registry.clear();
    CHECK(registry.empty());
    CHECK(registry.was_retired(100));
    CHECK(registry.bind_static(30, 100) ==
          HostWorldRegistryResult::RetiredId);
}

struct CallbackCounts {
    int create = 0;
    int despawn = 0;
    int add_child = 0;
    int remove_child = 0;
    int runtime = 0;
    int property = 0;
    int damage = 0;
    int destroyed = 0;
    int fx = 0;
    int begin = 0;
    int commit = 0;
    int rollback = 0;
    bool fail_fx = false;
};

WorldMutationEngineCallbacks make_callbacks(
    CallbackCounts& counts, WorldObserver& observer)
{
    WorldMutationEngineCallbacks callbacks;
    callbacks.create = [&](const ObjectCreatedEvent& event) {
        ++counts.create;
        (void)observer.publish(event);
        return true;
    };
    callbacks.despawn = [&](const ObjectDespawnedEvent& event) {
        ++counts.despawn;
        (void)observer.publish(event);
        return true;
    };
    callbacks.add_child = [&](const ParentChildAddedEvent& event) {
        ++counts.add_child;
        (void)observer.publish(event);
        return true;
    };
    callbacks.remove_child = [&](const ParentChildRemovedEvent& event) {
        ++counts.remove_child;
        (void)observer.publish(event);
        return true;
    };
    callbacks.runtime = [&](const RuntimeChangedEvent& event) {
        ++counts.runtime;
        (void)observer.publish(event);
        return true;
    };
    callbacks.property = [&](const PropertyChangedEvent& event) {
        ++counts.property;
        (void)observer.publish(event);
        return true;
    };
    callbacks.damage = [&](const DamageEvent& event) {
        ++counts.damage;
        (void)observer.publish(event);
        return true;
    };
    callbacks.destroyed = [&](const DestroyedEvent& event) {
        ++counts.destroyed;
        (void)observer.publish(event);
        return true;
    };
    callbacks.fx = [&](const FxEvent& event) {
        ++counts.fx;
        (void)observer.publish(event);
        return !counts.fail_fx;
    };
    callbacks.begin_transaction = [&counts] {
        ++counts.begin;
        return true;
    };
    callbacks.commit_transaction = [&counts] {
        ++counts.commit;
        return true;
    };
    callbacks.rollback_transaction = [&counts] {
        ++counts.rollback;
        return true;
    };
    return callbacks;
}

void test_world_mutation_applier_variants_ordering_and_replay()
{
    std::vector<WorldMutationEvent> observed;
    WorldObserver observer(
        [&observed](const WorldMutationEvent& event) {
            observed.push_back(event);
        });
    CallbackCounts counts;
    WorldMutationApplier applier(make_callbacks(counts, observer), observer);

    CHECK(applier.install_object(1, 7) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{ObjectCreatedEvent{2, 8}}, 1) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{ParentChildAddedEvent{1, 2}}, 2) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{
              RuntimeChangedEvent{2, {Byte{1}, Byte{2}}}}, 3) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{
              PropertyChangedEvent{2, 4, {Byte{3}}, false}}, 4) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{DamageEvent{2, 1, 9, 1.5f}}, 5) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{DestroyedEvent{2, 10}}, 6) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{FxEvent{2, 11, {Byte{4}}}}, 7) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{
              ParentChildRemovedEvent{1, 2}}, 8) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{ObjectDespawnedEvent{2}}, 9) ==
          WorldMutationApplyResult::Applied);

    CHECK(counts.create == 1);
    CHECK(counts.add_child == 1);
    CHECK(counts.runtime == 1);
    CHECK(counts.property == 1);
    CHECK(counts.damage == 1);
    CHECK(counts.destroyed == 1);
    CHECK(counts.fx == 1);
    CHECK(counts.remove_child == 1);
    CHECK(counts.despawn == 1);
    CHECK(counts.begin == 9 && counts.commit == 9);
    CHECK(counts.rollback == 0);
    CHECK(observed.empty());
    CHECK(applier.revision() == 9);
    CHECK(!applier.contains(2));

    CHECK(applier.apply(WorldMutationEvent{ObjectDespawnedEvent{2}}, 9) ==
          WorldMutationApplyResult::Duplicate);
    CHECK(counts.despawn == 1);
    CHECK(applier.apply(WorldMutationEvent{
              ParentChildRemovedEvent{1, 2}}, 8) ==
          WorldMutationApplyResult::Reordered);
    CHECK(applier.apply(WorldMutationEvent{ObjectCreatedEvent{3, 9}}, 11) ==
          WorldMutationApplyResult::Gap);
    CHECK(applier.revision() == 9);

    counts.fail_fx = true;
    CHECK(applier.apply(WorldMutationEvent{FxEvent{1, 12, {Byte{5}}}}, 10) ==
          WorldMutationApplyResult::CallbackFailed);
    CHECK(applier.revision() == 9);
    CHECK(counts.rollback == 1);
    counts.fail_fx = false;
    CHECK(applier.apply(WorldMutationEvent{FxEvent{1, 12, {Byte{5}}}}, 10) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.revision() == 10);

    CHECK(applier.apply(WorldMutationEnvelope{
              1, 11, {TransportRole::Client,
                      ReplicationSourceContext::NetworkReplay, kInvalidPeer},
              WorldMutationEvent{FxEvent{1, 13, {}}}}) ==
          WorldMutationApplyResult::InvalidRole);
    CHECK(applier.apply(WorldMutationEnvelope{
              1, 11, {TransportRole::Server,
                      ReplicationSourceContext::MapLoad, kInvalidPeer},
              WorldMutationEvent{FxEvent{1, 13, {}}}}) ==
          WorldMutationApplyResult::InvalidSource);
    CHECK(applier.apply(WorldMutationEvent{
              ParentChildAddedEvent{1, 99}}, 11) ==
          WorldMutationApplyResult::UnknownObject);
    CHECK(applier.revision() == 10);

    CHECK(observer.publish(ObjectCreatedEvent{77, 1}) ==
          ObservationResult::Emitted);
    CHECK(observed.size() == 1);
}

void test_object_id_reuse_is_rejected()
{
    WorldMutationEngineCallbacks callbacks;
    callbacks.create = [](const ObjectCreatedEvent&) { return true; };
    callbacks.despawn = [](const ObjectDespawnedEvent&) { return true; };
    WorldMutationApplier applier(std::move(callbacks));

    CHECK(applier.apply(WorldMutationEvent{ObjectCreatedEvent{50, 1}}, 1) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{ObjectDespawnedEvent{50}}, 2) ==
          WorldMutationApplyResult::Applied);
    CHECK(applier.apply(WorldMutationEvent{ObjectCreatedEvent{50, 1}}, 3) ==
          WorldMutationApplyResult::ObjectIdCollision);
    CHECK(applier.revision() == 2);
}

} // namespace

int main()
{
    test_host_world_registry();
    test_world_mutation_applier_variants_ordering_and_replay();
    test_object_id_reuse_is_rejected();

    if (failures != 0) {
        std::cerr << failures << " world-mutation applier test(s) failed\n";
        return 1;
    }
    std::cout << "world-mutation applier tests passed\n";
    return 0;
}

