#include "net/quest_state_projection.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace kraken::net {
namespace {

enum class PacketKind : std::uint8_t { Snapshot = 1, Delta = 2 };
constexpr std::size_t kRecordHeaderSize = 26;

void put_u16(std::vector<Byte>& output, std::uint16_t value)
{
    output.push_back(static_cast<Byte>(value & 0xffu));
    output.push_back(static_cast<Byte>((value >> 8u) & 0xffu));
}

void put_u32(std::vector<Byte>& output, std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8)
        output.push_back(static_cast<Byte>((value >> shift) & 0xffu));
}

void put_u64(std::vector<Byte>& output, std::uint64_t value)
{
    for (unsigned int shift = 0; shift < 64; shift += 8)
        output.push_back(static_cast<Byte>((value >> shift) & 0xffu));
}

void put_text(std::vector<Byte>& output, const std::string& value)
{
    output.insert(output.end(), reinterpret_cast<const Byte*>(value.data()),
                  reinterpret_cast<const Byte*>(value.data() + value.size()));
}

class Reader final {
public:
    explicit Reader(ByteView input) : m_input(input) {}

    [[nodiscard]] bool u8(std::uint8_t& value) noexcept
    {
        if (m_offset >= m_input.size())
            return false;
        value = std::to_integer<std::uint8_t>(m_input[m_offset++]);
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& value) noexcept
    {
        std::uint8_t a = 0;
        std::uint8_t b = 0;
        if (!u8(a) || !u8(b))
            return false;
        value = static_cast<std::uint16_t>(a) |
                (static_cast<std::uint16_t>(b) << 8u);
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) noexcept
    {
        std::uint8_t bytes[4]{};
        for (std::uint8_t& byte : bytes) {
            if (!u8(byte))
                return false;
        }
        value = static_cast<std::uint32_t>(bytes[0]) |
                (static_cast<std::uint32_t>(bytes[1]) << 8u) |
                (static_cast<std::uint32_t>(bytes[2]) << 16u) |
                (static_cast<std::uint32_t>(bytes[3]) << 24u);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) noexcept
    {
        std::uint8_t bytes[8]{};
        for (std::uint8_t& byte : bytes) {
            if (!u8(byte))
                return false;
        }
        value = 0;
        for (unsigned int index = 0; index < 8; ++index)
            value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8u);
        return true;
    }

    [[nodiscard]] bool text(std::size_t count, std::string& value)
    {
        if (count > m_input.size() - m_offset)
            return false;
        value.assign(reinterpret_cast<const char*>(m_input.data()) + m_offset,
                     count);
        m_offset += count;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    { return m_input.size() - m_offset; }

private:
    ByteView m_input;
    std::size_t m_offset = 0;
};

bool valid_source_kind(QuestProjectionSourceKind kind) noexcept
{
    return kind == QuestProjectionSourceKind::TriggerNormal ||
           kind == QuestProjectionSourceKind::TriggerCinematic ||
           kind == QuestProjectionSourceKind::DynamicQuest ||
           kind == QuestProjectionSourceKind::ReferencedObject;
}

bool valid_text(const std::string& value, bool allow_empty = true)
{
    if (!allow_empty && value.empty())
        return false;
    if (value.size() > kQuestProjectionMaxIdentityFieldBytes)
        return false;
    for (const unsigned char character : value) {
        if (character < 0x20u || character == 0x7fu)
            return false;
    }
    return true;
}

bool identity_less(const QuestProjectionRecord& left,
                   const QuestProjectionRecord& right)
{
    const std::string left_key = left.identity.canonical_key();
    const std::string right_key = right.identity.canonical_key();
    if (left_key != right_key)
        return left_key < right_key;
    return static_cast<std::uint8_t>(left.kind()) <
           static_cast<std::uint8_t>(right.kind());
}

bool record_key_equal(const QuestProjectionRecord& left,
                      const QuestProjectionRecord& right)
{ return left.identity.canonical_key() == right.identity.canonical_key(); }

QuestProjectionCodecError validate_identity(
    const QuestProjectionIdentity& identity,
    std::string_view expected_fingerprint = {})
{
    if (identity.resource_fingerprint.size() > kQuestProjectionMaxIdentityFieldBytes ||
        identity.map_namespace.size() > kQuestProjectionMaxIdentityFieldBytes ||
        identity.resource_path.size() > kQuestProjectionMaxIdentityFieldBytes ||
        identity.stable_name.size() > kQuestProjectionMaxIdentityFieldBytes)
        return QuestProjectionCodecError::IdentityTooLong;
    if (!valid_text(identity.resource_fingerprint, false) ||
        !valid_text(identity.map_namespace, false) ||
        !valid_text(identity.resource_path, false) ||
        !valid_text(identity.stable_name, false) ||
        !valid_source_kind(identity.source_kind))
        return QuestProjectionCodecError::InvalidIdentity;
    if (!expected_fingerprint.empty() &&
        identity.resource_fingerprint != expected_fingerprint)
        return QuestProjectionCodecError::InvalidFingerprint;
    if (identity.id == kInvalidQuestProjectionId)
        return QuestProjectionCodecError::InvalidIdentity;
    if (identity.id != quest_projection_id_hash(identity.canonical_key()))
        return QuestProjectionCodecError::IdentityHashMismatch;
    return QuestProjectionCodecError::None;
}

