#ifndef KRAKEN_NET_WORLD_STATE_SNAPSHOT_HPP
#define KRAKEN_NET_WORLD_STATE_SNAPSHOT_HPP

#include "net/world_observer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace kraken::net {

// Epoch and revision are deliberately not part of this wire format.  They
// remain fields of WorldSnapshot, whose payload is the byte sequence encoded
// here.
inline constexpr std::uint32_t kWorldStateSnapshotWireMagic = 0x31535757u;
inline constexpr std::uint16_t kWorldStateSnapshotWireVersion = 1;
inline constexpr std::uint16_t kWorldStateSnapshotWireFlags = 0;
inline constexpr std::size_t kWorldStateSnapshotWireHeaderSize = 16;

inline constexpr std::size_t kWorldStateSnapshotMaxObjects = 4096;
inline constexpr std::size_t kWorldStateSnapshotMaxPropertiesPerObject = 256;
inline constexpr std::size_t kWorldStateSnapshotMaxProperties = 65536;
inline constexpr std::size_t kWorldStateSnapshotMaxBlobBytes = 64u * 1024u;
inline constexpr std::size_t kWorldStateSnapshotMaxWireBytes = 4u * 1024u * 1024u;

// Short aliases are useful at call sites and keep the limits discoverable for
// packet-budget checks.
inline constexpr std::size_t kMaxWorldStateSnapshotObjects =
    kWorldStateSnapshotMaxObjects;
inline constexpr std::size_t kMaxWorldStateSnapshotProperties =
    kWorldStateSnapshotMaxPropertiesPerObject;
inline constexpr std::size_t kMaxWorldStateSnapshotBlob =
    kWorldStateSnapshotMaxBlobBytes;
inline constexpr std::size_t kMaxWorldStateSnapshotWire =
    kWorldStateSnapshotMaxWireBytes;

struct WorldStateSnapshot {
    std::vector<ObjectRecord> objects;

    [[nodiscard]] bool semantic_equal(
        const WorldStateSnapshot& other) const;
};

enum class WorldStateSnapshotCodecError : std::uint8_t {
    None,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadFlags,
    WireTooLarge,
    TooManyObjects,
    TooManyProperties,
    BlobTooLarge,
    InvalidObjectId,
    InvalidTypeId,
    InvalidPropertyId,
    InvalidParentId,
    UnknownParent,
    SelfParent,
    ParentCycle,
    DuplicateObjectId,
    DuplicatePropertyId,

    // Descriptive aliases used by older codec call sites.
    InvalidIdentity = InvalidObjectId,
    InvalidPrototype = InvalidTypeId,
    DuplicateId = DuplicateObjectId,
    DuplicateProperty = DuplicatePropertyId,
    PayloadTooLarge = BlobTooLarge,
    InputTooLarge = WireTooLarge,
};

[[nodiscard]] constexpr bool world_state_snapshot_codec_succeeded(
    const WorldStateSnapshotCodecError error) noexcept
{ return error == WorldStateSnapshotCodecError::None; }

[[nodiscard]] WorldStateSnapshotCodecError encode_world_state_snapshot(
    std::span<const ObjectRecord> records, std::vector<Byte>& output);

[[nodiscard]] inline WorldStateSnapshotCodecError encode_world_state_snapshot(
    const std::vector<ObjectRecord>& records, std::vector<Byte>& output)
{ return encode_world_state_snapshot(std::span<const ObjectRecord>{records}, output); }

[[nodiscard]] inline WorldStateSnapshotCodecError encode_world_state_snapshot(
    const WorldStateSnapshot& snapshot, std::vector<Byte>& output)
{ return encode_world_state_snapshot(snapshot.objects, output); }

// Decode is transactional: output is assigned only after the complete header,
// bounds, identity, graph, and canonical-record validation succeeds.
[[nodiscard]] WorldStateSnapshotCodecError decode_world_state_snapshot(
    ByteView input, std::vector<ObjectRecord>& output);

[[nodiscard]] WorldStateSnapshotCodecError decode_world_state_snapshot(
    ByteView input, WorldStateSnapshot& output);

[[nodiscard]] bool world_state_snapshot_semantic_equal(
    std::span<const ObjectRecord> left,
    std::span<const ObjectRecord> right);

[[nodiscard]] inline bool world_state_snapshot_semantic_equal(
    const std::vector<ObjectRecord>& left,
    const std::vector<ObjectRecord>& right)
{ return world_state_snapshot_semantic_equal(
      std::span<const ObjectRecord>{left}, std::span<const ObjectRecord>{right}); }

