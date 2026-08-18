#include "net/world_observer.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
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

std::vector<Byte> bytes(std::initializer_list<std::uint8_t> values)
{
    std::vector<Byte> result;
    result.reserve(values.size());
    for (const std::uint8_t value : values)
        result.push_back(static_cast<Byte>(value));
    return result;
}

void test_typed_events_sink_and_duplicate_suppression()
{
    std::vector<WorldMutationEvent> received;
    WorldObserver observer(
        [&received](const WorldMutationEvent& event) {
            received.push_back(event);
        });

    observer.begin_observation_pass();
    CHECK(observer.observe(ObjectCreatedEvent{1, 7}) ==
          ObservationResult::Emitted);
    CHECK(observer.observe(ObjectCreatedEvent{1, 7}) ==
          ObservationResult::Duplicate);
    CHECK(observer.observe(ObjectDespawnedEvent{2}) ==
          ObservationResult::Emitted);
    CHECK(observer.observe(ParentChildAddedEvent{1, 2}) ==
          ObservationResult::Emitted);
    CHECK(observer.observe(ParentChildRemovedEvent{1, 2}) ==
          ObservationResult::Emitted);
    CHECK(observer.observe(RuntimeChangedEvent{2, bytes({1, 2})}) ==
          ObservationResult::Emitted);
    CHECK(observer.observe(PropertyChangedEvent{2, 4, bytes({3}), false}) ==
          ObservationResult::Emitted);
    CHECK(observer.observe(DamageEvent{2, 1, 9, 12.5f}) ==
          ObservationResult::Emitted);
    CHECK(observer.observe(DestroyedEvent{2, 3}) ==
          ObservationResult::Emitted);
    CHECK(observer.observe(FxEvent{2, 11, bytes({4})}) ==
          ObservationResult::Emitted);

    CHECK(received.size() == 9);
    CHECK(mutation_kind(received[0]) == WorldMutationKind::ObjectCreated);
    CHECK(mutation_kind(received[1]) == WorldMutationKind::ObjectDespawned);
    CHECK(mutation_kind(received[2]) == WorldMutationKind::ParentChildAdded);
    CHECK(mutation_kind(received[3]) ==
          WorldMutationKind::ParentChildRemoved);
    CHECK(mutation_kind(received[4]) == WorldMutationKind::RuntimeChanged);
    CHECK(mutation_kind(received[5]) == WorldMutationKind::PropertyChanged);
    CHECK(mutation_kind(received[6]) == WorldMutationKind::Damage);
    CHECK(mutation_kind(received[7]) == WorldMutationKind::Destroyed);
    CHECK(mutation_kind(received[8]) == WorldMutationKind::Fx);
    CHECK(std::holds_alternative<DamageEvent>(received[6]));
}

void test_replay_suppression_is_raii_and_nested()
{
    std::size_t received = 0;
    WorldObserver observer([&received](const WorldMutationEvent&) {
        ++received;
    });

    observer.begin_observation_pass();
    {
        ReplayGuard outer = observer.suppress_replay();
        CHECK(observer.replay_suppressed());
        CHECK(observer.observe(RuntimeChangedEvent{5, bytes({1})}) ==
              ObservationResult::Suppressed);
        {
            ReplayGuard inner = observer.suppress_replay();
            CHECK(observer.replay_suppressed());
            CHECK(observer.observe(FxEvent{5, 1, {}}) ==
                  ObservationResult::Suppressed);
        }
        CHECK(observer.replay_suppressed());
    }

    CHECK(!observer.replay_suppressed());
    CHECK(observer.observe(RuntimeChangedEvent{5, bytes({1})}) ==
          ObservationResult::Emitted);
    CHECK(received == 1);
}

ObjectRecord record(const HostObjectId object_id,
                    const HostObjectId parent_id,
                    std::initializer_list<std::uint8_t> runtime,
                    std::initializer_list<PropertySnapshot> properties = {})
{
    ObjectRecord result{};
    result.object_id = object_id;
    result.parent_id = parent_id;
    result.runtime = bytes(runtime);
    result.properties = properties;
    return result;
}