QuestProjectionCodecError validate_record(
    const QuestProjectionRecord& record, std::string_view expected_fingerprint = {})
{
    const QuestProjectionCodecError identity_error =
        validate_identity(record.identity, expected_fingerprint);
    if (identity_error != QuestProjectionCodecError::None)
        return identity_error;
    if (record.kind() == QuestProjectionRecordKind::Trigger) {
        if (record.identity.source_kind != QuestProjectionSourceKind::TriggerNormal &&
            record.identity.source_kind != QuestProjectionSourceKind::TriggerCinematic)
            return QuestProjectionCodecError::InvalidSourceKind;
        const TriggerProjectionState& trigger =
            std::get<TriggerProjectionState>(record.state);
        if (trigger.state != QuestTriggerState::EventWait &&
            trigger.state != QuestTriggerState::Action &&
            trigger.state != QuestTriggerState::Off)
            return QuestProjectionCodecError::InvalidTriggerState;
        if (!std::isfinite(trigger.timeout_for_time_period) ||
            !valid_text(trigger.fly_path_for_cinematic_fly) ||
            !valid_text(trigger.call_obj_name))
            return QuestProjectionCodecError::InvalidScalar;
        if (trigger.object_refs.size() > kQuestProjectionMaxObjectReferences)
            return QuestProjectionCodecError::TooManyReferences;
        for (const QuestProjectionIdentity& reference : trigger.object_refs) {
            if (validate_identity(reference, expected_fingerprint) !=
                QuestProjectionCodecError::None ||
                reference.source_kind != QuestProjectionSourceKind::ReferencedObject)
                return QuestProjectionCodecError::InvalidIdentity;
        }
        if (trigger.call_obj_ref.has_value() &&
            (validate_identity(*trigger.call_obj_ref, expected_fingerprint) !=
                 QuestProjectionCodecError::None ||
             trigger.call_obj_ref->source_kind !=
                 QuestProjectionSourceKind::ReferencedObject))
            return QuestProjectionCodecError::InvalidIdentity;
        return QuestProjectionCodecError::None;
    }
    if (record.identity.source_kind != QuestProjectionSourceKind::DynamicQuest)
        return QuestProjectionCodecError::InvalidSourceKind;
    const DynamicQuestProjectionState& quest =
        std::get<DynamicQuestProjectionState>(record.state);
        if (quest.status < DynamicQuestStatus::NotTaken ||
        quest.status > DynamicQuestStatus::Forgotten ||
        !valid_text(quest.hirer_name) || !valid_text(quest.target_name))
        return QuestProjectionCodecError::InvalidQuestStatus;
    for (const auto& reference : {quest.hirer_reference, quest.target_reference}) {
        if (reference.has_value() &&
            (validate_identity(*reference, expected_fingerprint) !=
                 QuestProjectionCodecError::None ||
             reference->source_kind != QuestProjectionSourceKind::ReferencedObject))
            return QuestProjectionCodecError::InvalidIdentity;
    }
    return QuestProjectionCodecError::None;
}

QuestProjectionCodecError validate_records(
    std::span<const QuestProjectionRecord> input, bool allow_removed,
    std::vector<QuestProjectionRecord>& canonical,
    std::string_view expected_fingerprint = {})
{
    if (input.size() > kQuestProjectionMaxRecords)
        return QuestProjectionCodecError::TooManyRecords;
    canonical.assign(input.begin(), input.end());
    for (const QuestProjectionRecord& record : canonical) {
        if (!allow_removed && record.removed)
            return QuestProjectionCodecError::RemovedInSnapshot;
        const QuestProjectionCodecError error =
            validate_record(record, expected_fingerprint);
        if (error != QuestProjectionCodecError::None)
            return error;
    }
    std::sort(canonical.begin(), canonical.end(), identity_less);
    for (std::size_t index = 1; index < canonical.size(); ++index) {
        if (!record_key_equal(canonical[index - 1], canonical[index]))
            continue;
        if (canonical[index - 1].identity.id != canonical[index].identity.id)
            return QuestProjectionCodecError::HashCollision;
        return QuestProjectionCodecError::DuplicateIdentity;
    }
    for (std::size_t left = 0; left < canonical.size(); ++left) {
        for (std::size_t right = left + 1; right < canonical.size(); ++right) {
            if (canonical[left].identity.id == canonical[right].identity.id &&
                !record_key_equal(canonical[left], canonical[right]))
                return QuestProjectionCodecError::HashCollision;
        }
    }
    return QuestProjectionCodecError::None;
}

QuestProjectionCodecError validate_packet_identity(
    std::string_view fingerprint)
{
    return valid_text(std::string(fingerprint), false)
               ? QuestProjectionCodecError::None
               : QuestProjectionCodecError::InvalidFingerprint;
}

// Kept as a small local helper so the wire layout below remains visibly
// length-delimited and cannot be confused with an engine pointer/ObjId.
void encode_identity_payload(const QuestProjectionIdentity& identity,
                             std::vector<Byte>& output)
{
    put_u16(output, static_cast<std::uint16_t>(identity.resource_fingerprint.size()));
    put_u16(output, static_cast<std::uint16_t>(identity.map_namespace.size()));
    put_u16(output, static_cast<std::uint16_t>(identity.resource_path.size()));
    put_u16(output, static_cast<std::uint16_t>(identity.stable_name.size()));
    put_text(output, identity.resource_fingerprint);
    put_text(output, identity.map_namespace);
    put_text(output, identity.resource_path);
    put_text(output, identity.stable_name);
}

bool decode_identity_payload(Reader& reader, QuestProjectionIdentity& identity);

void encode_reference(const QuestProjectionIdentity& identity,
                      std::vector<Byte>& output)
{
    output.push_back(static_cast<Byte>(identity.source_kind));
    output.push_back(static_cast<Byte>(0));
    put_u64(output, identity.id);
    encode_identity_payload(identity, output);
}

bool decode_reference(Reader& reader, QuestProjectionIdentity& identity)
{
    std::uint8_t source_kind = 0;
    std::uint8_t reserved = 0;
    if (!reader.u8(source_kind) || !reader.u8(reserved) || reserved != 0 ||
        !reader.u64(identity.id) || !decode_identity_payload(reader, identity))
        return false;
    identity.source_kind = static_cast<QuestProjectionSourceKind>(source_kind);
    return true;
}

bool decode_identity_payload(Reader& reader, QuestProjectionIdentity& identity)
{
    std::uint16_t fingerprint_size = 0;
    std::uint16_t map_size = 0;
    std::uint16_t path_size = 0;
    std::uint16_t name_size = 0;
    if (!reader.u16(fingerprint_size) || !reader.u16(map_size) ||
        !reader.u16(path_size) || !reader.u16(name_size) ||
        fingerprint_size > kQuestProjectionMaxIdentityFieldBytes ||
        map_size > kQuestProjectionMaxIdentityFieldBytes ||
        path_size > kQuestProjectionMaxIdentityFieldBytes ||
        name_size > kQuestProjectionMaxIdentityFieldBytes ||
        !reader.text(fingerprint_size, identity.resource_fingerprint) ||
        !reader.text(map_size, identity.map_namespace) ||
        !reader.text(path_size, identity.resource_path) ||
        !reader.text(name_size, identity.stable_name))
        return false;
    return true;
}