[[nodiscard]] inline bool world_state_snapshot_semantic_equal(
    const WorldStateSnapshot& left, const WorldStateSnapshot& right)
{ return world_state_snapshot_semantic_equal(left.objects, right.objects); }

[[nodiscard]] bool world_state_snapshot_semantic_equal(
    ByteView left, ByteView right);

using WorldStateSnapshotDigest = std::array<std::uint8_t, 32>;

// The digest is SHA-256 of the canonical payload.  An invalid record set
// returns an all-zero digest; try_* exposes the validity bit when required.
[[nodiscard]] WorldStateSnapshotDigest world_state_snapshot_digest(
    std::span<const ObjectRecord> records);

[[nodiscard]] inline WorldStateSnapshotDigest world_state_snapshot_digest(
    const std::vector<ObjectRecord>& records)
{ return world_state_snapshot_digest(std::span<const ObjectRecord>{records}); }

[[nodiscard]] inline WorldStateSnapshotDigest world_state_snapshot_digest(
    const WorldStateSnapshot& snapshot)
{ return world_state_snapshot_digest(snapshot.objects); }

[[nodiscard]] WorldStateSnapshotDigest world_state_snapshot_digest(ByteView input);

[[nodiscard]] bool try_world_state_snapshot_digest(
    std::span<const ObjectRecord> records, WorldStateSnapshotDigest& output);

[[nodiscard]] std::string world_state_snapshot_digest_hex(
    std::span<const ObjectRecord> records);

// The visitor is intentionally engine-neutral.  create_record is the most
// expressive callback; create_object is convenient for an applier exposing a
// (host id, type id) operation.  For each phase, the first populated callback
// is used.  Missing callbacks are no-ops so a caller may subscribe to only the
// phase it owns.
using WorldStateSnapshotCreateRecordCallback =
    std::function<bool(const ObjectRecord&)>;
using WorldStateSnapshotCreateCallback =
    std::function<bool(HostObjectId, ObjectTypeId)>;
using WorldStateSnapshotRelationshipCallback =
    std::function<bool(HostObjectId, HostObjectId)>;
using WorldStateSnapshotRuntimeCallback =
    std::function<bool(HostObjectId, ByteView)>;
using WorldStateSnapshotPropertyCallback =
    std::function<bool(HostObjectId, PropertyId, ByteView)>;

struct WorldStateSnapshotVisitor {
    WorldStateSnapshotCreateRecordCallback create_record;
    WorldStateSnapshotCreateCallback create_object;
    WorldStateSnapshotRelationshipCallback relationship;
    WorldStateSnapshotRuntimeCallback runtime;
    WorldStateSnapshotPropertyCallback property;

    // Named aliases make the seam pleasant for mutation-applier adapters.
    WorldStateSnapshotCreateRecordCallback on_create_record;
    WorldStateSnapshotCreateCallback on_create;
    WorldStateSnapshotRelationshipCallback on_relationship;
    WorldStateSnapshotRuntimeCallback on_runtime;
    WorldStateSnapshotPropertyCallback on_property;
};

enum class WorldStateSnapshotApplyError : std::uint8_t {
    None,
    InvalidSnapshot,
    CallbackFailed,
};

[[nodiscard]] constexpr bool world_state_snapshot_apply_succeeded(
    const WorldStateSnapshotApplyError error) noexcept
{ return error == WorldStateSnapshotApplyError::None; }

// Application order is deterministic: parent-before-child creates, then
// parent relationships, then runtime values and ascending property IDs.
[[nodiscard]] WorldStateSnapshotApplyError apply_world_state_snapshot(
    std::span<const ObjectRecord> records,
    const WorldStateSnapshotVisitor& visitor);

[[nodiscard]] inline WorldStateSnapshotApplyError apply_world_state_snapshot(
    const std::vector<ObjectRecord>& records,
    const WorldStateSnapshotVisitor& visitor)
{ return apply_world_state_snapshot(
      std::span<const ObjectRecord>{records}, visitor); }

[[nodiscard]] inline WorldStateSnapshotApplyError apply_world_state_snapshot(
    const WorldStateSnapshot& snapshot,
    const WorldStateSnapshotVisitor& visitor)
{ return apply_world_state_snapshot(snapshot.objects, visitor); }

[[nodiscard]] WorldStateSnapshotApplyError apply_world_state_snapshot(
    ByteView input, const WorldStateSnapshotVisitor& visitor);

} // namespace kraken::net

#endif // KRAKEN_NET_WORLD_STATE_SNAPSHOT_HPP
