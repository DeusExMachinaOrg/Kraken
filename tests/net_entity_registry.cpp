#include "net/entity_registry.hpp"

#include <cstdint>
#include <iostream>
#include <limits>

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

void test_defaults_and_empty_state()
{
    using namespace kraken::net;

    EntityRegistry registry;
    CHECK(registry.empty());
    CHECK(!registry.full());
    CHECK(registry.size() == 0);
    CHECK(registry.capacity() == kDefaultEntityRegistryCapacity);

    ObjId obj_id = 123;
    NetId net_id = 456;
    CHECK(!registry.lookup_obj_id(1, obj_id));
    CHECK(obj_id == 123);
    CHECK(!registry.lookup_net_id(1, net_id));
    CHECK(net_id == 456);
    CHECK(registry.unbind_net_id(1) ==
          EntityRegistryUnbindResult::NotFound);
    CHECK(registry.unbind_obj_id(1) ==
          EntityRegistryUnbindResult::NotFound);
}

void test_invalid_ids_are_rejected_without_mutation()
{
    using namespace kraken::net;

    EntityRegistry registry(2);
    const ObjId invalid_obj = kInvalidObjId;
    const NetId invalid_net = kInvalidNetId;

    CHECK(registry.bind(invalid_net, 7) == EntityRegistryBindResult::InvalidId);
    CHECK(registry.bind(7, invalid_obj) == EntityRegistryBindResult::InvalidId);
    CHECK(registry.bind(invalid_net, invalid_obj) ==
          EntityRegistryBindResult::InvalidId);
    CHECK(registry.size() == 0);

    ObjId obj_id = 11;
    NetId net_id = 12;
    CHECK(!registry.lookup_obj_id(invalid_net, obj_id));
    CHECK(!registry.lookup_net_id(invalid_obj, net_id));
    CHECK(registry.unbind_net_id(invalid_net) ==
          EntityRegistryUnbindResult::InvalidId);
    CHECK(registry.unbind_obj_id(invalid_obj) ==
          EntityRegistryUnbindResult::InvalidId);
    CHECK(registry.size() == 0);

    // ObjId is opaque: valid negative values remain usable.
    CHECK(registry.bind(1, -1) == EntityRegistryBindResult::Inserted);
    CHECK(registry.bind(2, std::numeric_limits<ObjId>::max()) ==
          EntityRegistryBindResult::Inserted);
}

void test_bidirectional_lookup_and_idempotence()
{
    using namespace kraken::net;

    EntityRegistry registry(4);
    CHECK(registry.bind(100, -1) == EntityRegistryBindResult::Inserted);
    CHECK(registry.bind(200, 42) == EntityRegistryBindResult::Inserted);

    ObjId obj_id = 0;
    NetId net_id = 0;
    CHECK(registry.lookup_obj_id(100, obj_id));
    CHECK(obj_id == -1);
    CHECK(registry.lookup_net_id(-1, net_id));
    CHECK(net_id == 100);
    CHECK(registry.lookup_obj_id(200, obj_id));
    CHECK(obj_id == 42);
    CHECK(registry.lookup_net_id(42, net_id));
    CHECK(net_id == 200);

    CHECK(registry.bind(100, -1) == EntityRegistryBindResult::AlreadyBound);
    CHECK(registry.size() == 2);
}