void encode_record(const QuestProjectionRecord& record, std::vector<Byte>& output)
{
    output.push_back(static_cast<Byte>(record.kind()));
    output.push_back(static_cast<Byte>(record.removed ? 1u : 0u));
    put_u16(output, 0);
    put_u32(output, record.dependency_order);
    put_u64(output, record.identity.id);
    output.push_back(static_cast<Byte>(record.identity.source_kind));
    output.push_back(static_cast<Byte>(0));
    encode_identity_payload(record.identity, output);
    if (const auto* trigger = std::get_if<TriggerProjectionState>(&record.state)) {
        output.push_back(static_cast<Byte>(trigger->state));
        output.push_back(static_cast<Byte>(trigger->state_keep ? 1u : 0u));
        output.push_back(static_cast<Byte>(trigger->can_update ? 1u : 0u));
        output.push_back(static_cast<Byte>(0));
        put_u32(output, std::bit_cast<std::uint32_t>(trigger->count));
        put_u32(output, trigger->frames_for_frames_passed);
        put_u32(output, std::bit_cast<std::uint32_t>(trigger->timeout_for_time_period));
        put_u16(output, static_cast<std::uint16_t>(trigger->fly_path_for_cinematic_fly.size()));
        put_u16(output, static_cast<std::uint16_t>(trigger->object_refs.size()));
        put_u32(output, std::bit_cast<std::uint32_t>(trigger->call_event_id));
        put_u16(output, static_cast<std::uint16_t>(trigger->call_obj_name.size()));
        put_u16(output, static_cast<std::uint16_t>(
            trigger->call_obj_ref.has_value() ? 1u : 0u));
        put_text(output, trigger->fly_path_for_cinematic_fly);
        for (const QuestProjectionIdentity& reference : trigger->object_refs)
            encode_reference(reference, output);
        put_text(output, trigger->call_obj_name);
        if (trigger->call_obj_ref.has_value())
            encode_reference(*trigger->call_obj_ref, output);
        put_u32(output, std::bit_cast<std::uint32_t>(trigger->id_for_cinema_msg));
    } else {
        const auto& quest = std::get<DynamicQuestProjectionState>(record.state);
        output.push_back(static_cast<Byte>(quest.status));
        output.push_back(static_cast<Byte>(0));
        output.push_back(static_cast<Byte>(0));
        output.push_back(static_cast<Byte>(0));
        put_u32(output, std::bit_cast<std::uint32_t>(quest.reward));
        put_u64(output, std::bit_cast<std::uint64_t>(quest.take_game_time));
        put_u16(output, static_cast<std::uint16_t>(quest.hirer_name.size()));
        put_u16(output, static_cast<std::uint16_t>(quest.target_name.size()));
        put_u16(output, static_cast<std::uint16_t>(quest.hirer_reference.has_value() ? 1u : 0u));
        put_u16(output, static_cast<std::uint16_t>(quest.target_reference.has_value() ? 1u : 0u));
        put_text(output, quest.hirer_name);
        put_text(output, quest.target_name);
        if (quest.hirer_reference.has_value())
            encode_reference(*quest.hirer_reference, output);
        if (quest.target_reference.has_value())
            encode_reference(*quest.target_reference, output);
    }
}