void test_frame_diff_is_host_id_based_and_deterministic()
{
    std::vector<WorldMutationEvent> received;
    WorldObserver observer(
        [&received](const WorldMutationEvent& event) {
            received.push_back(event);
        });

    const std::array<ObjectRecord, 2> first_frame{
        record(2, 1, {2}, {{8, bytes({8})}, {7, bytes({7})}}),
        record(1, 0, {1})};
    const FrameObservationResult first = observer.observe_frame(first_frame);
    CHECK(first.emitted == 7);
    CHECK(received.size() == 7);
    CHECK(std::get<ObjectCreatedEvent>(received[0]).object_id == 1);
    CHECK(std::get<ObjectCreatedEvent>(received[1]).object_id == 2);
    CHECK(std::get<ParentChildAddedEvent>(received[2]).child_id == 2);
    CHECK(std::get<RuntimeChangedEvent>(received[3]).value == bytes({1}));
    CHECK(std::get<RuntimeChangedEvent>(received[4]).value == bytes({2}));
    CHECK(std::get<PropertyChangedEvent>(received[5]).property_id == 7);
    CHECK(std::get<PropertyChangedEvent>(received[6]).property_id == 8);

    received.clear();
    const std::array<ObjectRecord, 2> second_frame{
        record(3, 0, {3}),
        record(2, 3, {4}, {{7, bytes({70})}, {9, bytes({9})}})};
    const FrameObservationResult second = observer.observe_frame(second_frame);
    CHECK(second.emitted == 9);
    CHECK(received.size() == 9);
    CHECK(std::holds_alternative<ObjectDespawnedEvent>(received[0]));
    CHECK(std::get<ObjectDespawnedEvent>(received[0]).object_id == 1);
    CHECK(std::holds_alternative<ObjectCreatedEvent>(received[1]));
    CHECK(std::get<ObjectCreatedEvent>(received[1]).object_id == 3);
    CHECK(std::holds_alternative<ParentChildRemovedEvent>(received[2]));
    CHECK(std::holds_alternative<ParentChildAddedEvent>(received[3]));
    CHECK(std::holds_alternative<RuntimeChangedEvent>(received[4]));
    CHECK(std::get<RuntimeChangedEvent>(received[4]).value == bytes({4}));
    CHECK(std::holds_alternative<PropertyChangedEvent>(received[5]));
    CHECK(std::get<PropertyChangedEvent>(received[5]).property_id == 7);
    CHECK(!std::get<PropertyChangedEvent>(received[5]).removed);
    CHECK(std::holds_alternative<PropertyChangedEvent>(received[6]));
    CHECK(std::get<PropertyChangedEvent>(received[6]).property_id == 8);
    CHECK(std::get<PropertyChangedEvent>(received[6]).removed);
    CHECK(std::holds_alternative<PropertyChangedEvent>(received[7]));
    CHECK(std::get<PropertyChangedEvent>(received[7]).property_id == 9);
    CHECK(std::holds_alternative<RuntimeChangedEvent>(received[8]));
    CHECK(std::get<RuntimeChangedEvent>(received[8]).value == bytes({3}));

    received.clear();
    const FrameObservationResult unchanged =
        observer.observe_frame(second_frame);
    CHECK(unchanged.emitted == 0);
    CHECK(unchanged.duplicates == 0);
    CHECK(received.empty());

    // Invalid and duplicate host IDs are rejected without picking an input
    // order-dependent winner.
    const std::array<ObjectRecord, 2> invalid_frame{
        record(0, 0, {}), record(4, 4, {})};
    const FrameObservationResult invalid = observer.observe_frame(invalid_frame);
    CHECK(invalid.invalid_records == 2);
    CHECK(received.empty());
    const FrameObservationResult after_invalid =
        observer.observe_frame(second_frame);
    CHECK(after_invalid.emitted == 0);
}

void test_new_object_emits_empty_runtime_completion()
{
    std::vector<WorldMutationEvent> received;
    WorldObserver observer(
        [&received](const WorldMutationEvent& event) {
            received.push_back(event);
        });

    const std::array<ObjectRecord, 1> frame{record(42, 0, {})};
    const FrameObservationResult result = observer.observe_frame(frame);
    CHECK(result.emitted == 2);
    CHECK(received.size() == 2);
    CHECK(std::holds_alternative<ObjectCreatedEvent>(received[0]));
    CHECK(std::holds_alternative<RuntimeChangedEvent>(received[1]));
    CHECK(std::get<RuntimeChangedEvent>(received[1]).value.empty());
}

void test_arbitrary_depth_graph_creation_and_links()
{
    std::vector<WorldMutationEvent> received;
    WorldObserver observer([&received](const WorldMutationEvent& event) {
        received.push_back(event);
    });
    const std::array<ObjectRecord, 3> frame{
        record(30, 20, {}), record(10, 0, {}), record(20, 10, {})};
    const FrameObservationResult result = observer.observe_frame(frame);
    CHECK(result.emitted == 8);
    CHECK(received.size() == 8);
    CHECK(std::holds_alternative<ObjectCreatedEvent>(received[0]));
    CHECK(std::holds_alternative<ObjectCreatedEvent>(received[1]));
    CHECK(std::holds_alternative<ObjectCreatedEvent>(received[2]));
    CHECK(std::holds_alternative<ParentChildAddedEvent>(received[3]));
    CHECK(std::holds_alternative<ParentChildAddedEvent>(received[4]));
    CHECK(std::get<ParentChildAddedEvent>(received[3]).parent_id == 10);
    CHECK(std::get<ParentChildAddedEvent>(received[3]).child_id == 20);
    CHECK(std::get<ParentChildAddedEvent>(received[4]).parent_id == 20);
    CHECK(std::get<ParentChildAddedEvent>(received[4]).child_id == 30);
}

