#define LOGGER "multiplayer"

#include "net/runtime.hpp"

#include "config.hpp"
#include "ext/runtime.hpp"
#include "ext/logger.hpp"
#include "net/entity_registry.hpp"
#include "net/input_command.hpp"
#include "net/loot_transaction.hpp"
#include "net/session.hpp"
#include "net/snapshot_interpolation.hpp"
#include "net/transport.hpp"
#include "net/vehicle_snapshot.hpp"
#include "net/weapon_command.hpp"
#include "routines.hpp"

#include "hta/CVector.hpp"
#include "hta/Quaternion.hpp"
#include "hta/ai/Player.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/Chest.hpp"
#include "hta/ai/GeomRepository.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/ScriptServer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <cstdio>
#include <string>
#include <vector>

namespace kraken::net::runtime {
bool RequestLocalLoot(LootId loot_id, LootTransactionId transaction_id,
                      std::uint32_t amount);
LootId SpawnHostLoot(std::int32_t chest_prototype_id, std::int32_t resource_id,
                     std::uint32_t amount);
namespace {

constexpr uintptr_t kServerUpdateCallSite = 0x005C809D;
constexpr uintptr_t kServerUpdateAddress = 0x005F4090;

using Clock = std::chrono::steady_clock;
using ServerUpdateFn = void(__fastcall*)(void*, void*, float);

constexpr std::uint64_t kInterpolationDelayMs = 100;

struct RemoteEntity {
    NetId entity_id = kInvalidNetId;
    SnapshotInterpolationBuffer snapshots;
    std::uint32_t last_sequence = 0;
    bool has_sequence = false;
    WeaponCommand weapon{};
    bool has_weapon = false;
};

struct PeerController {
    PeerId peer = kInvalidPeer;
    NetId entity_id = kInvalidNetId;
    InputCommand input{};
    bool has_input = false;
    std::uint32_t last_sequence = 0;
    ObjId vehicle_obj_id = kInvalidObjId;
    WeaponCommand weapon{};
    bool has_weapon = false;
    std::uint32_t last_weapon_sequence = 0;
};

struct LootRecord {
    LootId loot_id = 0;
    ObjId chest_obj_id = kInvalidObjId;
    std::int32_t resource_id = -1;
    std::uint32_t remaining_amount = 0;
};

struct LootReceipt {
    PeerId peer = kInvalidPeer;
    LootTransactionId transaction_id = 0;
    LootResult result{};
};

struct RuntimeState {
    EnetTransport transport;
    std::unique_ptr<Session> session;
    std::vector<PeerId> peers;
    Clock::time_point next_ping{};
    Clock::time_point next_snapshot{};
    std::uint32_t next_snapshot_sequence = 1;
    std::uint32_t server_tick = 0;
    EntityRegistry entities;
    std::vector<RemoteEntity> remote_entities;
    std::vector<PeerController> controllers;
    SnapshotInterpolationBuffer local_correction;
    NetId local_entity_id = kInvalidNetId;
    Clock::time_point next_input{};
    std::uint32_t next_input_sequence = 1;
    std::uint32_t next_weapon_sequence = 1;
    LootId next_loot_id = 1;
    std::vector<LootRecord> loot_records;
    std::vector<LootReceipt> loot_receipts;
    ObjId host_vehicle_obj_id = kInvalidObjId;
    bool is_host = false;
    bool hook_installed = false;
};

RuntimeState g_state;
ServerUpdateFn g_server_update =
    reinterpret_cast<ServerUpdateFn>(kServerUpdateAddress);

int __fastcall lua_submit_local_weapon_command(hta::m3d::sArgStack& args)
{
    bool accepted = false;
    if (args.m_numInArgs == 2 &&
        args.m_InArgs[0].GetType() == hta::m3d::sArg::ARGTYPE_INT &&
        args.m_InArgs[1].GetType() == hta::m3d::sArg::ARGTYPE_BOOL)
        accepted = SubmitLocalWeaponCommand(args.m_InArgs[0].GetI(),
                                            args.m_InArgs[1].GetB());
    if (hta::m3d::sArg* const output = args.newOut())
        output->SetB(accepted);
    return 0;
}

int __fastcall lua_request_loot(hta::m3d::sArgStack& args)
{
    bool accepted = false;
    if (args.m_numInArgs == 3 &&
        args.m_InArgs[0].GetType() == hta::m3d::sArg::ARGTYPE_INT &&
        args.m_InArgs[1].GetType() == hta::m3d::sArg::ARGTYPE_INT &&
        args.m_InArgs[2].GetType() == hta::m3d::sArg::ARGTYPE_INT)
        accepted = RequestLocalLoot(static_cast<LootId>(args.m_InArgs[0].GetI()),
                                    static_cast<LootTransactionId>(args.m_InArgs[1].GetI()),
                                    static_cast<std::uint32_t>(args.m_InArgs[2].GetI()));
    if (hta::m3d::sArg* const output = args.newOut()) output->SetB(accepted);
    return 0;
}

int __fastcall lua_spawn_host_loot(hta::m3d::sArgStack& args)
{
    LootId loot_id = 0;
    if (args.m_numInArgs == 3 &&
        args.m_InArgs[0].GetType() == hta::m3d::sArg::ARGTYPE_INT &&
        args.m_InArgs[1].GetType() == hta::m3d::sArg::ARGTYPE_INT &&
        args.m_InArgs[2].GetType() == hta::m3d::sArg::ARGTYPE_INT)
        loot_id = SpawnHostLoot(args.m_InArgs[0].GetI(), args.m_InArgs[1].GetI(),
                                static_cast<std::uint32_t>(args.m_InArgs[2].GetI()));
    if (hta::m3d::sArg* const output = args.newOut()) {
        output->m_type = hta::m3d::sArg::ARGTYPE_INT;
        output->m_i = static_cast<std::int32_t>(loot_id);
    }
    return 0;
}

void register_lua_api()
{
    hta::m3d::Kernel* const kernel = hta::m3d::Kernel::Instance();
    hta::m3d::ScriptServer* const script_server =
        kernel ? kernel->m_scriptServer : nullptr;
    if (script_server == nullptr)
        return;
    const hta::m3d::eScriptError error = script_server->registerGlobalFunction(
        &lua_submit_local_weapon_command, "MP_SubmitWeaponCommand", "bool",
        "int gunId, bool trigger", "Submit weapon intent to the session host");
    LOG_INFO("Lua API MP_SubmitWeaponCommand registered code=%u",
             static_cast<unsigned>(error));
    const hta::m3d::eScriptError request_error = script_server->registerGlobalFunction(&lua_request_loot,
        "MP_RequestLoot", "bool", "int lootId, int transactionId, int amount",
        "Request an idempotent loot pickup from the session host");
    const hta::m3d::eScriptError spawn_error = script_server->registerGlobalFunction(&lua_spawn_host_loot,
        "MP_SpawnHostLoot", "int", "int chestPrototypeId, int resourceId, int amount",
        "Host-only: spawn a chest-backed loot record");
    LOG_INFO("Lua loot API registered request=%u spawn=%u",
             static_cast<unsigned>(request_error), static_cast<unsigned>(spawn_error));
}

struct EffectiveConfig {
    bool enabled = false;
    bool host = true;
    std::string address = "127.0.0.1";
    std::uint16_t port = kDefaultPort;
    std::uint32_t max_peers = 16;
};

std::optional<std::string> environment(const char* name)
{
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return std::nullopt;
    std::string result(value, length > 0 ? length - 1 : 0);
    std::free(value);
    return result;
}

std::uint32_t environment_uint(const char* name, std::uint32_t fallback,
                               std::uint32_t minimum,
                               std::uint32_t maximum)
{
    const auto value = environment(name);
    if (!value || value->empty())
        return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value->c_str(), &end, 10);
    if (end == value->c_str() || *end != '\0')
        return fallback;
    return (std::clamp)(static_cast<std::uint32_t>(parsed), minimum, maximum);
}

EffectiveConfig effective_config(const Config& config)
{
    EffectiveConfig result;
    result.enabled = environment_uint("KRAKEN_MP_ENABLED",
                                      config.multiplayer_enabled.value, 0, 1) != 0;
    result.host = environment_uint("KRAKEN_MP_HOST",
                                   config.multiplayer_host.value, 0, 1) != 0;
    result.address = environment("KRAKEN_MP_ADDRESS")
                         .value_or(config.multiplayer_address.value);
    result.port = static_cast<std::uint16_t>(environment_uint(
        "KRAKEN_MP_PORT", config.multiplayer_port.value, 1024, 65535));
    result.max_peers = environment_uint("KRAKEN_MP_MAX_PEERS",
                                        config.multiplayer_max_peers.value, 2, 16);
    return result;
}

SnapshotTimestampMs now_ms()
{
    return static_cast<SnapshotTimestampMs>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
}

bool sequence_is_newer(std::uint32_t candidate, std::uint32_t current)
{
    return static_cast<std::int32_t>(candidate - current) > 0;
}

RemoteEntity* find_or_add_remote(NetId entity_id)
{
    const auto found = std::find_if(
        g_state.remote_entities.begin(), g_state.remote_entities.end(),
        [entity_id](const RemoteEntity& remote) {
            return remote.entity_id == entity_id;
        });
    if (found != g_state.remote_entities.end())
        return &*found;
    if (g_state.remote_entities.size() >= 16)
        return nullptr;

    g_state.remote_entities.push_back(RemoteEntity{entity_id});
    return &g_state.remote_entities.back();
}

PeerController* find_controller(PeerId peer)
{
    const auto found = std::find_if(g_state.controllers.begin(),
                                    g_state.controllers.end(),
                                    [peer](const PeerController& c) { return c.peer == peer; });
    return found == g_state.controllers.end() ? nullptr : &*found;
}

hta::ai::Vehicle* find_vehicle(NetId entity_id);

LootRecord* find_loot(LootId loot_id)
{
    const auto found = std::find_if(g_state.loot_records.begin(),
                                    g_state.loot_records.end(),
        [loot_id](const LootRecord& record) { return record.loot_id == loot_id; });
    return found == g_state.loot_records.end() ? nullptr : &*found;
}

void send_loot_result(PeerId peer, const LootResult& result)
{
    std::array<Byte, kLootResultWireSize> payload{};
    if (encode_loot_result(result, payload) == LootCodecError::None)
        (void)g_state.session->send(peer, MessageType::LootResult,
                                    Channel::Reliable, payload);
}

void remember_loot_receipt(PeerId peer, const LootResult& result)
{
    if (g_state.loot_receipts.size() >= 256)
        g_state.loot_receipts.erase(g_state.loot_receipts.begin());
    g_state.loot_receipts.push_back({peer, result.transaction_id, result});
}

void send_entity_assignment(PeerId peer, NetId entity_id)
{
    std::array<Byte, 4> payload{};
    for (int index = 0; index != 4; ++index)
        payload[index] = static_cast<Byte>(entity_id >> (8 * index));
    (void)g_state.session->send(peer, MessageType::EntityAssign,
                                Channel::Reliable, payload);
}

void receive_entity_assignment(const SessionEvent& event)
{
    if (g_state.is_host || event.payload.size() != 4)
        return;
    NetId entity = 0;
    for (int index = 0; index != 4; ++index)
        entity |= static_cast<NetId>(static_cast<std::uint8_t>(event.payload[index])) << (8 * index);
    if (entity == kInvalidNetId)
        return;
    g_state.local_entity_id = entity;
    LOG_INFO("local entity assigned id=%u", entity);
}

void receive_input(const SessionEvent& event)
{
    if (!g_state.is_host)
        return;
    PeerController* const controller = find_controller(event.peer);
    if (controller == nullptr)
        return;
    InputCommand input{};
    const InputCommandCodecError decoded = decode_input_command(event.payload, input);
    if (!input_command_codec_succeeded(decoded) || input.entity_id != controller->entity_id) {
        LOG_ERROR("drop input peer=%u code=%u", event.peer, static_cast<unsigned>(decoded));
        return;
    }
    if (controller->has_input && !sequence_is_newer(input.sequence, controller->last_sequence))
        return;
    controller->input = input;
    controller->last_sequence = input.sequence;
    controller->has_input = true;
}

void relay_weapon_command(const WeaponCommand& command)
{
    std::array<Byte, kWeaponCommandWireSize> payload{};
    if (encode_weapon_command(command, payload) != WeaponCommandCodecError::None)
        return;
    for (const PeerId peer : g_state.peers)
        (void)g_state.session->send(peer, MessageType::WeaponCommand,
                                    Channel::Reliable, payload);
}

void receive_weapon_command(const SessionEvent& event)
{
    if (!g_state.is_host) {
        WeaponCommand command{};
        const WeaponCommandCodecError decoded =
            decode_weapon_command(event.payload, command);
        if (!weapon_command_codec_succeeded(decoded)) {
            LOG_ERROR("peer=%u bad weapon command code=%u", event.peer,
                      static_cast<unsigned>(decoded));
            return;
        }
        RemoteEntity* const remote = find_or_add_remote(command.entity_id);
        if (remote == nullptr) {
            LOG_ERROR("too many remote entities; drop weapon entity=%u",
                      command.entity_id);
            return;
        }
        if (remote->has_weapon &&
            !sequence_is_newer(command.sequence, remote->weapon.sequence))
            return;
        remote->weapon = command;
        remote->has_weapon = true;
        // Deliberately no Gun::Fire call here: this packet is an event for a
        // future visual/modding layer, never a client-side combat simulation.
        return;
    }
    PeerController* const controller = find_controller(event.peer);
    if (controller == nullptr)
        return;
    WeaponCommand command{};
    const WeaponCommandCodecError decoded =
        decode_weapon_command(event.payload, command);
    if (!weapon_command_codec_succeeded(decoded) ||
        command.entity_id != controller->entity_id) {
        LOG_ERROR("drop weapon peer=%u code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    if (controller->has_weapon &&
        !sequence_is_newer(command.sequence, controller->last_weapon_sequence))
        return;
    controller->weapon = command;
    controller->last_weapon_sequence = command.sequence;
    controller->has_weapon = true;
    // Clients use this event only for presentation.  The relay intentionally
    // contains no projectile state and cannot cause client-side damage.
    relay_weapon_command(command);
}

void receive_loot_request(const SessionEvent& event)
{
    if (!g_state.is_host)
        return;
    LootRequest request{};
    const LootCodecError decoded = decode_loot_request(event.payload, request);
    PeerController* const controller = find_controller(event.peer);
    if (!loot_codec_succeeded(decoded) || controller == nullptr ||
        request.entity_id != controller->entity_id) {
        LOG_ERROR("drop loot request peer=%u code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }
    const auto prior = std::find_if(g_state.loot_receipts.begin(),
                                    g_state.loot_receipts.end(),
        [&event, &request](const LootReceipt& receipt) {
            return receipt.peer == event.peer &&
                   receipt.transaction_id == request.transaction_id;
        });
    if (prior != g_state.loot_receipts.end()) {
        send_loot_result(event.peer, prior->result);
        return;
    }
    LootResult result{};
    result.transaction_id = request.transaction_id;
    result.loot_id = request.loot_id;
    LootRecord* const loot = find_loot(request.loot_id);
    if (loot == nullptr) result.code = LootResultCode::NotFound;
    else if (loot->remaining_amount == 0) { result.resource_id=loot->resource_id; result.code=LootResultCode::Exhausted; }
    else {
        hta::ai::Vehicle* const vehicle = find_vehicle(controller->entity_id);
        hta::ai::CServer* const server = hta::ai::CServer::Instance();
        hta::ai::Chest* const chest = server && server->m_pObjects
            ? reinterpret_cast<hta::ai::Chest*>(server->m_pObjects->GetEntityByObjId(loot->chest_obj_id))
            : nullptr;
        if (vehicle == nullptr || chest == nullptr || vehicle->m_repository == nullptr)
            result.code = LootResultCode::NotFound;
        else {
            const hta::CVector a = vehicle->GetPosition();
            const hta::CVector b = chest->GetPosition();
            const float dx=a.x-b.x, dy=a.y-b.y, dz=a.z-b.z;
            result.resource_id = loot->resource_id;
            result.remaining_amount = loot->remaining_amount;
            if (dx*dx + dy*dy + dz*dz > 144.0f) result.code = LootResultCode::TooFar;
            else if (!vehicle->m_repository->CanPlaceItems(loot->resource_id, static_cast<int32_t>(request.amount))) result.code = LootResultCode::InventoryFull;
            else {
                const std::uint32_t granted = (std::min)(request.amount, loot->remaining_amount);
                if (!vehicle->m_repository->AddItems(loot->resource_id, static_cast<int32_t>(granted))) result.code = LootResultCode::InventoryFull;
                else {
                    (void)chest->GetRepository()->GiveUpThingByResourceId(loot->resource_id, static_cast<int32_t>(granted));
                    loot->remaining_amount -= granted;
                    result.granted_amount = granted;
                    result.remaining_amount = loot->remaining_amount;
                    result.code = LootResultCode::Granted;
                }
            }
        }
    }
    remember_loot_receipt(event.peer, result);
    send_loot_result(event.peer, result);
}

void receive_loot_result(const SessionEvent& event)
{
    if (g_state.is_host) return;
    LootResult result{};
    if (decode_loot_result(event.payload, result) != LootCodecError::None) return;
    LOG_INFO("loot result txn=%u loot=%u code=%u granted=%u remaining=%u",
             result.transaction_id, result.loot_id, static_cast<unsigned>(result.code),
             result.granted_amount, result.remaining_amount);
}

void receive_remote_snapshot(const SessionEvent& event)
{
    if (g_state.is_host)
        return;

    VehicleSnapshot snapshot{};
    const VehicleSnapshotCodecError decoded =
        decode_vehicle_snapshot(event.payload, snapshot);
    if (!vehicle_snapshot_codec_succeeded(decoded)) {
        LOG_ERROR("peer=%u bad vehicle snapshot code=%u", event.peer,
                  static_cast<unsigned>(decoded));
        return;
    }

    if (snapshot.entity_id == g_state.local_entity_id) {
        (void)g_state.local_correction.push(now_ms(), snapshot);
        return;
    }

    RemoteEntity* const remote = find_or_add_remote(snapshot.entity_id);
    if (remote == nullptr) {
        LOG_ERROR("too many remote entities; drop entity=%u", snapshot.entity_id);
        return;
    }
    if (remote->has_sequence &&
        !sequence_is_newer(snapshot.sequence, remote->last_sequence))
        return;

    const SnapshotInterpolationStatus pushed =
        remote->snapshots.push(now_ms(), snapshot);
    if (!snapshot_interpolation_succeeded(pushed)) {
        LOG_ERROR("drop snapshot entity=%u interpolation code=%u",
                  snapshot.entity_id, static_cast<unsigned>(pushed));
        return;
    }
    remote->last_sequence = snapshot.sequence;
    remote->has_sequence = true;
}

void handle_event(SessionEvent&& event)
{
    switch (event.type) {
    case SessionEventType::PeerConnected:
        if (std::find(g_state.peers.begin(), g_state.peers.end(), event.peer) ==
            g_state.peers.end())
            g_state.peers.push_back(event.peer);
        LOG_INFO("peer=%u handshake complete", event.peer);
        if (g_state.is_host) {
            const NetId entity = event.peer + 1;
            g_state.controllers.push_back(PeerController{event.peer, entity});
            send_entity_assignment(event.peer, entity);
        }
        (void)g_state.session->ping(event.peer);
        break;

    case SessionEventType::PeerDisconnected:
        std::erase(g_state.peers, event.peer);
        LOG_INFO("peer=%u disconnected", event.peer);
        std::erase_if(g_state.controllers, [peer = event.peer](const PeerController& c) { return c.peer == peer; });
        break;

    case SessionEventType::RoundTripTime:
        LOG_INFO("peer=%u rtt=%u ms", event.peer, event.round_trip_time_ms);
        break;

    case SessionEventType::Message:
        if (event.message_type == MessageType::Snapshot)
            receive_remote_snapshot(event);
        else if (event.message_type == MessageType::EntityAssign)
            receive_entity_assignment(event);
        else if (event.message_type == MessageType::Input)
            receive_input(event);
        else if (event.message_type == MessageType::WeaponCommand)
            receive_weapon_command(event);
        else if (event.message_type == MessageType::LootRequest)
            receive_loot_request(event);
        else if (event.message_type == MessageType::LootResult)
            receive_loot_result(event);
        LOG_DEBUG("peer=%u message=%u bytes=%u channel=%u", event.peer,
                  static_cast<unsigned>(event.message_type),
                  static_cast<unsigned>(event.payload.size()),
                  static_cast<unsigned>(event.channel));
        break;

    case SessionEventType::ProtocolError:
        LOG_ERROR("peer=%u protocol error=%u", event.peer,
                  static_cast<unsigned>(event.protocol_error));
        break;
    }
}

void send_client_input()
{
    if (g_state.is_host || g_state.local_entity_id == kInvalidNetId ||
        g_state.peers.empty())
        return;
    const Clock::time_point now = Clock::now();
    if (now < g_state.next_input)
        return;
    g_state.next_input = now + std::chrono::milliseconds(50);
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr)
        return;
    InputCommand input{};
    input.entity_id = g_state.local_entity_id;
    input.sequence = g_state.next_input_sequence++;
    input.client_tick = g_state.server_tick;
    input.throttle = vehicle->m_throttle;
    input.steer = vehicle->m_steerRadians;
    input.brake = vehicle->m_brake;
    input.handbrake = vehicle->m_bHandBrake;
    std::array<Byte, kInputCommandWireSize> payload{};
    if (encode_input_command(input, payload) == InputCommandCodecError::None)
        (void)g_state.session->send(g_state.peers.front(), MessageType::Input,
                                    Channel::Unreliable, payload);
}

hta::ai::Vehicle* find_vehicle(NetId entity_id)
{
    ObjId object_id = kInvalidObjId;
    if (!g_state.entities.lookup_obj_id(entity_id, object_id))
        return nullptr;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::Obj* const object = server && server->m_pObjects
        ? server->m_pObjects->GetEntityByObjId(object_id) : nullptr;
    return object ? reinterpret_cast<hta::ai::Vehicle*>(object) : nullptr;
}

hta::ai::Vehicle* ensure_host_vehicle(PeerController& controller)
{
    if (hta::ai::Vehicle* const existing = find_vehicle(controller.entity_id))
        return existing;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const local = player ? player->GetVehicle() : nullptr;
    if (!server || !server->m_pObjects || !local)
        return nullptr;
    char name[48]{};
    std::snprintf(name, sizeof(name), "kraken_player_%u", controller.entity_id);
    const ObjId object_id = server->m_pObjects->CreateNewObject(
        local->GetPrototypeId(), name, -1, -1);
    if (object_id < 0)
        return nullptr;
    hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(object_id);
    if (!object || g_state.entities.bind(controller.entity_id, object_id) !=
                       EntityRegistryBindResult::Inserted)
        return nullptr;
    hta::ai::Vehicle* const vehicle = reinterpret_cast<hta::ai::Vehicle*>(object);
    hta::CVector position = local->GetPosition();
    position.x += 8.0f * static_cast<float>(controller.entity_id);
    vehicle->SetPositionSelf(position);
    vehicle->SetRotationSelf(local->GetRotation());
    vehicle->SetLinearVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
    vehicle->SetAngularVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
    controller.vehicle_obj_id = object_id;
    LOG_INFO("host player vehicle spawned peer=%u entity=%u objId=%d",
             controller.peer, controller.entity_id, object_id);
    return vehicle;
}

void apply_host_inputs()
{
    if (!g_state.is_host)
        return;
    for (PeerController& controller : g_state.controllers) {
        if (!controller.has_input)
            continue;
        hta::ai::Vehicle* const vehicle = ensure_host_vehicle(controller);
        if (!vehicle)
            continue;
        vehicle->SetThrottle(controller.input.throttle, false);
        vehicle->m_steerRadians = controller.input.steer;
        vehicle->SetBrake(controller.input.brake);
        vehicle->m_bHandBrake = controller.input.handbrake;
    }
}

void apply_host_weapons()
{
    if (!g_state.is_host)
        return;
    for (PeerController& controller : g_state.controllers) {
        if (!controller.has_weapon)
            continue;
        hta::ai::Vehicle* const vehicle = ensure_host_vehicle(controller);
        if (vehicle == nullptr)
            continue;
        const WeaponCommand command = controller.weapon;
        controller.has_weapon = false;
        // This is the original Vehicle -> Gun/CompoundGun route.  Its shells,
        // collision callbacks and InflictDamage are therefore evaluated only
        // in the host's ODE simulation.
        const bool applied = vehicle->FireFromWeaponByGunId(
            command.gun_id, command.trigger_held);
        LOG_DEBUG("weapon host entity=%u gun=%d trigger=%u applied=%u",
                  command.entity_id, command.gun_id,
                  command.trigger_held ? 1u : 0u, applied ? 1u : 0u);
    }
}

hta::CVector to_engine_vector(const VehicleVector3& value)
{
    return {value.x, value.y, value.z};
}

hta::Quaternion to_engine_quaternion(const VehicleQuaternion& value)
{
    return {value.x, value.y, value.z, value.w};
}

hta::ai::Vehicle* ensure_remote_vehicle(RemoteEntity& remote,
                                        const VehicleSnapshot& snapshot)
{
    ObjId object_id = kInvalidObjId;
    if (g_state.entities.lookup_obj_id(remote.entity_id, object_id)) {
        hta::ai::CServer* const server = hta::ai::CServer::Instance();
        hta::ai::Obj* const object = server && server->m_pObjects
            ? server->m_pObjects->GetEntityByObjId(object_id) : nullptr;
        if (object != nullptr)
            return reinterpret_cast<hta::ai::Vehicle*>(object);
        (void)g_state.entities.unbind_net_id(remote.entity_id);
    }

    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const local_vehicle = player ? player->GetVehicle() : nullptr;
    if (server == nullptr || server->m_pObjects == nullptr ||
        local_vehicle == nullptr)
        return nullptr;

    char name[48]{};
    std::snprintf(name, sizeof(name), "kraken_net_%u", snapshot.entity_id);
    const ObjId created_id = server->m_pObjects->CreateNewObject(
        local_vehicle->GetPrototypeId(), name, -1, -1);
    if (created_id < 0) {
        LOG_ERROR("cannot spawn ghost entity=%u prototype=%d", snapshot.entity_id,
                  local_vehicle->GetPrototypeId());
        return nullptr;
    }
    hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(created_id);
    if (object == nullptr) {
        LOG_ERROR("spawned ghost entity=%u missing objId=%d", snapshot.entity_id,
                  created_id);
        return nullptr;
    }
    const EntityRegistryBindResult bound =
        g_state.entities.bind(remote.entity_id, created_id);
    if (bound != EntityRegistryBindResult::Inserted &&
        bound != EntityRegistryBindResult::AlreadyBound) {
        LOG_ERROR("cannot bind ghost entity=%u objId=%d code=%u", snapshot.entity_id,
                  created_id, static_cast<unsigned>(bound));
        return nullptr;
    }
    LOG_INFO("ghost spawned entity=%u objId=%d prototype=%d", snapshot.entity_id,
             created_id, local_vehicle->GetPrototypeId());
    return reinterpret_cast<hta::ai::Vehicle*>(object);
}

void apply_remote_snapshots()
{
    if (g_state.is_host || g_state.remote_entities.empty())
        return;

    const SnapshotTimestampMs current = now_ms();
    const SnapshotTimestampMs target = current > kInterpolationDelayMs
        ? current - kInterpolationDelayMs : 0;
    for (RemoteEntity& remote : g_state.remote_entities) {
        VehicleSnapshot sampled{};
        if (!snapshot_interpolation_succeeded(
                remote.snapshots.sample(target, sampled)))
            continue;
        hta::ai::Vehicle* const ghost = ensure_remote_vehicle(remote, sampled);
        if (ghost == nullptr)
            continue;

        ghost->SetPositionSelf(to_engine_vector(sampled.position));
        ghost->SetRotationSelf(to_engine_quaternion(sampled.rotation));
        ghost->SetLinearVelocity(to_engine_vector(sampled.linear_velocity));
        ghost->SetAngularVelocity(to_engine_vector(sampled.angular_velocity));
    }
}

void apply_local_correction()
{
    if (g_state.is_host || g_state.local_entity_id == kInvalidNetId)
        return;
    VehicleSnapshot authoritative{};
    const SnapshotTimestampMs now = now_ms();
    const SnapshotTimestampMs target = now > kInterpolationDelayMs ? now - kInterpolationDelayMs : 0;
    if (!snapshot_interpolation_succeeded(g_state.local_correction.sample(target, authoritative)))
        return;
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (!vehicle)
        return;
    constexpr float alpha = 0.15f;
    const hta::CVector current = vehicle->GetPosition();
    const hta::CVector target_pos = to_engine_vector(authoritative.position);
    vehicle->SetPositionSelf(hta::CVector(current.x + (target_pos.x-current.x)*alpha,
                                           current.y + (target_pos.y-current.y)*alpha,
                                           current.z + (target_pos.z-current.z)*alpha));
    const hta::CVector velocity = vehicle->GetLinearVelocity();
    const hta::CVector target_velocity = to_engine_vector(authoritative.linear_velocity);
    vehicle->SetLinearVelocity(hta::CVector(velocity.x + (target_velocity.x-velocity.x)*alpha,
                                             velocity.y + (target_velocity.y-velocity.y)*alpha,
                                             velocity.z + (target_velocity.z-velocity.z)*alpha));
}

void pump()
{
    if (!g_state.session)
        return;

    const TransportResult result = g_state.session->pump();
    if (!result && result.code != TransportResultCode::WouldBlock)
        LOG_ERROR("network pump failed code=%u", static_cast<unsigned>(result.code));

    std::array<SessionEvent, 64> events{};
    for (;;) {
        const std::size_t count = g_state.session->drain_events(events);
        if (count == 0)
            break;
        for (std::size_t index = 0; index < count; ++index)
            handle_event(std::move(events[index]));
    }

    const Clock::time_point now = Clock::now();
    if (now < g_state.next_ping)
        return;
    g_state.next_ping = now + std::chrono::seconds(1);
    for (const PeerId peer : g_state.peers)
        (void)g_state.session->ping(peer);
}

VehicleVector3 to_snapshot_vector(const hta::CVector& value)
{
    return {value.x, value.y, value.z};
}

VehicleQuaternion to_snapshot_quaternion(const hta::Quaternion& value)
{
    return {value.x, value.y, value.z, value.w};
}

void capture_and_broadcast_host_snapshot()
{
    if (!g_state.is_host || g_state.peers.empty())
        return;

    const Clock::time_point now = Clock::now();
    if (now < g_state.next_snapshot)
        return;
    g_state.next_snapshot = now + std::chrono::milliseconds(50);

    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (vehicle == nullptr)
        return;

    const ObjId vehicle_obj_id = vehicle->GetId();
    NetId entity_id = kInvalidNetId;
    if (!g_state.entities.lookup_net_id(vehicle_obj_id, entity_id)) {
        if (g_state.host_vehicle_obj_id != kInvalidObjId)
            (void)g_state.entities.unbind_obj_id(g_state.host_vehicle_obj_id);

        const EntityRegistryBindResult bound =
            g_state.entities.bind(1, vehicle_obj_id);
        if (bound != EntityRegistryBindResult::Inserted &&
            bound != EntityRegistryBindResult::AlreadyBound) {
            LOG_ERROR("cannot bind host vehicle objId=%d code=%u", vehicle_obj_id,
                      static_cast<unsigned>(bound));
            return;
        }
        g_state.host_vehicle_obj_id = vehicle_obj_id;
        if (!g_state.entities.lookup_net_id(vehicle_obj_id, entity_id))
            return;
    }

    VehicleSnapshot snapshot{};
    snapshot.entity_id = entity_id;
    snapshot.sequence = g_state.next_snapshot_sequence++;
    snapshot.server_tick = g_state.server_tick;
    snapshot.position = to_snapshot_vector(vehicle->GetPosition());
    snapshot.rotation = to_snapshot_quaternion(vehicle->GetRotation());
    snapshot.linear_velocity = to_snapshot_vector(vehicle->GetLinearVelocity());
    snapshot.angular_velocity = to_snapshot_vector(vehicle->GetAngularVelocity());

    std::array<Byte, kVehicleSnapshotWireSize> payload{};
    const VehicleSnapshotCodecError encoded =
        encode_vehicle_snapshot(snapshot, MutableByteView{payload});
    if (!vehicle_snapshot_codec_succeeded(encoded)) {
        LOG_ERROR("host snapshot encode failed code=%u",
                  static_cast<unsigned>(encoded));
        return;
    }

    for (const PeerId peer : g_state.peers) {
        const TransportResult result = g_state.session->send(
            peer, MessageType::Snapshot, Channel::Unreliable, ByteView{payload});
        if (!result)
            LOG_ERROR("snapshot send to peer=%u failed code=%u", peer,
                      static_cast<unsigned>(result.code));
    }

    // Each client-owned vehicle is simulated only by the host, then sent to
    // every client (including its owner) as the authoritative correction.
    for (const PeerController& controller : g_state.controllers) {
        hta::ai::Vehicle* const remote_vehicle = find_vehicle(controller.entity_id);
        if (remote_vehicle == nullptr)
            continue;
        VehicleSnapshot remote{};
        remote.entity_id = controller.entity_id;
        remote.sequence = g_state.next_snapshot_sequence++;
        remote.server_tick = g_state.server_tick;
        remote.position = to_snapshot_vector(remote_vehicle->GetPosition());
        remote.rotation = to_snapshot_quaternion(remote_vehicle->GetRotation());
        remote.linear_velocity = to_snapshot_vector(remote_vehicle->GetLinearVelocity());
        remote.angular_velocity = to_snapshot_vector(remote_vehicle->GetAngularVelocity());
        std::array<Byte, kVehicleSnapshotWireSize> remote_payload{};
        if (!vehicle_snapshot_codec_succeeded(
                encode_vehicle_snapshot(remote, remote_payload)))
            continue;
        for (const PeerId peer : g_state.peers)
            (void)g_state.session->send(peer, MessageType::Snapshot,
                                        Channel::Unreliable, remote_payload);
    }
}

// The original call is ai::CServer::Update(float): ECX=this, float on stack.
// A free __fastcall hook reserves EDX as the second dummy argument.
void __fastcall server_update_hook(void* server, void*, float elapsed_time)
{
    // Receive/apply packets before native gameplay and ODE advance.
    pump();
    apply_host_inputs();
    apply_host_weapons();
    g_server_update(server, nullptr, elapsed_time);
    ++g_state.server_tick;
    apply_remote_snapshots();
    apply_local_correction();
    send_client_input();
    // The ODE frame is complete here; capture only through Vehicle/PhysicObj API.
    capture_and_broadcast_host_snapshot();
}

} // namespace

void Apply(const Config* config)
{
    if (!config)
        return;
    const EffectiveConfig effective = effective_config(*config);
    if (!effective.enabled)
        return;
    if (g_state.session)
        return;

    SessionConfig session_config{};
    session_config.role = effective.host
                              ? SessionRole::Server
                              : SessionRole::Client;
    session_config.transport.role = session_config.role == SessionRole::Server
                                        ? TransportRole::Server
                                        : TransportRole::Client;
    session_config.transport.bind_endpoint.host =
        session_config.role == SessionRole::Server ? "0.0.0.0" : "127.0.0.1";
    session_config.transport.bind_endpoint.port =
        effective.port;
    session_config.transport.max_peers = effective.max_peers;

    g_state.session =
        std::make_unique<Session>(g_state.transport, std::move(session_config));
    TransportResult result = g_state.session->start();
    if (!result) {
        LOG_ERROR("failed to start network role=%s port=%u code=%u",
                  effective.host ? "host" : "client",
                  effective.port,
                  static_cast<unsigned>(result.code));
        g_state.session.reset();
        return;
    }

    if (!effective.host) {
        Endpoint endpoint{effective.address, effective.port};
        result = g_state.session->connect(endpoint);
        if (!result) {
            LOG_ERROR("failed to connect to %s:%u code=%u", endpoint.host.c_str(),
                      endpoint.port, static_cast<unsigned>(result.code));
            g_state.session->stop();
            g_state.session.reset();
            return;
        }
    }

    try {
        routines::ChangeCall(reinterpret_cast<void*>(kServerUpdateCallSite),
                             &server_update_hook);
        g_state.hook_installed = true;
    }
    catch (const std::exception& error) {
        LOG_ERROR("failed to install server tick hook: %s", error.what());
        g_state.session->stop();
        g_state.session.reset();
        return;
    }

    g_state.next_ping = Clock::now() + std::chrono::seconds(1);
    g_state.next_snapshot = Clock::now();
    g_state.next_snapshot_sequence = 1;
    g_state.server_tick = 0;
    g_state.entities.clear();
    g_state.remote_entities.clear();
    g_state.controllers.clear();
    g_state.local_entity_id = kInvalidNetId;
    g_state.next_input = Clock::now();
    g_state.next_input_sequence = 1;
    g_state.next_weapon_sequence = 1;
    g_state.host_vehicle_obj_id = kInvalidObjId;
    g_state.is_host = effective.host;
    ::kraken::runtime::OnLoad(&register_lua_api);
    LOG_INFO("network started role=%s endpoint=%s:%u max_peers=%u",
             effective.host ? "host" : "client",
             effective.host ? "0.0.0.0" : effective.address.c_str(),
             effective.port, effective.max_peers);
}

bool SubmitLocalWeaponCommand(int gun_id, bool trigger_held)
{
    if (g_state.is_host || !g_state.session ||
        g_state.local_entity_id == kInvalidNetId || g_state.peers.empty())
        return false;
    WeaponCommand command{};
    command.entity_id = g_state.local_entity_id;
    command.sequence = g_state.next_weapon_sequence++;
    command.client_tick = g_state.server_tick;
    command.gun_id = gun_id;
    command.trigger_held = trigger_held;
    std::array<Byte, kWeaponCommandWireSize> payload{};
    if (encode_weapon_command(command, payload) != WeaponCommandCodecError::None)
        return false;
    return static_cast<bool>(g_state.session->send(
        g_state.peers.front(), MessageType::WeaponCommand, Channel::Reliable,
        payload));
}

bool RequestLocalLoot(LootId loot_id, LootTransactionId transaction_id,
                      std::uint32_t amount)
{
    if (g_state.is_host || !g_state.session ||
        g_state.local_entity_id == kInvalidNetId || g_state.peers.empty())
        return false;
    LootRequest request{g_state.local_entity_id, loot_id, transaction_id, amount};
    std::array<Byte, kLootRequestWireSize> payload{};
    if (encode_loot_request(request, payload) != LootCodecError::None)
        return false;
    return static_cast<bool>(g_state.session->send(g_state.peers.front(),
        MessageType::LootRequest, Channel::Reliable, payload));
}

LootId SpawnHostLoot(std::int32_t chest_prototype_id, std::int32_t resource_id,
                     std::uint32_t amount)
{
    if (!g_state.is_host || amount == 0 || chest_prototype_id < 0 || resource_id < 0)
        return 0;
    hta::ai::CServer* const server = hta::ai::CServer::Instance();
    hta::ai::Player* const player = hta::ai::Player::Instance();
    hta::ai::Vehicle* const vehicle = player ? player->GetVehicle() : nullptr;
    if (server == nullptr || server->m_pObjects == nullptr || vehicle == nullptr)
        return 0;
    const LootId loot_id = g_state.next_loot_id++;
    char name[48]{};
    std::snprintf(name, sizeof(name), "kraken_loot_%u", loot_id);
    const ObjId object_id = server->m_pObjects->CreateNewObject(
        chest_prototype_id, name, -1, -1);
    if (object_id < 0) return 0;
    hta::ai::Obj* const object = server->m_pObjects->GetEntityByObjId(object_id);
    hta::ai::Chest* const chest = object ? reinterpret_cast<hta::ai::Chest*>(object) : nullptr;
    if (chest == nullptr || chest->GetRepository() == nullptr ||
        !chest->GetRepository()->AddItems(resource_id, static_cast<int32_t>(amount)))
        return 0;
    hta::CVector position = vehicle->GetPosition();
    position.x += 4.0f;
    chest->SetPositionSelf(position);
    g_state.loot_records.push_back({loot_id, object_id, resource_id, amount});
    LOG_INFO("loot spawned id=%u objId=%d resource=%d amount=%u", loot_id,
             object_id, resource_id, amount);
    return loot_id;
}

} // namespace kraken::net::runtime