QuestProjectionCodecError decode_packet(
    ByteView input, PacketKind expected_kind, QuestProjectionEpoch& epoch,
    QuestProjectionRevision& base_revision, QuestProjectionRevision& revision,
    std::string& resource_fingerprint, std::vector<QuestProjectionRecord>& records)
{
    if (input.size() > kQuestProjectionMaxWireBytes)
        return QuestProjectionCodecError::WireTooLarge;
    if (input.size() < kQuestProjectionWireHeaderSize)
        return QuestProjectionCodecError::InputSizeMismatch;
    Reader reader(input);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t kind = 0;
    std::uint8_t flags = 0;
    std::uint32_t count = 0;
    std::uint32_t payload_size = 0;
    std::uint16_t fingerprint_size = 0;
    std::uint16_t header_reserved = 0;
    if (!reader.u32(magic) || !reader.u16(version) || !reader.u8(kind) ||
        !reader.u8(flags) || !reader.u32(epoch) || !reader.u64(base_revision) ||
        !reader.u64(revision) || !reader.u32(count) || !reader.u32(payload_size) ||
        !reader.u16(fingerprint_size) || !reader.u16(header_reserved))
        return QuestProjectionCodecError::InputSizeMismatch;
    if (magic != kQuestProjectionWireMagic)
        return QuestProjectionCodecError::BadMagic;
    if (version != kQuestProjectionWireVersion)
        return QuestProjectionCodecError::BadVersion;
    if (kind != static_cast<std::uint8_t>(expected_kind))
        return QuestProjectionCodecError::BadKind;
    if (flags != kQuestProjectionWireFlags || header_reserved != 0)
        return QuestProjectionCodecError::BadFlags;
    if (epoch == kInvalidQuestProjectionEpoch)
        return QuestProjectionCodecError::InvalidEpoch;
    if (count > kQuestProjectionMaxRecords)
        return QuestProjectionCodecError::TooManyRecords;
    if (payload_size != reader.remaining() ||
        payload_size > kQuestProjectionMaxWireBytes - kQuestProjectionWireHeaderSize)
        return QuestProjectionCodecError::PayloadTooLarge;
    if (fingerprint_size > kQuestProjectionMaxIdentityFieldBytes ||
        !reader.text(fingerprint_size, resource_fingerprint) ||
        validate_packet_identity(resource_fingerprint) != QuestProjectionCodecError::None)
        return QuestProjectionCodecError::InvalidFingerprint;
    if (expected_kind == PacketKind::Delta &&
        (revision == kInvalidQuestProjectionRevision || revision <= base_revision))
        return QuestProjectionCodecError::InvalidDeltaRange;

    std::vector<QuestProjectionRecord> decoded;
    decoded.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (reader.remaining() < kRecordHeaderSize)
            return QuestProjectionCodecError::InputSizeMismatch;
        std::uint8_t record_kind = 0;
        std::uint8_t record_flags = 0;
        std::uint16_t reserved = 0;
        std::uint32_t dependency_order = 0;
        std::uint64_t identity_id = 0;
        std::uint8_t source_kind = 0;
        std::uint8_t identity_reserved = 0;
        if (!reader.u8(record_kind) || !reader.u8(record_flags) ||
            !reader.u16(reserved) || !reader.u32(dependency_order) ||
            !reader.u64(identity_id) || !reader.u8(source_kind) ||
            !reader.u8(identity_reserved) || reserved != 0 ||
            identity_reserved != 0 || (record_flags & ~1u) != 0)
            return QuestProjectionCodecError::InvalidScalar;
        if (record_kind != static_cast<std::uint8_t>(QuestProjectionRecordKind::Trigger) &&
            record_kind != static_cast<std::uint8_t>(QuestProjectionRecordKind::DynamicQuest))
            return QuestProjectionCodecError::InvalidRecordKind;
        QuestProjectionRecord record;
        record.dependency_order = dependency_order;
        record.removed = (record_flags & 1u) != 0;
        record.identity.id = identity_id;
        record.identity.source_kind = static_cast<QuestProjectionSourceKind>(source_kind);
        if (!decode_identity_payload(reader, record.identity))
            return QuestProjectionCodecError::InputSizeMismatch;
        std::uint8_t first = 0;
        std::uint8_t second = 0;
        std::uint8_t third = 0;
        std::uint8_t fourth = 0;
        if (!reader.u8(first) || !reader.u8(second) || !reader.u8(third) ||
            !reader.u8(fourth) || fourth != 0)
            return QuestProjectionCodecError::InputSizeMismatch;
        if (record_kind == static_cast<std::uint8_t>(QuestProjectionRecordKind::Trigger)) {
            std::uint32_t count_bits = 0;
            std::uint32_t frames = 0;
            std::uint32_t pause_bits = 0;
            std::uint16_t fly_size = 0;
            std::uint16_t ref_count = 0;
            std::uint32_t event_bits = 0;
            std::uint16_t call_name_size = 0;
            std::uint16_t call_ref_size = 0;
            if (!reader.u32(count_bits) || !reader.u32(frames) ||
                !reader.u32(pause_bits) || !reader.u16(fly_size) ||
                !reader.u16(ref_count) || !reader.u32(event_bits) ||
                !reader.u16(call_name_size) || !reader.u16(call_ref_size) ||
                fly_size > kQuestProjectionMaxIdentityFieldBytes ||
                ref_count > kQuestProjectionMaxObjectReferences ||
                call_name_size > kQuestProjectionMaxIdentityFieldBytes ||
                call_ref_size > kQuestProjectionMaxIdentityFieldBytes * 5u)
                return QuestProjectionCodecError::InvalidScalar;
            TriggerProjectionState trigger;
            trigger.state = static_cast<QuestTriggerState>(first);
            trigger.state_keep = second != 0;
            trigger.can_update = third != 0;
            trigger.count = std::bit_cast<std::int32_t>(count_bits);
            trigger.frames_for_frames_passed = frames;
            trigger.timeout_for_time_period = std::bit_cast<float>(pause_bits);
            if (!reader.text(fly_size, trigger.fly_path_for_cinematic_fly))
                return QuestProjectionCodecError::InputSizeMismatch;
            for (std::uint16_t ref_index = 0; ref_index < ref_count; ++ref_index) {
                QuestProjectionIdentity reference;
                if (!decode_reference(reader, reference))
                    return QuestProjectionCodecError::InputSizeMismatch;
                trigger.object_refs.push_back(std::move(reference));
            }
            if (!reader.text(call_name_size, trigger.call_obj_name))
                return QuestProjectionCodecError::InputSizeMismatch;
            if (call_ref_size != 0) {
                QuestProjectionIdentity reference;
                if (call_ref_size != 1 || !decode_reference(reader, reference))
                    return QuestProjectionCodecError::InputSizeMismatch;
                trigger.call_obj_ref = std::move(reference);
            }
            std::uint32_t cinema_bits = 0;
            if (!reader.u32(cinema_bits))
                return QuestProjectionCodecError::InputSizeMismatch;
            trigger.call_event_id = std::bit_cast<std::int32_t>(event_bits);
            trigger.id_for_cinema_msg = std::bit_cast<std::int32_t>(cinema_bits);
            record.state = std::move(trigger);
        } else {
            std::uint32_t reward_bits = 0;
            std::uint64_t take_time_bits = 0;
            std::uint16_t hirer_size = 0;
            std::uint16_t target_size = 0;
            std::uint16_t hirer_ref_size = 0;
            std::uint16_t target_ref_size = 0;
            if (second != 0 || third != 0 || !reader.u32(reward_bits) ||
                !reader.u64(take_time_bits) || !reader.u16(hirer_size) ||
                !reader.u16(target_size) || !reader.u16(hirer_ref_size) ||
                !reader.u16(target_ref_size) ||
                hirer_size > kQuestProjectionMaxIdentityFieldBytes ||
                target_size > kQuestProjectionMaxIdentityFieldBytes ||
                hirer_ref_size > 1u || target_ref_size > 1u)
                return QuestProjectionCodecError::InvalidScalar;
            DynamicQuestProjectionState quest;
            quest.status = static_cast<DynamicQuestStatus>(first);
            quest.reward = std::bit_cast<std::int32_t>(reward_bits);
            quest.take_game_time = std::bit_cast<std::int64_t>(take_time_bits);
            if (!reader.text(hirer_size, quest.hirer_name) ||
                !reader.text(target_size, quest.target_name))
                return QuestProjectionCodecError::InputSizeMismatch;
            if (hirer_ref_size != 0) {
                QuestProjectionIdentity reference;
                if (!decode_reference(reader, reference))
                    return QuestProjectionCodecError::InputSizeMismatch;
                quest.hirer_reference = std::move(reference);
            }
            if (target_ref_size != 0) {
                QuestProjectionIdentity reference;
                if (!decode_reference(reader, reference))
                    return QuestProjectionCodecError::InputSizeMismatch;
                quest.target_reference = std::move(reference);
            }
            record.state = std::move(quest);
        }
        decoded.push_back(std::move(record));
    }
    if (reader.remaining() != 0)
        return QuestProjectionCodecError::InputSizeMismatch;
    std::vector<QuestProjectionRecord> canonical;
    const QuestProjectionCodecError validation = validate_records(
        decoded, expected_kind == PacketKind::Delta, canonical,
        resource_fingerprint);
    if (validation != QuestProjectionCodecError::None)
        return validation;
    records.swap(canonical);
    return QuestProjectionCodecError::None;
}