void test_post_call_overlap_is_reconciled_without_duplicate()
{
    int parent = 0;
    int child = 0;
    const void* child_parent = nullptr;
    std::vector<WorldMutationEvent> received;
    WorldObserver observer([&received](const WorldMutationEvent& event) {
        received.push_back(event);
    });
    EngineBindingConfig config{};
    config.identity_from_object = [&](const void* object)
        -> std::optional<HostObjectId> {
        return object == &parent ? std::optional<HostObjectId>{10}
             : object == &child ? std::optional<HostObjectId>{20}
                                : std::nullopt;
    };
    config.parent_from_object = [&](const void*) { return child_parent; };
    EngineBinding binding(observer, std::move(config));
    const std::array<ObjectRecord, 2> initial{
        record(10, 0, {}), record(20, 0, {})};
    (void)observer.observe_frame(initial);
    received.clear();

    child_parent = &parent;
    observer.begin_observation_pass();
    CHECK(binding.observe_add_child_succeeded(&parent, &child, true));
    const std::array<ObjectRecord, 2> linked{
        record(10, 0, {}), record(20, 10, {})};
    const FrameObservationResult reconciled = observer.observe_frame(linked);
    CHECK(reconciled.emitted == 0);
    CHECK(reconciled.duplicates == 1);
    CHECK(received.size() == 1);

    child_parent = nullptr;
    CHECK(!binding.observe_add_child_succeeded(&parent, &child, true));
    CHECK(!binding.observe_remove_child_succeeded(&parent, &child, false));
    CHECK(received.size() == 1);
    CHECK(binding.observe_remove_child_succeeded(&parent, &child, true));
}

void test_engine_binding_is_post_success_and_injected()
{
    int chest = 0;
    int child = 0;
    int created = 0;
    std::vector<WorldMutationEvent> received;
    WorldObserver observer(
        [&received](const WorldMutationEvent& event) {
            received.push_back(event);
        });

    EngineBindingConfig config{};
    config.identity_from_object = [&](const void* object)
        -> std::optional<HostObjectId> {
        if (object == &chest)
            return 10;
        if (object == &child)
            return 11;
        if (object == &created)
            return 12;
        return std::nullopt;
    };
    config.identity_from_engine_obj_id =
        [](const std::int32_t id) -> std::optional<HostObjectId> {
        return id == 42 ? std::optional<HostObjectId>{12}
                         : std::nullopt;
    };
    config.type_from_object = [](const void*) { return ObjectTypeId{99}; };

    EngineBinding binding(observer, std::move(config));
    observer.begin_observation_pass();
    CHECK(binding.observe_chest_add_child_succeeded(&chest, &child));
    CHECK(!binding.observe_chest_remove_child_succeeded(
        &chest, &child, false));
    CHECK(binding.observe_chest_remove_child_succeeded(
        &chest, &child, true));
    CHECK(binding.observe_obj_container_create_succeeded(&created));
    CHECK(binding.observe_obj_container_remove_succeeded(42, true));
    CHECK(received.size() == 4);

    CHECK(EngineBinding::addresses().chest_add_child_rva == 0x3eb920u);
    CHECK(EngineBinding::addresses().chest_remove_child_rva == 0x3eb460u);
    CHECK(EngineBinding::addresses().chest_vtable_rva == 0x5adba8u);
    CHECK(EngineBinding::addresses().obj_add_child_rva == 0x28db70u);
    CHECK(EngineBinding::addresses().vehicle_remove_child_rva == 0x1e5f10u);
    CHECK(EngineBinding::addresses().obj_container_create_new_object_body_rva ==
          0x233c60u);
    CHECK(EngineBinding::addresses().obj_container_create_new_object_lua_callsite_rva ==
          0x233d0du);
    CHECK(EngineBinding::addresses().obj_container_create_new_object_rva ==
          0x233c60u);
    CHECK(EngineBinding::addresses().obj_container_add_obj_id_to_remove_rva ==
          0x28e350u);
}

} // namespace

int main()
{
    test_typed_events_sink_and_duplicate_suppression();
    test_replay_suppression_is_raii_and_nested();
    test_frame_diff_is_host_id_based_and_deterministic();
    test_new_object_emits_empty_runtime_completion();
    test_arbitrary_depth_graph_creation_and_links();
    test_post_call_overlap_is_reconciled_without_duplicate();
    test_engine_binding_is_post_success_and_injected();

    if (failures != 0) {
        std::cerr << failures << " world observer test(s) failed\n";
        return 1;
    }

    std::cout << "world observer tests passed\n";
    return 0;
}