void test_collisions_never_overwrite()
{
    using namespace kraken::net;

    EntityRegistry registry(4);
    CHECK(registry.bind(10, 100) == EntityRegistryBindResult::Inserted);

    // Existing net ID cannot be redirected.
    CHECK(registry.bind(10, 101) == EntityRegistryBindResult::Collision);
    // Existing object ID cannot be redirected.
    CHECK(registry.bind(11, 100) == EntityRegistryBindResult::Collision);
    CHECK(registry.size() == 1);

    ObjId obj_id = 0;
    NetId net_id = 0;
    CHECK(registry.lookup_obj_id(10, obj_id) && obj_id == 100);
    CHECK(registry.lookup_net_id(100, net_id) && net_id == 10);
    CHECK(!registry.lookup_obj_id(11, obj_id));
    CHECK(!registry.lookup_net_id(101, net_id));

    // The caller must explicitly unbind before reusing either key.
    CHECK(registry.unbind_net_id(10) == EntityRegistryUnbindResult::Removed);
    CHECK(registry.bind(10, 101) == EntityRegistryBindResult::Inserted);
    CHECK(registry.unbind_obj_id(101) == EntityRegistryUnbindResult::Removed);
    CHECK(registry.bind(11, 100) == EntityRegistryBindResult::Inserted);
}

void test_capacity_and_reuse()
{
    using namespace kraken::net;

    EntityRegistry registry(2);
    CHECK(registry.bind(1, 1) == EntityRegistryBindResult::Inserted);
    CHECK(registry.bind(2, 2) == EntityRegistryBindResult::Inserted);
    CHECK(registry.full());
    CHECK(registry.bind(3, 3) == EntityRegistryBindResult::Full);
    CHECK(registry.size() == 2);

    CHECK(registry.unbind_obj_id(1) == EntityRegistryUnbindResult::Removed);
    CHECK(!registry.full());
    CHECK(registry.bind(3, 3) == EntityRegistryBindResult::Inserted);
    CHECK(registry.size() == 2);

    CHECK(registry.unbind_net_id(999) == EntityRegistryUnbindResult::NotFound);
    CHECK(registry.unbind_obj_id(999) == EntityRegistryUnbindResult::NotFound);
    CHECK(registry.size() == 2);
}

void test_removal_is_deterministic_and_clear_is_complete()
{
    using namespace kraken::net;

    EntityRegistry registry(8);
    CHECK(registry.bind(30, 300) == EntityRegistryBindResult::Inserted);
    CHECK(registry.bind(10, 100) == EntityRegistryBindResult::Inserted);
    CHECK(registry.bind(20, 200) == EntityRegistryBindResult::Inserted);

    CHECK(registry.unbind_net_id(10) == EntityRegistryUnbindResult::Removed);
    CHECK(registry.bind(40, 400) == EntityRegistryBindResult::Inserted);
    ObjId obj_id = 0;
    CHECK(registry.lookup_obj_id(30, obj_id) && obj_id == 300);
    CHECK(registry.lookup_obj_id(20, obj_id) && obj_id == 200);
    CHECK(registry.lookup_obj_id(40, obj_id) && obj_id == 400);

    CHECK(registry.unbind_obj_id(300) == EntityRegistryUnbindResult::Removed);
    CHECK(registry.unbind_net_id(30) == EntityRegistryUnbindResult::NotFound);
    CHECK(registry.size() == 2);

    registry.clear();
    CHECK(registry.empty());
    CHECK(!registry.full());
    CHECK(registry.capacity() == 8);
    CHECK(registry.unbind_net_id(20) == EntityRegistryUnbindResult::NotFound);
}

void test_zero_capacity()
{
    using namespace kraken::net;

    EntityRegistry registry(0);
    CHECK(registry.empty());
    CHECK(registry.full());
    CHECK(registry.bind(1, 1) == EntityRegistryBindResult::Full);
    CHECK(registry.size() == 0);
}

} // namespace

int main()
{
    test_defaults_and_empty_state();
    test_invalid_ids_are_rejected_without_mutation();
    test_bidirectional_lookup_and_idempotence();
    test_collisions_never_overwrite();
    test_capacity_and_reuse();
    test_removal_is_deterministic_and_clear_is_complete();
    test_zero_capacity();

    if (failures != 0) {
        std::cerr << failures << " registry test(s) failed\n";
        return 1;
    }

    std::cout << "entity registry tests passed\n";
    return 0;
}