QuestProjectionCodecError encode_packet(
    PacketKind kind, QuestProjectionEpoch epoch, QuestProjectionRevision base,
    QuestProjectionRevision revision, std::string_view fingerprint,
    std::span<const QuestProjectionRecord> input, std::vector<Byte>& output)
{
    if (epoch == kInvalidQuestProjectionEpoch)
        return QuestProjectionCodecError::InvalidEpoch;
    if (kind == PacketKind::Delta &&
        (revision == kInvalidQuestProjectionRevision || revision <= base))
        return QuestProjectionCodecError::InvalidDeltaRange;
    const QuestProjectionCodecError fingerprint_error =
        validate_packet_identity(fingerprint);
    if (fingerprint_error != QuestProjectionCodecError::None)
        return fingerprint_error;
    std::vector<QuestProjectionRecord> canonical;
    const QuestProjectionCodecError validation = validate_records(
        input, kind == PacketKind::Delta, canonical, fingerprint);
    if (validation != QuestProjectionCodecError::None)
        return validation;
    std::vector<Byte> payload;
    put_text(payload, std::string(fingerprint));
    for (const QuestProjectionRecord& record : canonical)
        encode_record(record, payload);
    if (payload.size() > kQuestProjectionMaxWireBytes - kQuestProjectionWireHeaderSize)
        return QuestProjectionCodecError::PayloadTooLarge;
    output.clear();
    output.reserve(kQuestProjectionWireHeaderSize + payload.size());
    put_u32(output, kQuestProjectionWireMagic);
    put_u16(output, kQuestProjectionWireVersion);
    output.push_back(static_cast<Byte>(kind));
    output.push_back(static_cast<Byte>(kQuestProjectionWireFlags));
    put_u32(output, epoch);
    put_u64(output, kind == PacketKind::Delta ? base : 0);
    put_u64(output, revision);
    put_u32(output, static_cast<std::uint32_t>(canonical.size()));
    put_u32(output, static_cast<std::uint32_t>(payload.size()));
    put_u16(output, static_cast<std::uint16_t>(fingerprint.size()));
    put_u16(output, 0);
    output.insert(output.end(), payload.begin(), payload.end());
    return QuestProjectionCodecError::None;
}

bool same_record_set(std::span<const QuestProjectionRecord> left,
                     std::span<const QuestProjectionRecord> right)
{
    if (left.size() != right.size())
        return false;
    std::vector<QuestProjectionRecord> a;
    std::vector<QuestProjectionRecord> b;
    return validate_records(left, true, a) == QuestProjectionCodecError::None &&
           validate_records(right, true, b) == QuestProjectionCodecError::None &&
           a == b;
}

bool apply_delta_to_records(std::vector<QuestProjectionRecord>& target,
                            const QuestProjectionDelta& delta)
{
    std::vector<QuestProjectionRecord> changes;
    if (validate_records(delta.records, true, changes,
                         delta.resource_fingerprint) != QuestProjectionCodecError::None)
        return false;
    for (const QuestProjectionRecord& record : changes) {
        const std::string key = record.identity.canonical_key();
        const auto it = std::find_if(target.begin(), target.end(),
            [&key](const QuestProjectionRecord& item) {
                return item.identity.canonical_key() == key;
            });
        if (record.removed) {
            if (it == target.end())
                return false;
            target.erase(it);
        } else if (it == target.end()) {
            target.push_back(record);
        } else {
            *it = record;
        }
    }
    std::vector<QuestProjectionRecord> canonical;
    if (validate_records(target, false, canonical,
                         delta.resource_fingerprint) != QuestProjectionCodecError::None)
        return false;
    target.swap(canonical);
    return true;
}

} // namespace

QuestProjectionId quest_projection_id_hash(std::string_view key) noexcept
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : key) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    hash &= 0x7fffffffffffffffull;
    return hash == kInvalidQuestProjectionId ? 1u : hash;
}

std::string QuestProjectionIdentity::canonical_key() const
{
    if (!valid_text(resource_fingerprint, false) ||
        !valid_text(map_namespace, false) || !valid_source_kind(source_kind) ||
        !valid_text(resource_path, false) || !valid_text(stable_name, false))
        return {};
    return "quest-v2|" + std::to_string(resource_fingerprint.size()) + ":" +
           resource_fingerprint + "|" + std::to_string(map_namespace.size()) + ":" +
           map_namespace + "|" + std::to_string(static_cast<unsigned>(source_kind)) +
           "|" + std::to_string(resource_path.size()) + ":" + resource_path +
           "|" + std::to_string(stable_name.size()) + ":" + stable_name;
}

bool QuestProjectionIdentity::valid() const
{
    const std::string key = canonical_key();
    return !key.empty() && id != kInvalidQuestProjectionId &&
           id == quest_projection_id_hash(key);
}

bool QuestProjectionIdentity::operator==(
    const QuestProjectionIdentity& other) const
{
    return id == other.id && source_kind == other.source_kind &&
           resource_fingerprint == other.resource_fingerprint &&
           map_namespace == other.map_namespace &&
           resource_path == other.resource_path && stable_name == other.stable_name;
}

QuestProjectionRecordKind QuestProjectionRecord::kind() const noexcept
{
    return std::holds_alternative<TriggerProjectionState>(state)
               ? QuestProjectionRecordKind::Trigger
               : QuestProjectionRecordKind::DynamicQuest;
}

bool QuestProjectionRecord::operator==(
    const QuestProjectionRecord& other) const
{ return identity == other.identity && dependency_order == other.dependency_order &&
         removed == other.removed && state == other.state; }

QuestTriggerProvenanceBindResult QuestTriggerProvenanceRegistry::bind(
    const QuestProjectionSourceKind source_kind,
    std::string resource_path,
    std::string stable_name)
{
    if (m_locked)
        return QuestTriggerProvenanceBindResult::Locked;
    if ((source_kind != QuestProjectionSourceKind::TriggerNormal &&
         source_kind != QuestProjectionSourceKind::TriggerCinematic) ||
        resource_path.empty() || stable_name.empty() ||
        resource_path.size() > kQuestProjectionMaxIdentityFieldBytes ||
        stable_name.size() > kQuestProjectionMaxIdentityFieldBytes) {
        m_locked = true;
        return QuestTriggerProvenanceBindResult::Invalid;
    }
    const auto duplicate = std::find_if(
        m_entries.begin(), m_entries.end(),
        [&stable_name](const Entry& entry) {
            return entry.stable_name == stable_name;
        });
    if (duplicate != m_entries.end()) {
        m_locked = true;
        return QuestTriggerProvenanceBindResult::Duplicate;
    }
    m_entries.push_back({std::move(stable_name),
                         QuestTriggerProvenance{source_kind, std::move(resource_path)}});
    return QuestTriggerProvenanceBindResult::Bound;
}

std::optional<QuestTriggerProvenance> QuestTriggerProvenanceRegistry::lookup(
    const std::string_view stable_name) const
{
    if (m_locked || stable_name.empty())
        return std::nullopt;
    const auto found = std::find_if(
        m_entries.begin(), m_entries.end(),
        [stable_name](const Entry& entry) { return entry.stable_name == stable_name; });
    return found == m_entries.end()
               ? std::nullopt
               : std::optional<QuestTriggerProvenance>(found->provenance);
}

void QuestTriggerProvenanceRegistry::begin_map_load(
    const QuestProjectionSourceKind source_kind) noexcept
{
    if (source_kind == QuestProjectionSourceKind::TriggerNormal)
        reset();
}

void QuestTriggerProvenanceRegistry::reset() noexcept
{
    m_locked = false;
    m_entries.clear();
}

bool collect_quest_projection_reference_keys(
    const std::span<const QuestProjectionRecord> records,
    std::vector<std::string>& output)
{
    output.clear();
    const auto add = [&output](const QuestProjectionIdentity& identity) {
        if (!identity.valid() ||
            identity.source_kind != QuestProjectionSourceKind::ReferencedObject)
            return false;
        const std::string key = identity.canonical_key();
        if (std::find(output.begin(), output.end(), key) == output.end())
            output.push_back(key);
        return true;
    };
    for (const QuestProjectionRecord& record : records) {
        if (record.removed)
            continue;
        if (const auto* trigger = std::get_if<TriggerProjectionState>(&record.state)) {
            for (const QuestProjectionIdentity& reference : trigger->object_refs) {
                if (!add(reference))
                    return false;
            }
            if (trigger->call_obj_ref.has_value() && !add(*trigger->call_obj_ref))
                return false;
        } else {
            const auto& quest = std::get<DynamicQuestProjectionState>(record.state);
            if (quest.hirer_reference.has_value() && !add(*quest.hirer_reference))
                return false;
            if (quest.target_reference.has_value() && !add(*quest.target_reference))
                return false;
        }
    }
    std::sort(output.begin(), output.end());
    return true;
}

QuestProjectionReferenceResolution resolve_quest_projection_reference(
    const std::string_view requested_key,
    const std::span<const std::string> candidate_keys) noexcept
{
    if (requested_key.empty())
        return QuestProjectionReferenceResolution::Missing;
    std::size_t matches = 0;
    for (const std::string& candidate : candidate_keys) {
        if (candidate == requested_key)
            ++matches;
    }
    if (matches == 0)
        return QuestProjectionReferenceResolution::Missing;
    return matches == 1 ? QuestProjectionReferenceResolution::Resolved
                        : QuestProjectionReferenceResolution::Duplicate;
}

QuestProjectionTransactionPlanResult build_quest_projection_transaction_plan(
    const std::span<const QuestProjectionRecord> target,
    const std::span<const std::string> locally_present_keys,
    QuestProjectionTransactionPlan& output)
{
    output.apply_records.clear();
    std::vector<std::string> seen;
    seen.reserve(target.size());
    for (const QuestProjectionRecord& record : target) {
        if (!record.identity.valid())
            return QuestProjectionTransactionPlanResult::InvalidRecord;
        const std::string key = record.identity.canonical_key();
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            return QuestProjectionTransactionPlanResult::InvalidRecord;
        seen.push_back(key);
        const auto present = std::find(
            locally_present_keys.begin(), locally_present_keys.end(), key) !=
            locally_present_keys.end();
        if (record.removed) {
            if (quest_projection_tombstone_result(present) ==
                QuestProjectionTombstoneResult::RejectPresent)
                return QuestProjectionTransactionPlanResult::TombstoneObjectPresent;
            continue;
        }
        output.apply_records.push_back(record);
    }
    return QuestProjectionTransactionPlanResult::Ready;
}

QuestProjectionCodecError encode_quest_projection_snapshot(
    const QuestProjectionSnapshot& snapshot, std::vector<Byte>& output)
{ return encode_packet(PacketKind::Snapshot, snapshot.epoch, 0, snapshot.revision,
                       snapshot.resource_fingerprint, snapshot.records, output); }

QuestProjectionCodecError decode_quest_projection_snapshot(
    ByteView input, QuestProjectionSnapshot& output)
{
    QuestProjectionSnapshot decoded;
    QuestProjectionRevision base = 0;
    const QuestProjectionCodecError result = decode_packet(
        input, PacketKind::Snapshot, decoded.epoch, base, decoded.revision,
        decoded.resource_fingerprint, decoded.records);
    if (result != QuestProjectionCodecError::None)
        return result;
    output = std::move(decoded);
    return QuestProjectionCodecError::None;
}

QuestProjectionCodecError encode_quest_projection_delta(
    const QuestProjectionDelta& delta, std::vector<Byte>& output)
{ return encode_packet(PacketKind::Delta, delta.epoch, delta.base_revision,
                       delta.revision, delta.resource_fingerprint, delta.records,
                       output); }

QuestProjectionCodecError decode_quest_projection_delta(
    ByteView input, QuestProjectionDelta& output)
{
    QuestProjectionDelta decoded;
    const QuestProjectionCodecError result = decode_packet(
        input, PacketKind::Delta, decoded.epoch, decoded.base_revision,
        decoded.revision, decoded.resource_fingerprint, decoded.records);
    if (result != QuestProjectionCodecError::None)
        return result;
    output = std::move(decoded);
    return QuestProjectionCodecError::None;
}

bool quest_projection_records_equal(std::span<const QuestProjectionRecord> left,
                                    std::span<const QuestProjectionRecord> right)
{ return same_record_set(left, right); }

QuestProjectionHost::QuestProjectionHost(const std::size_t history_capacity)
    : m_history_capacity(std::max<std::size_t>(history_capacity, 1u))
{}

void QuestProjectionHost::reset(QuestProjectionEpoch epoch,
                                std::string resource_fingerprint)
{
    m_epoch = epoch == kInvalidQuestProjectionEpoch ? 1u : epoch;
    m_revision = 0;
    m_initialized = false;
    m_resource_fingerprint = std::move(resource_fingerprint);
    m_records.clear();
    m_history.clear();
}

void QuestProjectionHost::set_resource_fingerprint(std::string value)
{ m_resource_fingerprint = std::move(value); }

QuestProjectionSnapshot QuestProjectionHost::snapshot() const
{ return {m_epoch, m_revision, m_resource_fingerprint, m_records}; }

QuestProjectionHostResult QuestProjectionHost::observe(
    std::span<const QuestProjectionRecord> records,
    QuestProjectionDelta& emitted)
{
    emitted = {};
    if (m_resource_fingerprint.empty() && !records.empty())
        m_resource_fingerprint = records.front().identity.resource_fingerprint;
    std::vector<QuestProjectionRecord> canonical;
    if (validate_packet_identity(m_resource_fingerprint) != QuestProjectionCodecError::None ||
        validate_records(records, false, canonical, m_resource_fingerprint) !=
            QuestProjectionCodecError::None)
        return QuestProjectionHostResult::InvalidInput;
    if (!m_initialized) {
        m_records = std::move(canonical);
        m_revision = 0;
        m_initialized = true;
        return QuestProjectionHostResult::Initialized;
    }
    if (m_records == canonical)
        return QuestProjectionHostResult::Unchanged;
    if (m_revision == (std::numeric_limits<QuestProjectionRevision>::max)())
        return QuestProjectionHostResult::RevisionExhausted;
    QuestProjectionDelta delta;
    delta.epoch = m_epoch;
    delta.base_revision = m_revision;
    delta.revision = m_revision + 1u;
    delta.resource_fingerprint = m_resource_fingerprint;
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    while (old_index < m_records.size() || new_index < canonical.size()) {
        if (old_index == m_records.size()) {
            delta.records.push_back(canonical[new_index++]);
            continue;
        }
        if (new_index == canonical.size()) {
            QuestProjectionRecord removed = m_records[old_index++];
            removed.removed = true;
            delta.records.push_back(std::move(removed));
            continue;
        }
        const std::string old_key = m_records[old_index].identity.canonical_key();
        const std::string new_key = canonical[new_index].identity.canonical_key();
        if (old_key < new_key) {
            QuestProjectionRecord removed = m_records[old_index++];
            removed.removed = true;
            delta.records.push_back(std::move(removed));
        } else if (new_key < old_key) {
            delta.records.push_back(canonical[new_index++]);
        } else {
            if (m_records[old_index].kind() != canonical[new_index].kind())
                return QuestProjectionHostResult::InvalidInput;
            if (m_records[old_index] != canonical[new_index])
                delta.records.push_back(canonical[new_index]);
            ++old_index;
            ++new_index;
        }
    }
    if (delta.records.size() > kQuestProjectionMaxRecords)
        return QuestProjectionHostResult::InvalidInput;
    m_revision = delta.revision;
    m_records = std::move(canonical);
    if (m_history.size() == m_history_capacity)
        m_history.erase(m_history.begin());
    m_history.push_back(delta);
    emitted = std::move(delta);
    return QuestProjectionHostResult::DeltaProduced;
}

bool QuestProjectionHost::deltas_after(
    QuestProjectionEpoch epoch, QuestProjectionRevision revision,
    std::vector<QuestProjectionDelta>& output) const
{
    output.clear();
    if (epoch != m_epoch || revision > m_revision)
        return false;
    if (revision == m_revision)
        return true;
    if (m_history.empty() || revision < m_history.front().base_revision)
        return false;
    QuestProjectionRevision expected = revision + 1u;
    for (const QuestProjectionDelta& delta : m_history) {
        if (delta.revision <= revision)
            continue;
        if (delta.base_revision != expected - 1u || delta.revision != expected)
            return false;
        output.push_back(delta);
        ++expected;
    }
    return expected == m_revision + 1u;
}

QuestProjectionClient::QuestProjectionClient(
    std::size_t pending_capacity, QuestProjectionApplyCallback applier)
    : m_pending_capacity(std::max<std::size_t>(pending_capacity, 1u)),
      m_applier(std::move(applier))
{}

void QuestProjectionClient::reset() noexcept
{
    m_state = QuestProjectionClientState::Idle;
    m_epoch = kInvalidQuestProjectionEpoch;
    m_snapshot_revision = kInvalidQuestProjectionRevision;
    m_applied_revision = kInvalidQuestProjectionRevision;
    m_world_ready = false;
    m_resource_fingerprint.clear();
    m_baseline.clear();
    m_pending.clear();
    m_records.clear();
}

void QuestProjectionClient::set_applier(QuestProjectionApplyCallback applier)
{ m_applier = std::move(applier); }

QuestProjectionClientResult QuestProjectionClient::fail_closed(
    QuestProjectionClientResult result) noexcept
{
    m_state = QuestProjectionClientState::ResnapshotRequired;
    m_world_ready = false;
    m_baseline.clear();
    m_pending.clear();
    return result;
}

bool QuestProjectionClient::queue_delta(const QuestProjectionDelta& delta)
{
    const auto position = std::lower_bound(
        m_pending.begin(), m_pending.end(), delta.revision,
        [](const QuestProjectionDelta& item, QuestProjectionRevision revision) {
            return item.revision < revision;
        });
    if (position != m_pending.end() && position->revision == delta.revision)
        return position->base_revision == delta.base_revision &&
               position->resource_fingerprint == delta.resource_fingerprint &&
               quest_projection_records_equal(position->records, delta.records);
    if (m_pending.size() >= m_pending_capacity)
        return false;
    m_pending.insert(position, delta);
    return true;
}

QuestProjectionClientResult QuestProjectionClient::accept_delta(
    const QuestProjectionDelta& delta)
{
    std::vector<QuestProjectionRecord> canonical;
    if (validate_packet_identity(delta.resource_fingerprint) != QuestProjectionCodecError::None ||
        validate_records(delta.records, true, canonical, delta.resource_fingerprint) !=
            QuestProjectionCodecError::None)
        return fail_closed(QuestProjectionClientResult::Invalid);
    QuestProjectionDelta normalized = delta;
    normalized.records = std::move(canonical);
    if (m_epoch == kInvalidQuestProjectionEpoch)
        m_epoch = delta.epoch;
    if (delta.epoch != m_epoch)
        return fail_closed(QuestProjectionClientResult::WrongEpoch);
    if (m_resource_fingerprint.empty())
        m_resource_fingerprint = delta.resource_fingerprint;
    if (delta.resource_fingerprint != m_resource_fingerprint)
        return fail_closed(QuestProjectionClientResult::WrongFingerprint);
    if (m_state == QuestProjectionClientState::ResnapshotRequired)
        return QuestProjectionClientResult::ResnapshotRequired;
    if (m_state == QuestProjectionClientState::Ready) {
        if (delta.revision <= m_applied_revision)
            return QuestProjectionClientResult::Duplicate;
        if (delta.base_revision != m_applied_revision ||
            delta.revision != m_applied_revision + 1u)
            return fail_closed(QuestProjectionClientResult::Gap);
        std::vector<QuestProjectionRecord> target = m_records;
        if (!apply_delta_to_records(target, normalized))
            return fail_closed(QuestProjectionClientResult::Invalid);
        if (m_applier && !m_applier(m_records, target))
            return fail_closed(QuestProjectionClientResult::ApplyFailed);
        m_records = std::move(target);
        m_applied_revision = delta.revision;
        return QuestProjectionClientResult::Applied;
    }
    const auto existing = std::find_if(
        m_pending.begin(), m_pending.end(),
        [&normalized](const QuestProjectionDelta& item) {
            return item.revision == normalized.revision;
        });
    if (existing != m_pending.end()) {
        if (existing->base_revision == normalized.base_revision &&
            existing->resource_fingerprint == normalized.resource_fingerprint &&
            quest_projection_records_equal(existing->records, normalized.records))
            return QuestProjectionClientResult::Duplicate;
        return fail_closed(QuestProjectionClientResult::Invalid);
    }
    if (!queue_delta(normalized))
        return fail_closed(QuestProjectionClientResult::Overflow);
    return QuestProjectionClientResult::Buffered;
}

QuestProjectionClientResult QuestProjectionClient::begin_snapshot(
    const QuestProjectionSnapshot& snapshot)
{
    if (m_state == QuestProjectionClientState::ResnapshotRequired)
        return QuestProjectionClientResult::ResnapshotRequired;
    std::vector<QuestProjectionRecord> canonical;
    if (validate_packet_identity(snapshot.resource_fingerprint) != QuestProjectionCodecError::None ||
        validate_records(snapshot.records, false, canonical, snapshot.resource_fingerprint) !=
            QuestProjectionCodecError::None)
        return fail_closed(QuestProjectionClientResult::Invalid);
    if (m_epoch != kInvalidQuestProjectionEpoch && snapshot.epoch != m_epoch)
        return fail_closed(QuestProjectionClientResult::WrongEpoch);
    if (!m_resource_fingerprint.empty() &&
        snapshot.resource_fingerprint != m_resource_fingerprint)
        return fail_closed(QuestProjectionClientResult::WrongFingerprint);
    if (m_resource_fingerprint.empty())
        m_resource_fingerprint = snapshot.resource_fingerprint;
    if (m_state == QuestProjectionClientState::Ready &&
        snapshot.epoch == m_epoch && snapshot.revision < m_applied_revision)
        return fail_closed(QuestProjectionClientResult::Invalid);
    if (m_state == QuestProjectionClientState::Transferring &&
        snapshot.epoch == m_epoch && snapshot.revision != m_snapshot_revision)
        return fail_closed(QuestProjectionClientResult::Invalid);
    if (m_state == QuestProjectionClientState::Ready &&
        snapshot.epoch == m_epoch && snapshot.revision == m_applied_revision &&
        quest_projection_records_equal(canonical, m_records))
        return QuestProjectionClientResult::Duplicate;
    if (m_state == QuestProjectionClientState::Transferring &&
        snapshot.epoch == m_epoch && snapshot.revision == m_snapshot_revision &&
        quest_projection_records_equal(canonical, m_baseline))
        return QuestProjectionClientResult::Duplicate;
    m_epoch = snapshot.epoch;
    m_snapshot_revision = snapshot.revision;
    m_baseline = std::move(canonical);
    m_world_ready = false;
    m_state = QuestProjectionClientState::Transferring;
    std::vector<QuestProjectionDelta> retained;
    retained.reserve(m_pending.size());
    for (const QuestProjectionDelta& delta : m_pending) {
        if (delta.epoch != m_epoch ||
            delta.resource_fingerprint != m_resource_fingerprint)
            return fail_closed(QuestProjectionClientResult::WrongFingerprint);
        if (delta.revision > m_snapshot_revision)
            retained.push_back(delta);
    }
    m_pending.swap(retained);
    return QuestProjectionClientResult::BaselineAccepted;
}

QuestProjectionClientResult QuestProjectionClient::mark_world_ready() noexcept
{
    if (m_state == QuestProjectionClientState::ResnapshotRequired)
        return QuestProjectionClientResult::ResnapshotRequired;
    if (m_state == QuestProjectionClientState::Idle)
        return QuestProjectionClientResult::NotReady;
    m_world_ready = true;
    return ready() ? QuestProjectionClientResult::Ready
                   : QuestProjectionClientResult::BaselineAccepted;
}

QuestProjectionClientResult QuestProjectionClient::apply_target(
    std::span<const QuestProjectionRecord> target,
    QuestProjectionRevision revision)
{
    std::vector<QuestProjectionRecord> canonical;
    if (validate_records(target, false, canonical, m_resource_fingerprint) !=
        QuestProjectionCodecError::None)
        return fail_closed(QuestProjectionClientResult::Invalid);
    if (m_applier && !m_applier(m_records, canonical))
        return fail_closed(QuestProjectionClientResult::ApplyFailed);
    m_records = std::move(canonical);
    m_applied_revision = revision;
    return QuestProjectionClientResult::Applied;
}

QuestProjectionClientResult QuestProjectionClient::replay_pending()
{
    std::vector<QuestProjectionRecord> target = m_baseline;
    QuestProjectionRevision expected = m_snapshot_revision;
    for (const QuestProjectionDelta& delta : m_pending) {
        if (delta.revision <= expected)
            continue;
        if (delta.base_revision != expected || delta.revision != expected + 1u)
            return fail_closed(QuestProjectionClientResult::Gap);
        if (!apply_delta_to_records(target, delta))
            return fail_closed(QuestProjectionClientResult::Invalid);
        expected = delta.revision;
    }
    const QuestProjectionClientResult applied = apply_target(target, expected);
    if (applied != QuestProjectionClientResult::Applied)
        return applied;
    m_pending.clear();
    m_baseline.clear();
    m_state = QuestProjectionClientState::Ready;
    return QuestProjectionClientResult::Ready;
}

QuestProjectionClientResult QuestProjectionClient::commit()
{
    if (m_state == QuestProjectionClientState::ResnapshotRequired)
        return QuestProjectionClientResult::ResnapshotRequired;
    if (m_state != QuestProjectionClientState::Transferring || !m_world_ready)
        return QuestProjectionClientResult::NotReady;
    return replay_pending();
}

} // namespace kraken::net
